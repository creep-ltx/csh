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

/* ---- the command table -------------------------------------------------- */

#define TSLOTS 1024             /* a power of two; more names than this: slower, not wrong */
#define MAXDIRS 24

static char *pool;              /* the names, each ending in 0 */
static long poollen, poolcap;
static LONG slot[TSLOTS];       /* offset + 1 into pool, 0 = empty */
static int tnames, built, checked;
static struct DateStamp dirdate[MAXDIRS];
static BPTR dirlock[MAXDIRS];
static int ndirs;

static void tadd(const char *name, int n)
{
    ULONG h = hash(name, n) & (TSLOTS - 1);
    int probes = 0;
    if (tnames >= TSLOTS * 3 / 4) return;
    while (slot[h]) {
        const char *s = pool + slot[h] - 1;
        if ((int)strlen(s) == n && ieq(s, name, n)) return;
        h = (h + 1) & (TSLOTS - 1);
        if (++probes > TSLOTS) return;
    }
    if (poollen + n + 1 > poolcap) {
        long cap = poolcap ? poolcap * 2 : 8192;
        char *p = AllocVec(cap, MEMF_ANY);
        if (!p) return;
        if (pool) { memcpy(p, pool, poollen); FreeVec(pool); }
        pool = p; poolcap = cap;
    }
    memcpy(pool + poollen, name, n);
    pool[poollen + n] = 0;
    slot[h] = poollen + 1;
    poollen += n + 1;
    tnames++;
}

static int thas(const char *name, int n)
{
    ULONG h = hash(name, n) & (TSLOTS - 1);
    int probes = 0;
    while (slot[h]) {
        const char *s = pool + slot[h] - 1;
        if ((int)strlen(s) == n && ieq(s, name, n)) return 1;
        h = (h + 1) & (TSLOTS - 1);
        if (++probes > TSLOTS) break;
    }
    return 0;
}

/* The drawers commands come from: the Path's, then C:. The locks are the
 * CLI's (not to be unlocked), C:'s our own. */
static BPTR clock_;

static int gatherdirs(BPTR *d)
{
    struct pathnode *pn;
    int n = 0;
    for (pn = BADDR(csh.cli->cli_CommandDir); pn && n < MAXDIRS - 1; pn = BADDR(pn->next)) d[n++] = pn->lock;
    if (!clock_) {
        APTR win = csh.me->pr_WindowPtr;
        csh.me->pr_WindowPtr = (APTR)-1;
        clock_ = Lock("C:", SHARED_LOCK);
        csh.me->pr_WindowPtr = win;
    }
    if (clock_) d[n++] = clock_;
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
    memset(slot, 0, sizeof slot);
    poollen = 0; tnames = 0;
    ndirs = gatherdirs(d);
    for (i = 0; i < ndirs; i++) {
        dirlock[i] = d[i];
        if (Examine(d[i], fib)) dirdate[i] = fib->fib_Date;
        readdir_(d[i], fib);
    }
    FreeDosObject(DOS_FIB, fib);
    built = 1;
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

void comp_forget(void) { checked = 0; }

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

int comp_kind(const char *word, int n)
{
    static char last[256];
    static int lastkind;
    char name[256];
    int i, k, q = 0;
    for (i = k = 0; i < n && k < (int)sizeof name - 1; i++) {
        if (word[i] == '"') { q = !q; continue; }
        name[k++] = word[i];
    }
    name[k] = 0;
    if (!k) return W_NONE;
    if (!strcmp(name, last)) return lastkind;
    strcpy(last, name);
    if (strchr(name, ':') || strchr(name, '/')) {
        APTR win = csh.me->pr_WindowPtr;
        BPTR l;
        csh.me->pr_WindowPtr = (APTR)-1;
        l = Lock(name, SHARED_LOCK);
        csh.me->pr_WindowPtr = win;
        if (l) UnLock(l);
        return lastkind = l ? W_COMMAND : W_UNKNOWN;
    }
    if (isbuiltin(name, k)) return lastkind = W_BUILTIN;
    if (isalias(name, k)) return lastkind = W_COMMAND;
    if ((i = isresident(name, k))) return lastkind = i == -2 ? W_BUILTIN : W_COMMAND;
    check();
    if (thas(name, k)) return lastkind = W_COMMAND;
    {   /* the current dir comes before the Path */
        APTR win = csh.me->pr_WindowPtr;
        BPTR l;
        csh.me->pr_WindowPtr = (APTR)-1;
        l = Lock(name, SHARED_LOCK);
        csh.me->pr_WindowPtr = win;
        if (l) UnLock(l);
        return lastkind = l ? W_COMMAND : W_UNKNOWN;
    }
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

int comp_complete(char *line, int *len, int *cur, int max)
{
    char word[256], dir[256], add[300];
    int ws = 0, i, q = 0, n, wl, dl = 0, type, shared, o;
    const char *p;
    for (i = 0; i < *cur; i++) {
        char c = line[i];
        if (c == '"') q = !q;
        else if (!q && (c == ' ' || c == '\t' || c == ';' || c == '|' || c == '&' || c == '(' || c == '>' || c == '<')) ws = i + 1;
    }
    for (i = ws, wl = 0; i < *cur && wl < (int)sizeof word - 1; i++) if (line[i] != '"') word[wl++] = line[i];
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
            else { mfiles("", word, wl); mdevices(word, wl); }
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
    /* the word again: the dir typed, the name as the match spells it, its ending */
    {
        int needq = 0, k;
        for (k = 0; k < shared; k++) if (mname[0][k] == ' ') needq = 1;
        for (k = 0; k < dl; k++) if (dir[k] == ' ') needq = 1;
        o = 0;
        if (needq) add[o++] = '"';
        memcpy(add + o, dir, dl); o += dl;
        memcpy(add + o, mname[0], shared); o += shared;
        if (type == 1) add[o++] = '/';
        else if (type == 0 && word[0] == '$' && dl == 2) add[o++] = '}';
        if (needq && type != 1 && type >= 0) add[o++] = '"';
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
