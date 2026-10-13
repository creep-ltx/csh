/* comp.c - what csh knows about names, for Tab and for the line's colours.
 *
 * Commands are csh's built-ins, the aliases, the resident list (the ROM's
 * internal commands too) and the files in the Path's drawers and C:. The
 * files are read once into a table (a hash of names) and read again only
 * when one of those drawers changed (its date, checked at most once a
 * prompt) or the Path did: so the colour of the command word, worked out
 * at every key, never touches a disk.
 *
 * Tab completes the word before the cursor: a command in a command's place
 * (the line's first word, or after ; | && ||), a $variable, else a file or
 * drawer (relative to the dir typed before it) and, for a word with no
 * ':' or '/', also the assigns, volumes and devices. One match: it is put
 * in (a drawer with '/', an assign with ':', anything else with a space);
 * several: as much as they share, or a list under the line. */
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/exall.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "csh.h"
#include "edit.h"
#include "lang.h"

struct pathnode { BPTR next; BPTR lock; };

/* ---- names, case-blind ------------------------------------------------ */

static int lc(int c)
{
    c &= 0xff;
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7)) c += 32;
    return c;
}

static int ieq(const char *a, const char *b, int n)
{
    int i;
    for (i = 0; i < n; i++) if (lc(a[i]) != lc(b[i])) return 0;
    return 1;
}

static int iprefix(const char *s, const char *p, int n)
{
    return (int)strlen(s) >= n && ieq(s, p, n);
}

static ULONG hash(const char *s, int n)
{
    ULONG h = 5381;
    while (n--) h = h * 33 + lc(*s++);
    return h;
}

static int isdirlock(BPTR l, struct FileInfoBlock *fib)
{
    return Examine(l, fib) && fib->fib_DirEntryType > 0;
}

/* ---- the command table -------------------------------------------------- */

#define MAXDIRS 24
#define MAXC    8               /* drawers of a multi-assign C: */

static char *pool;              /* the names, each ending in 0 */
static long poollen, poolcap;
static LONG *slot;              /* offset + 1 into pool, 0 = empty; nslots a power of two */
static int nslots, tnames, built, checked;
static struct DateStamp dirdate[MAXDIRS];
static BPTR dirlock[MAXDIRS];
static int ndirs;
static void tplforget(void);

static void tput(long off)      /* a name already in the pool, into the slots */
{
    const char *name = pool + off;
    ULONG h = hash(name, strlen(name)) & (nslots - 1);
    while (slot[h]) h = (h + 1) & (nslots - 1);
    slot[h] = off + 1;
}

/* Twice the slots, the names put in again: never full (no command left red). */
static int tgrow(void)
{
    int n = nslots ? nslots * 2 : 1024;
    LONG *s = AllocVec(n * sizeof *s, MEMF_ANY | MEMF_CLEAR);
    long o;
    if (!s) return 0;
    if (slot) FreeVec(slot);
    slot = s; nslots = n;
    for (o = 0; o < poollen; o += strlen(pool + o) + 1) tput(o);
    return 1;
}

static int thas(const char *name, int n)
{
    ULONG h;
    if (!nslots) return 0;
    h = hash(name, n) & (nslots - 1);
    while (slot[h]) {
        const char *s = pool + slot[h] - 1;
        if ((int)strlen(s) == n && ieq(s, name, n)) return 1;
        h = (h + 1) & (nslots - 1);
    }
    return 0;
}

static void tadd(const char *name, int n)
{
    if (thas(name, n)) return;
    if ((tnames + 1) * 4 >= nslots * 3 && !tgrow()) return;
    if (poollen + n + 1 > poolcap) {
        long cap = poolcap ? poolcap * 2 : 8192;
        char *p = AllocVec(cap, MEMF_ANY);
        if (!p) return;
        if (pool) { memcpy(p, pool, poollen); FreeVec(pool); }
        pool = p; poolcap = cap;
    }
    memcpy(pool + poollen, name, n);
    pool[poollen + n] = 0;
    tput(poollen);
    poollen += n + 1;
    tnames++;
}

/* The drawers commands come from: the Path's, then every drawer of C:
 * (a multi-assign has several). The Path's locks are the CLI's (not ours
 * to unlock); C:'s are copies of our own, taken again only when C: is
 * assigned elsewhere (SameLock), so a check costs no disk. */
static BPTR cdir[MAXC];
static int ncdir;

static void cdirs(void)
{
    struct DevProc *dp = NULL;
    BPTR now[MAXC];
    int n = 0, i, same;
    APTR win = csh.me->pr_WindowPtr;
    csh.me->pr_WindowPtr = (APTR)-1;
    while (n < MAXC && (dp = GetDeviceProc("C:", dp))) {
        if (dp->dvp_Lock) now[n++] = dp->dvp_Lock;
        if (!(dp->dvp_Flags & DVPF_ASSIGN)) break;
    }
    if (dp) FreeDeviceProc(dp);
    csh.me->pr_WindowPtr = win;
    same = n == ncdir;
    for (i = 0; i < n && same; i++) same = SameLock(now[i], cdir[i]) == LOCK_SAME;
    if (same) return;
    for (i = 0; i < ncdir; i++) UnLock(cdir[i]);
    for (ncdir = i = 0; i < n; i++) if ((cdir[ncdir] = DupLock(now[i]))) ncdir++;
}

static int gatherdirs(BPTR *d)
{
    struct pathnode *pn;
    int n = 0, i;
    for (pn = BADDR(csh.cli->cli_CommandDir); pn && n < MAXDIRS - MAXC; pn = BADDR(pn->next)) d[n++] = pn->lock;
    cdirs();
    for (i = 0; i < ncdir; i++) d[n++] = cdir[i];
    return n;
}

static void readdir_(BPTR dir, struct FileInfoBlock *fib)
{
    if (!Examine(dir, fib)) return;
    while (ExNext(dir, fib)) {
        int n = strlen(fib->fib_FileName);
        if (fib->fib_DirEntryType > 0) continue;
        if (n > 5 && ieq(fib->fib_FileName + n - 5, ".info", 5)) continue;
        tadd(fib->fib_FileName, n);
    }
}

static void build(void)
{
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    BPTR d[MAXDIRS];
    int i;
    if (!fib) return;
    if (slot) memset(slot, 0, nslots * sizeof *slot);
    poollen = 0; tnames = 0;
    ndirs = gatherdirs(d);
    for (i = 0; i < ndirs; i++) {
        dirlock[i] = d[i];
        if (Examine(d[i], fib)) dirdate[i] = fib->fib_Date;
        readdir_(d[i], fib);
    }
    FreeDosObject(DOS_FIB, fib);
    built = 1;
    tplforget();                /* a command may have changed: its keywords read again */
}

/* At most once a prompt: did the Path or a drawer of it change? */
static void check(void)
{
    struct FileInfoBlock *fib;
    BPTR d[MAXDIRS];
    int n, i, stale = 0;
    if (checked && built) return;
    checked = 1;
    if (!built) { build(); return; }
    n = gatherdirs(d);
    if (n != ndirs) stale = 1;
    if (!stale && (fib = AllocDosObject(DOS_FIB, NULL))) {
        for (i = 0; i < n && !stale; i++) {
            if (d[i] != dirlock[i]) stale = 1;
            else if (Examine(d[i], fib) && CompareDates(&fib->fib_Date, &dirdate[i])) stale = 1;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    if (stale) build();
}


/* ---- what a word is ------------------------------------------------------ */

extern const char *const builtin_names[];

static int isbuiltin(const char *w, int n)
{
    int i;
    for (i = 0; builtin_names[i]; i++)
        if ((int)strlen(builtin_names[i]) == n && ieq(builtin_names[i], w, n)) return 1;
    return 0;
}

/* -2 internal, 1 normal resident, 0 none */
static int isresident(const char *w, int n)
{
    struct DosInfo *di = BADDR(((struct RootNode *)DOSBase->dl_Root)->rn_Info);
    struct Segment *s;
    int r = 0;
    Forbid();
    for (s = BADDR(*(BPTR *)((UBYTE *)di + 0x10)); s; s = BADDR(s->seg_Next)) {
        if (s->seg_Name[0] != n || !ieq((char *)s->seg_Name + 1, w, n)) continue;
        if (s->seg_UC >= 0) { r = 1; break; }
        if (s->seg_UC == CMD_INTERNAL) r = -2;
    }
    Permit();
    return r;
}

static int isalias(const char *w, int n)
{
    char name[64];
    if (n >= (int)sizeof name) return 0;
    memcpy(name, w, n); name[n] = 0;
    return FindVar(name, LV_ALIAS) != NULL;
}

/* A name with a device part whose handler has not started (CD0:, a
 * floppy drive's DF1:): asking about it would start it, at a key. */
static int sleeping(const char *name)
{
    const char *c = strchr(name, ':');
    struct DosList *dl;
    char dev[32];
    int n = c ? c - name : 0, r = 0;
    if (!c || n == 0 || n >= (int)sizeof dev) return 0;
    memcpy(dev, name, n); dev[n] = 0;
    dl = LockDosList(LDF_READ | LDF_DEVICES);
    if ((dl = FindDosEntry(dl, dev, LDF_DEVICES)) && !dl->dol_Task) r = 1;
    UnLockDosList(LDF_READ | LDF_DEVICES);
    return r;
}

/* A few words remembered (a line has several command words: a | b), so a
 * key that leaves them as they were asks nothing again; forgotten at the
 * next prompt (comp_forget). */
#define NKIND 6
static struct { char name[256]; int kind; } kinds[NKIND];
static int nkinds, kindnext;

static int kindof(const char *name)
{
    int i;
    for (i = 0; i < nkinds; i++) if (!strcmp(kinds[i].name, name)) return kinds[i].kind;
    return -1;
}

static int keep(const char *name, int kind)
{
    int i = kindnext;
    kindnext = (kindnext + 1) % NKIND;
    if (nkinds < NKIND) nkinds++;
    strcpy(kinds[i].name, name);
    kinds[i].kind = kind;
    return kind;
}

static int lockable(const char *name)
{
    APTR win = csh.me->pr_WindowPtr;
    BPTR l;
    csh.me->pr_WindowPtr = (APTR)-1;
    l = Lock((STRPTR)name, SHARED_LOCK);
    csh.me->pr_WindowPtr = win;
    if (l) UnLock(l);
    return l != 0;
}

void comp_forget(void) { checked = 0; nkinds = 0; }

/* What a command word is. cheap: the word is still being typed - nothing
 * that needs a disk (W_NONE then, not remembered). */
int comp_kind(const char *word, int n, int cheap)
{
    char name[256];
    int i, k, q = 0;
    for (i = k = 0; i < n && k < (int)sizeof name - 1; i++) {
        if (word[i] == '"' || word[i] == '\'') { q = !q; continue; }
        name[k++] = word[i];
    }
    name[k] = 0;
    if (!k) return W_NONE;
    if ((i = kindof(name)) >= 0) return i;
    if (strchr(name, ':') || strchr(name, '/')) {
        if (cheap || sleeping(name)) return W_NONE;
        return keep(name, lockable(name) ? W_COMMAND : W_UNKNOWN);
    }
    if (isbuiltin(name, k)) return keep(name, W_BUILTIN);
    if (isalias(name, k)) return keep(name, W_COMMAND);
    if ((i = isresident(name, k))) return keep(name, i == -2 ? W_BUILTIN : W_COMMAND);
    check();
    if (thas(name, k)) return keep(name, W_COMMAND);
    /* the current dir comes before the Path */
    if (cheap) return W_NONE;
    return keep(name, lockable(name) ? W_COMMAND : W_UNKNOWN);
}

/* ---- matches ------------------------------------------------------------- */

#define MAXM 400
static char *mpool;
static int mlen, mcap = 16384;
static char *mname[MAXM];
static char mtype[MAXM];          /* 0 file/command, 1 drawer, 2 assign/volume (':' in it) */
static int nm, listed;

static void madd(const char *s, int n, int type)
{
    int i;
    if (nm >= MAXM) return;
    if (!mpool && !(mpool = AllocVec(mcap, MEMF_ANY))) return;
    if (mlen + n + 1 > mcap) return;
    for (i = 0; i < nm; i++) if ((int)strlen(mname[i]) == n && ieq(mname[i], s, n)) return;
    memcpy(mpool + mlen, s, n);
    mpool[mlen + n] = 0;
    mname[nm] = mpool + mlen;
    mtype[nm++] = type;
    mlen += n + 1;
}

static void mcommands(const char *p, int n)
{
    struct DosInfo *di = BADDR(((struct RootNode *)DOSBase->dl_Root)->rn_Info);
    struct LocalVar *lv;
    struct Segment *s;
    long o;
    int i;
    for (i = 0; builtin_names[i]; i++) if (iprefix(builtin_names[i], p, n)) madd(builtin_names[i], strlen(builtin_names[i]), 0);
    for (lv = (struct LocalVar *)csh.me->pr_LocalVars.mlh_Head; lv->lv_Node.ln_Succ;
         lv = (struct LocalVar *)lv->lv_Node.ln_Succ)
        if ((lv->lv_Node.ln_Type & 0x7f) == LV_ALIAS && iprefix(lv->lv_Node.ln_Name, p, n))
            madd(lv->lv_Node.ln_Name, strlen(lv->lv_Node.ln_Name), 0);
    {   /* the resident list: names copied under Forbid, nothing else done there */
        static char rn[64][32];
        int nr = 0;
        Forbid();
        for (s = BADDR(*(BPTR *)((UBYTE *)di + 0x10)); s && nr < 64; s = BADDR(s->seg_Next)) {
            int l = s->seg_Name[0];
            if ((s->seg_UC >= 0 || s->seg_UC == CMD_INTERNAL) && l < 32 && l >= n
                && ieq((char *)s->seg_Name + 1, p, n) && s->seg_Name[1] != '.' && s->seg_Name[1] != '(') {
                memcpy(rn[nr], s->seg_Name + 1, l); rn[nr][l] = 0; nr++;
            }
        }
        Permit();
        for (i = 0; i < nr; i++) madd(rn[i], strlen(rn[i]), 0);
    }
    check();
    for (o = 0; o < poollen; o += strlen(pool + o) + 1)
        if (iprefix(pool + o, p, n)) madd(pool + o, strlen(pool + o), 0);
}

static void mvars(const char *p, int n)
{
    struct LocalVar *lv;
    struct FileInfoBlock *fib;
    BPTR l;
    for (lv = (struct LocalVar *)csh.me->pr_LocalVars.mlh_Head; lv->lv_Node.ln_Succ;
         lv = (struct LocalVar *)lv->lv_Node.ln_Succ)
        if ((lv->lv_Node.ln_Type & 0x7f) == LV_VAR && iprefix(lv->lv_Node.ln_Name, p, n))
            madd(lv->lv_Node.ln_Name, strlen(lv->lv_Node.ln_Name), 0);
    if ((l = Lock("ENV:", SHARED_LOCK))) {
        if ((fib = AllocDosObject(DOS_FIB, NULL))) {
            if (Examine(l, fib))
                while (ExNext(l, fib))
                    if (fib->fib_DirEntryType < 0 && iprefix(fib->fib_FileName, p, n))
                        madd(fib->fib_FileName, strlen(fib->fib_FileName), 0);
            FreeDosObject(DOS_FIB, fib);
        }
        UnLock(l);
    }
}

static void mfiles(const char *dir, const char *p, int n)
{
    struct FileInfoBlock *fib;
    APTR win = csh.me->pr_WindowPtr;
    BPTR l;
    int dotinfo = n && p[0] == '.';
    csh.me->pr_WindowPtr = (APTR)-1;
    l = Lock((STRPTR)dir, SHARED_LOCK);
    csh.me->pr_WindowPtr = win;
    if (!l) return;
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (Examine(l, fib) && fib->fib_DirEntryType > 0)
            while (ExNext(l, fib)) {
                char *f = fib->fib_FileName;
                int fl = strlen(f);
                if (!iprefix(f, p, n)) continue;
                if (!dotinfo && fl > 5 && ieq(f + fl - 5, ".info", 5) && !(n > 0 && ieq(p + n - 1, ".", 1))) continue;
                madd(f, fl, fib->fib_DirEntryType > 0 ? 1 : 0);
            }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
}

static void mdevices(const char *p, int n)
{
    static char dn[64][34];
    struct DosList *dl;
    int nd = 0, i;
    dl = LockDosList(LDF_READ | LDF_ASSIGNS | LDF_VOLUMES | LDF_DEVICES);
    while ((dl = NextDosEntry(dl, LDF_ASSIGNS | LDF_VOLUMES | LDF_DEVICES)) && nd < 64) {
        UBYTE *b = BADDR(dl->dol_Name);
        int l = b[0];
        if (dl->dol_Type == DLT_DEVICE && !dl->dol_Task) continue;   /* no handler: CON:, AUX: ... */
        if (l < 32 && l >= n && ieq((char *)b + 1, p, n)) {
            memcpy(dn[nd], b + 1, l); dn[nd][l] = ':'; dn[nd][l + 1] = 0; nd++;
        }
    }
    UnLockDosList(LDF_READ | LDF_ASSIGNS | LDF_VOLUMES | LDF_DEVICES);
    for (i = 0; i < nd; i++) madd(dn[i], strlen(dn[i]), 2);
}

static int icmp(const char *a, const char *b)
{
    while (*a && lc(*a) == lc(*b)) a++, b++;
    return lc(*a) - lc(*b);
}

static void sortm(void)
{
    int i, j;
    for (i = 1; i < nm; i++) {
        char *s = mname[i], t = mtype[i];
        for (j = i; j > 0 && icmp(mname[j - 1], s) > 0; j--) { mname[j] = mname[j - 1]; mtype[j] = mtype[j - 1]; }
        mname[j] = s; mtype[j] = t;
    }
}

static void showlist(void)
{
    int w = 0, i, cols, rows, r, c, shown = nm > 200 ? 200 : nm;
    for (i = 0; i < shown; i++) { int l = strlen(mname[i]) + (mtype[i] == 1); if (l > w) w = l; }
    w += 2;
    cols = (term_cols - 1) / w;
    if (cols < 1) cols = 1;
    rows = (shown + cols - 1) / cols;
    term_outs("\n");
    for (r = 0; r < rows; r++) {
        for (c = 0; c < cols; c++) {
            int k = c * rows + r, l;
            if (k >= shown) break;
            term_outs(mname[k]);
            l = strlen(mname[k]);
            if (mtype[k] == 1) { term_outs("/"); l++; }
            if (c + 1 < cols && (c + 1) * rows + r < shown) while (l++ < w) term_out(" ", 1);
        }
        term_outs("\n");
    }
    if (nm > shown) { term_outs("("); term_num(nm - shown); term_outs(" more)\n"); }
    listed = 1;
}

int comp_listing(void)
{
    int l = listed;
    listed = 0;
    return l;
}


/* ---- a command's keywords, for Tab after it -------------------------------- */

/* The ROM's internal commands live in Kickstart: their templates as the
 * AmigaDOS manual gives them. */
static const char *const internaltpl[][2] = {
    { "CD", "DIR" }, { "Path", "PATH/M,ADD/S,SHOW/S,RESET/S,QUIET/S,REMOVE/S" },
    { "Echo", "STRING/M,NOLINE/S,FIRST/K/N,LEN/K/N,TO/K" },
    { "If", "NOT/S,WARN/S,ERROR/S,FAIL/S,EQ/K,GT/K,GE/K,VAL/S,EXISTS/K" },
    { "Resident", "NAME,FILE,REMOVE/S,ADD/S,REPLACE/S,PURE=FORCE/S,SYSTEM/S" },
    { "Skip", "LABEL,BACK/S" }, { "Stack", "SIZE/N" }, { "NewShell", "WINDOW,FROM/K" },
    { "NewCLI", "WINDOW,FROM/K" }, { "Execute", "FILE/A" }, { "Failat", "RCLIM/N" },
    { "Quit", "RC/N" }, { "Run", "COMMAND/F" }, { "Ask", "PROMPT/A" },
    { NULL, NULL }
};

#define NTPL 40
static struct { char name[32]; char *words; } tpl[NTPL];   /* words: names, 0-separated, "" ends */
static int tplnext;

static void tplforget(void)
{
    int i;
    for (i = 0; i < NTPL; i++) { if (tpl[i].words) os_free(tpl[i].words); tpl[i].words = NULL; }
}

static int tplpart(const char *s, int n)        /* KEY[=ALIAS]...[/X]... : 1 = with a /X */
{
    int i = 0, mod = 0;
    if (n < 1 || !((s[0] >= 'A' && s[0] <= 'Z') || (s[0] >= 'a' && s[0] <= 'z'))) return -1;
    while (i < n && s[i] != '/') {
        char c = s[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '=')) return -1;
        i++;
    }
    while (i < n) {
        char m;
        if (s[i] != '/' || i + 1 >= n) return -1;
        m = s[i + 1] | 32;
        if (!strchr("aksnmft", m)) return -1;
        mod = 1;
        i += 2;
    }
    return mod;
}

/* How good a template the text is: the parts that read as ReadArgs parts
 * from its start on (stopping at the first that does not), if one at least
 * has a /X; *len gets how much of the text they are. */
static int tplscore(const char *s, int n, int *len)
{
    int parts = 0, mods = 0, start = 0, i;
    *len = 0;
    for (i = 0; i <= n; i++) {
        if (i == n || s[i] == ',') {
            int r = tplpart(s + start, i - start);
            if (r < 0) break;
            mods += r; parts++;
            *len = i;
            start = i + 1;
        }
    }
    return mods ? parts : 0;
}

/* The keywords worth offering: /S switches and /K keys (not the places of
 * a file or a number), by their longest name. */
static char *tplwords(const char *t)
{
    char *w = os_alloc(strlen(t) + 2), *o = w;
    const char *p = t;
    if (!w) return NULL;
    while (*p) {
        const char *e = strchr(p, ','), *eq, *sl;
        int n = e ? e - p : (int)strlen(p);
        int k;
        for (eq = p; eq < p + n && *eq != '=' && *eq != '/'; eq++) ;
        sl = memchr(p, '/', n);
        k = 0;
        if (sl) {
            const char *m;
            for (m = sl; m < p + n; m++) if (m[0] == '/' && ((m[1] | 32) == 's' || (m[1] | 32) == 'k')) k = 1;
        }
        if (k) {        /* of NAME=OTHER, the longer (PAT, not P; LFORMATQUOTE, not LFQ) */
            const char *best = p, *q = p, *end = sl ? sl : p + n;
            int bl = 0;
            while (q < end) {
                const char *e2 = q;
                while (e2 < end && *e2 != '=') e2++;
                if (e2 - q > bl) { bl = e2 - q; best = q; }
                q = e2 < end ? e2 + 1 : end;
            }
            memcpy(o, best, bl); o += bl; *o++ = 0;
        }
        p += n;
        if (*p == ',') p++;
    }
    *o = 0;
    return w;
}

/* The best template in a program file (the longest run of ReadArgs parts). */
static char *tplfromfile(BPTR lock)
{
    BPTR fh = OpenFromLock(lock);
    static char buf[4096];
    char best[320], run[320];
    int bestscore = 0, rl = 0;
    LONG n, total = 0;
    if (!fh) { UnLock(lock); return NULL; }
    best[0] = 0;
    /* the template sits in the code or data, near the start in the programs
     * looked at: 160 KB read at most (a floppy reads that in a few seconds) */
    while (total < 160000 && (n = Read(fh, buf, sizeof buf)) > 0) {
        LONG i;
        total += n;
        for (i = 0; i < n; i++) {
            char c = buf[i];
            if (((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '/' || c == ',' || c == '=' || c == '_' || c == '-') && rl < (int)sizeof run - 1) {
                run[rl++] = c;
                continue;
            }
            if (rl >= 3) {
                /* a stray byte or two may sit before it ("3DIR/M,..." in 3.2's List) */
                int k;
                for (k = 0; k < 4 && k < rl; k++) {
                    int len, sc = tplscore(run + k, rl - k, &len);
                    if (sc > bestscore) { bestscore = sc; memcpy(best, run + k, len); best[len] = 0; }
                }
            }
            rl = 0;
        }
    }
    Close(fh);
    return bestscore ? tplwords(best) : NULL;
}

/* A command's file: as typed with a drawer, else the current dir, the
 * Path, C: (the search of run.c); a lock, or 0. */
static BPTR cmdfile(const char *name)
{
    struct FileInfoBlock *fib;
    struct pathnode *pn;
    APTR win = csh.me->pr_WindowPtr;
    BPTR l = 0;
    char cn[64];
    if (!(fib = AllocDosObject(DOS_FIB, NULL))) return 0;
    csh.me->pr_WindowPtr = (APTR)-1;
    if ((l = Lock((STRPTR)name, SHARED_LOCK)) && !isdirlock(l, fib)) goto done;
    if (l) { UnLock(l); l = 0; }
    if (strchr(name, ':') || strchr(name, '/')) goto done;
    for (pn = BADDR(csh.cli->cli_CommandDir); pn; pn = BADDR(pn->next)) {
        BPTR old = CurrentDir(pn->lock);
        l = Lock((STRPTR)name, SHARED_LOCK);
        CurrentDir(old);
        if (l && !isdirlock(l, fib)) goto done;
        if (l) { UnLock(l); l = 0; }
    }
    if (strlen(name) < sizeof cn - 3) {
        strcpy(cn, "C:"); strcat(cn, name);
        if ((l = Lock(cn, SHARED_LOCK)) && isdirlock(l, fib)) { UnLock(l); l = 0; }
    }
done:
    csh.me->pr_WindowPtr = win;
    FreeDosObject(DOS_FIB, fib);
    return l;
}

static const char *cmdwords(const char *name)
{
    int i;
    BPTR l;
    char *w = NULL;
    for (i = 0; i < NTPL; i++) if (tpl[i].words && ieq(tpl[i].name, name, strlen(name) + 1)) return tpl[i].words;
    for (i = 0; internaltpl[i][0]; i++)
        if ((int)strlen(internaltpl[i][0]) == (int)strlen(name) && ieq(internaltpl[i][0], name, strlen(name)) && isresident(name, strlen(name)) == -2) {
            w = tplwords(internaltpl[i][1]);
            break;
        }
    if (!w && (l = cmdfile(name))) w = tplfromfile(l);
    if (!w && !(w = os_alloc(2))) return NULL;          /* none: remembered too */
    if (strlen(name) >= sizeof tpl[0].name) { return w; }
    i = tplnext; tplnext = (tplnext + 1) % NTPL;
    if (tpl[i].words) os_free(tpl[i].words);
    strcpy(tpl[i].name, name);
    tpl[i].words = w;
    return w;
}

/* The command whose argument the word at ws is (the first word since the
 * last ; | & or the line's start). */
static int cmdof(const char *line, int ws, char *out, int size)
{
    int s = ws, i, n;
    while (s > 0 && line[s - 1] != ';' && line[s - 1] != '|' && line[s - 1] != '&' && line[s - 1] != '(') s--;
    while (s < ws && (line[s] == ' ' || line[s] == '\t')) s++;
    for (i = s; i < ws && line[i] != ' ' && line[i] != '\t'; i++) ;
    n = i - s;
    if (n <= 0 || n >= size) return 0;
    memcpy(out, line + s, n); out[n] = 0;
    if (out[0] == '"') return 0;
    return 1;
}

/* Is the word starting at ws in a command's place? */
static int commandplace(const char *line, int ws)
{
    int i = ws;
    while (i > 0 && (line[i - 1] == ' ' || line[i - 1] == '\t')) i--;
    if (i == 0) return 1;
    if (line[i - 1] == ';' || line[i - 1] == '|' || line[i - 1] == '&' || line[i - 1] == '(') return 1;
    {
        static const char *const kw[] = { "then", "do", "else", "elif", "if", "while", "until", "!", NULL };
        int k, e = i, s;
        for (s = e; s > 0 && line[s - 1] != ' ' && line[s - 1] != '\t'; s--) ;
        for (k = 0; kw[k]; k++)
            if ((int)strlen(kw[k]) == e - s && !memcmp(line + s, kw[k], e - s)) return commandplace(line, s);
    }
    return 0;
}

/* Characters that make a name need quotes on a csh line. */
static int special(char c)
{
    return c == ' ' || c == '\t' || strchr(";|&()<>$`'\"*?[]{}#~\\", c) != NULL;
}

int comp_complete(char *line, int *len, int *cur, int max)
{
    char word[256], dir[256], add[LINE_MAX];
    int ws = 0, i, q = 0, n, wl, dl = 0, type, shared, o;
    const char *p;
    for (i = 0; i < *cur; i++) {
        char c = line[i];
        if (q) { if (c == q) q = 0; }
        else if (c == '"' || c == '\'') q = c;
        else if (c == ' ' || c == '\t' || c == ';' || c == '|' || c == '&' || c == '(' || c == '>' || c == '<') ws = i + 1;
    }
    for (i = ws, wl = 0; i < *cur; i++) {
        if (line[i] == '"' || line[i] == '\'') continue;
        if (wl >= (int)sizeof word - 1) { term_outs("\a"); return 0; }    /* too long to complete */
        word[wl++] = line[i];
    }
    word[wl] = 0;
    nm = mlen = 0;
    if (word[0] == '$') {
        int b = word[1] == '{';
        mvars(word + 1 + b, wl - 1 - b);
        dl = 1 + b;
        memcpy(dir, word, dl); dir[dl] = 0;
    } else {
        const char *sep = NULL;
        for (p = word; *p; p++) if (*p == ':' || *p == '/') sep = p;
        if (sep) {
            dl = sep - word + 1;
            memcpy(dir, word, dl); dir[dl] = 0;
            mfiles(dir, word + dl, wl - dl);
        } else {
            dir[0] = 0;
            if (commandplace(line, ws)) mcommands(word, wl);
            else {
                char cmd[64];
                mfiles("", word, wl);
                mdevices(word, wl);
                if (cmdof(line, ws, cmd, sizeof cmd)) {    /* the command's own keywords */
                    const char *k = cmdwords(cmd);
                    for (; k && *k; k += strlen(k) + 1)
                        if (iprefix(k, word, wl)) madd(k, strlen(k), 0);
                }
            }
            if (commandplace(line, ws)) {
                /* drawers here too: a drawer typed alone is a CD */
                int before = nm, k;
                mfiles("", word, wl);
                for (k = before; k < nm; k++) if (mtype[k] != 1) { mtype[k] = 9; }
                for (k = before, o = before; k < nm; k++) if (mtype[k] == 1) { mname[o] = mname[k]; mtype[o++] = 1; }
                nm = o;
                mdevices(word, wl);
            }
        }
    }
    if (!nm) { term_outs("\a"); return 0; }
    sortm();
    n = wl - dl;                       /* what of the name is typed */
    if (nm == 1) {
        shared = strlen(mname[0]);
        type = mtype[0];
    } else {
        shared = strlen(mname[0]);
        for (i = 1; i < nm; i++) {
            int k = 0;
            while (k < shared && mname[i][k] && lc(mname[i][k]) == lc(mname[0][k])) k++;
            shared = k;
        }
        type = -1;
        if (shared <= n) { showlist(); return 1; }
    }
    /* the word again: the dir typed, the name as the match spells it, its
     * ending; in quotes when a character of it means something to csh
     * ('...' when one means something inside "..." too) */
    {
        int needq = 0, k;
        char qc = '"';
        if (word[0] != '$') {
            for (k = 0; k < shared; k++) if (special(mname[0][k])) needq = 1;
            for (k = 0; k < dl; k++) if (special(dir[k])) needq = 1;
            for (k = 0; k < shared && needq; k++) if (strchr("$`\"\\", mname[0][k])) qc = '\'';
            for (k = 0; k < dl && needq; k++) if (strchr("$`\"\\", dir[k])) qc = '\'';
        }
        if (dl + shared + 4 >= (int)sizeof add) { term_outs("\a"); return 0; }
        o = 0;
        if (needq) add[o++] = qc;
        memcpy(add + o, dir, dl); o += dl;
        memcpy(add + o, mname[0], shared); o += shared;
        if (type == 1) add[o++] = '/';
        else if (type == 0 && word[0] == '$' && dl == 2) add[o++] = '}';
        if (needq && type != 1 && type >= 0) add[o++] = qc;
        if (type == 0) add[o++] = ' ';
        add[o] = 0;
    }
    if (*len - (*cur - ws) + o >= max) return 0;
    memmove(line + ws + o, line + *cur, *len - *cur);
    memcpy(line + ws, add, o);
    *len += o - (*cur - ws);
    *cur = ws + o;
    line[*len] = 0;
    return 1;
}

/* csh ends: the tables, the matches, the keywords freed; C:'s locks back. */
void comp_end(void)
{
    int i;
    if (pool) FreeVec(pool);
    if (slot) FreeVec(slot);
    if (mpool) FreeVec(mpool);
    pool = NULL; slot = NULL; mpool = NULL;
    poollen = poolcap = 0; nslots = tnames = 0; built = 0;
    for (i = 0; i < ncdir; i++) UnLock(cdir[i]);
    ncdir = 0;
    tplforget();
}
