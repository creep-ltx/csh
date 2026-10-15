/* run.c - running one line the way the 3.2 ROM Shell does (docs/design.md
 * "Running a command"; tested facts in Knowledge/amiga/writing-a-shell.md):
 *  - alias, $variables, backquotes, then the line split up (line.c);
 *  - the search: csh's built-ins, the resident list (the ROM's internal
 *    commands too), the current dir, the Path, then C:;
 *  - a drawer typed alone is a CD; an s-bit file is a script (Execute), or
 *    ARexx when it starts with slash-star (RX);
 *  - each command runs on csh's own process with RunCommand, so it sees a
 *    proper CLI: the ROM's If, Skip and Execute then move cli_CurrentInput
 *    and csh's loop simply reads on;
 *  - a pipe is still handed to the ROM Shell (step 5 brings csh's own). */
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <stdio.h>
#include <string.h>
#include "csh.h"
#include "edit.h"
#include "lang.h"

struct pathnode { BPTR next; BPTR lock; };   /* cli_CommandDir's list */

enum { F_NONE, F_RES, F_FILE, F_DIR };

static int getvar(const char *name, char *buf, int size)
{
    return GetVar((STRPTR)name, buf, size, 0);
}

static int getalias(const char *name, char *buf, int size)
{
    return GetVar((STRPTR)name, buf, size, LV_ALIAS | GVF_LOCAL_ONLY);
}

static int noname(const char *s)
{
    return strchr(s, ':') == NULL && strchr(s, '/') == NULL;
}

/* A number as text, into b (at least 12 bytes); returns its length. */
int ltoa10(LONG n, char *b)
{
    char t[12];
    int i = 0, o = 0;
    ULONG u = n < 0 ? -(ULONG)n : (ULONG)n;
    if (n < 0) b[o++] = '-';
    do { t[i++] = '0' + u % 10; u /= 10; } while (u);
    while (i) b[o++] = t[--i];
    b[o] = 0;
    return o;
}

/* $RC and $Result2 are only rewritten when they change: SetVar frees and
 * allocates each time, and most commands return 0 after a 0. */
void set_rc(LONG rc, LONG r2)
{
    static LONG lastrc = -12345, lastr2 = -12345;
    char b[12];
    if (rc == 0) r2 = 0;
    csh.cli->cli_ReturnCode = rc;
    csh.cli->cli_Result2 = r2;
    csh.rc = rc;
    if (rc != lastrc || !FindVar("RC", LV_VAR)) {
        ltoa10(rc, b); SetVar("RC", b, -1, LV_VAR | GVF_LOCAL_ONLY); lastrc = rc;
    }
    if (r2 != lastr2 || !FindVar("Result2", LV_VAR)) {
        ltoa10(r2, b); SetVar("Result2", b, -1, LV_VAR | GVF_LOCAL_ONLY); lastr2 = r2;
    }
}

/* The resident list: a normal entry (seg_UC >= 0, use count raised) or an
 * internal command (CMD_INTERNAL). Disabled and system entries are skipped. */
static int bstreq(const UBYTE *b, const char *s, int n)
{
    int i;
    if (b[0] != n) return 0;
    for (i = 0; i < n; i++) {
        UBYTE x = b[i + 1], y = s[i];
        if (x != y) {
            if (x >= 'A' && x <= 'Z') x += 32; else if (x >= 0xC0 && x <= 0xDE && x != 0xD7) x += 32;
            if (y >= 'A' && y <= 'Z') y += 32; else if (y >= 0xC0 && y <= 0xDE && y != 0xD7) y += 32;
            if (x != y) return 0;
        }
    }
    return 1;
}

/* One walk over the resident list (its header is at DosInfo+$10, not where
 * the NDK says): a normal entry wins over an internal one of the same name,
 * as FindSegment(name, 0, FALSE) first would. */
static struct Segment *resident(const char *name)
{
    struct DosInfo *di = BADDR(((struct RootNode *)DOSBase->dl_Root)->rn_Info);
    struct Segment *s, *internal = NULL, *normal = NULL;
    int n = strlen(name);
    if (n > 255) return NULL;
    Forbid();
    for (s = BADDR(*(BPTR *)((UBYTE *)di + 0x10)); s; s = BADDR(s->seg_Next)) {
        if (!bstreq(s->seg_Name, name, n)) continue;
        if (s->seg_UC >= 0) { normal = s; break; }
        if (s->seg_UC == CMD_INTERNAL && !internal) internal = s;
    }
    if (normal) normal->seg_UC++;
    Permit();
    return normal ? normal : internal;
}

static void resident_done(struct Segment *s)
{
    Forbid();
    if (s->seg_UC > 0) s->seg_UC--;
    Permit();
}

static BPTR lockin(BPTR dir, const char *name)
{
    BPTR old = CurrentDir(dir), l = Lock((STRPTR)name, SHARED_LOCK);
    CurrentDir(old);
    return l;
}

static int isdir(BPTR l, struct FileInfoBlock *fib)
{
    return Examine(l, fib) && fib->fib_DirEntryType > 0;
}

/* Finds a command. F_RES: *seg is the resident entry; F_FILE / F_DIR:
 * *lock is the file or drawer, fib holds its Examine, and the file loads
 * as *lname from the drawer *where (0: the current dir) - no full path
 * name is made, that walks every parent drawer. Requesters stay off. */
static int find(const char *name, int alone, struct Segment **seg, BPTR *lock,
                struct FileInfoBlock *fib, BPTR *where, const char **lname)
{
    static char cname[260];
    APTR win = csh.me->pr_WindowPtr;
    struct pathnode *pn;
    BPTR l;
    int r = F_NONE;
    *where = 0; *lname = name;
    if (noname(name) && (*seg = resident(name))) return F_RES;
    csh.me->pr_WindowPtr = (APTR)-1;
    if ((l = Lock((STRPTR)name, SHARED_LOCK))) {
        if (!isdir(l, fib)) { *lock = l; r = F_FILE; goto done; }
        if (alone || !noname(name)) { *lock = l; r = F_DIR; goto done; }
        UnLock(l);
    }
    if (!noname(name)) goto done;
    for (pn = BADDR(csh.cli->cli_CommandDir); pn; pn = BADDR(pn->next)) {
        if ((l = lockin(pn->lock, name))) {
            if (!isdir(l, fib)) { *lock = l; *where = pn->lock; r = F_FILE; goto done; }
            UnLock(l);
        }
    }
    strcpy(cname, "C:");
    strncat(cname, name, sizeof cname - 3);
    if ((l = Lock(cname, SHARED_LOCK))) {
        if (!isdir(l, fib)) { *lock = l; *lname = cname; r = F_FILE; goto done; }
        UnLock(l);
    }
done:
    csh.me->pr_WindowPtr = win;
    return r;
}

static BPTR openredir(const char *name, LONG mode, int append)
{
    BPTR fh;
    if (append) {
        if ((fh = Open((STRPTR)name, MODE_READWRITE))) { Seek(fh, 0, OFFSET_END); return fh; }
    }
    return Open((STRPTR)name, mode);
}

/* _CMDLINE: while a program runs, the line that started it (name and
 * arguments, a pipe's whole line), in a local variable of csh's process -
 * CTerm's saved layouts read it to start the program again. Only for a
 * line typed at the prompt (in scripts it would cost time and say nothing
 * CTerm can use); the outermost run sets it (a script started at the
 * prompt keeps its own line, its commands leave it), the end takes it
 * away. NULL: hold the place but set nothing (a line cut short). */
static int cmdline_depth, cmdline_set;

/* cmd and its args; args NULL: cmd is a whole line already */
void run_cmdline_begin(const char *cmd, const char *args)
{
    static char b[LINE_MAX];
    int n;
    if (cmdline_depth++ || !cmd || !csh.typed) return;
    if (!args) {
        strncpy(b, cmd, sizeof b - 1);
        b[sizeof b - 1] = 0;
    } else {
        if ((int)strlen(cmd) + (int)strlen(args) + 4 > (int)sizeof b) return;
        if (strchr(cmd, ' ')) { b[0] = '"'; strcpy(b + 1, cmd); strcat(b, "\""); }
        else strcpy(b, cmd);
        if (*args && *args != '\n') { strcat(b, " "); strcat(b, args); }
    }
    n = strlen(b);
    while (n && (b[n - 1] == '\n' || b[n - 1] == ' ')) b[--n] = 0;
    cmdline_set = SetVar("_CMDLINE", b, -1, LV_VAR | GVF_LOCAL_ONLY);
}

void run_cmdline_end(void)
{
    if (cmdline_depth && !--cmdline_depth && cmdline_set) {
        DeleteVar("_CMDLINE", LV_VAR | GVF_LOCAL_ONLY);
        cmdline_set = 0;
    }
}

/* Runs a loaded command on this process, as the ROM Shell's UserShell does:
 * stack, redirections, program name and dir, cli_Module, breaks cleared. */
static LONG run_seg(BPTR seg, const char *name, struct cmdline *c, BPTR progdir, int *detached)
{
    struct CommandLineInterface *cli = csh.cli;
    BPTR in = 0, out = 0, err = 0, oldin, oldout, olddir, oldces;
    char oldname[256];
    LONG stack = cli->cli_DefaultStack * 4, rc, r2;
    *detached = 0;
    if (c->in[0] && !(in = Open(c->in, c->inout ? MODE_READWRITE : MODE_OLDFILE))) {
        PrintFault(IoErr(), c->in); set_rc(20, IoErr()); return -1;
    }
    if (c->inout) out = in;
    else if (c->out[0] && !(out = openredir(c->out, MODE_NEWFILE, c->out_append))) {
        PrintFault(IoErr(), c->out); if (in) Close(in); set_rc(20, IoErr()); return -1;
    }
    if (c->err[0] && !(err = openredir(c->err, MODE_NEWFILE, c->err_append))) {
        PrintFault(IoErr(), c->err); if (in) Close(in); if (out && out != in) Close(out);
        set_rc(20, IoErr()); return -1;
    }
    if (stack < 4000) stack = 4000;
    /* the defaults are the streams of the moment: csh points them elsewhere
     * for a redirected loop or block, or $(...) */
    Flush(Output());
    oldin = SelectInput(in ? in : c->inh ? (BPTR)c->inh : Input());
    oldout = SelectOutput(out ? out : Output());
    oldces = csh.me->pr_CES;
    if (err) csh.me->pr_CES = err;
    else if (c->errtoout) csh.me->pr_CES = out ? out : Output();
    GetProgramName(oldname, sizeof oldname);
    SetProgramName((STRPTR)name);
    olddir = SetProgramDir(progdir);
    cli->cli_Module = seg;
    /* C, E and F as the ROM Shell does; a Ctrl-D stays for the script loop */
    SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F);
    run_cmdline_begin(name, c->args);
    SetIoErr(0);
    rc = RunCommand(seg, stack, c->args, c->arglen);
    r2 = IoErr();
    run_cmdline_end();
    if (cli->cli_Module == 0) *detached = 1;   /* the command kept its code: never unload */
    cli->cli_Module = csh.module;
    Flush(Output());
    SetProgramDir(olddir);
    SetProgramName(oldname);
    csh.me->pr_CES = oldces;
    SelectOutput(oldout);
    SelectInput(oldin);
    if (err) Close(err);
    if (out && out != in) Close(out);
    if (in) Close(in);
    if (rc == -1) { PrintFault(ERROR_NO_FREE_STORE, (STRPTR)name); rc = 20; r2 = ERROR_NO_FREE_STORE; }
    set_rc(rc, r2);
    return rc;
}

/* Runs a ROM internal command (CD, Execute...) with an argument line. */
LONG run_internal(const char *name, const char *args, struct cmdline *redir)
{
    struct cmdline c;
    struct Segment *s = resident(name);
    int det;
    LONG rc;
    if (!s) { Printf("%s: Unknown command\n", name); set_rc(10, ERROR_OBJECT_NOT_FOUND); return 10; }
    if (redir) c = *redir; else memset(&c, 0, sizeof c);
    strncpy(c.cmd, name, sizeof c.cmd - 1);
    strncpy(c.args, args, sizeof c.args - 2);
    c.args[sizeof c.args - 2] = 0;
    c.arglen = strlen(c.args);
    if (!c.arglen || c.args[c.arglen - 1] != '\n') { c.args[c.arglen++] = '\n'; c.args[c.arglen] = 0; }
    rc = run_seg(s->seg_Seg, name, &c, 0, &det);
    resident_done(s);
    return rc;
}

/* "name" args - a path quoted for a command line; 0 when it does not fit
 * (said) */
static int quoted(char *dst, int size, const char *path, const char *args)
{
    int n = strlen(args);
    while (n && args[n - 1] == '\n') n--;
    if ((int)strlen(path) + n + 5 > size) {
        dst[0] = 0;
        Printf("csh: line too long\n"); set_rc(10, 0);
        return 0;
    }
    dst[0] = '"'; strcpy(dst + 1, path); strcat(dst, "\"");
    if (n) { strcat(dst, " "); strncat(dst, args, n); }
    return 1;
}

/* A script's full name from its lock (unlocked); 0 when it is too long (said). */
static int lockname(BPTR lock, char *buf, int size)
{
    int ok = NameFromLock(lock, buf, size);
    LONG e = IoErr();
    UnLock(lock);
    if (!ok) { PrintFault(e, "csh"); set_rc(20, e); }
    return ok;
}

int run_capture(const char *cmd, char *buf, int size)
{
    static int k;
    char tmp[48];
    BPTR fh, nil;
    LONG n = -1;
    strcpy(tmp, "T:csh-bq-");
    csh_tag(tmp + strlen(tmp));
    strcat(tmp, "-");
    ltoa10(++k, tmp + strlen(tmp));
    if (!(fh = Open(tmp, MODE_NEWFILE))) return -1;
    nil = Open("NIL:", MODE_OLDFILE);
    SystemTags((STRPTR)cmd, SYS_Input, nil, SYS_Output, fh, TAG_END);
    Close(nil);
    Seek(fh, 0, OFFSET_BEGINNING);
    n = Read(fh, buf, size - 1);
    Close(fh);
    DeleteFile(tmp);
    if (n < 0) n = 0;
    buf[n] = 0;
    return n;
}

/* Ends the script being read (EOF, FailAt, Ctrl-D): back to the console.
 * The T: copy that Execute made is deleted, as the ROM Shell does. */
void script_end(void)
{
    struct CommandLineInterface *cli = csh.cli;
    UBYTE *cf = BADDR(cli->cli_CommandFile);
    if (cli->cli_CurrentInput != cli->cli_StandardInput) {
        Close(cli->cli_CurrentInput);
        cli->cli_CurrentInput = cli->cli_StandardInput;
    }
    if (cf && cf[0]) {
        char name[256];
        memcpy(name, cf + 1, cf[0]); name[cf[0]] = 0;
        /* csh FROM x typed in an Amiga script: that script's file is its Shell's */
        if (strcmp(name, csh.outercf)) {
            DeleteFile(name);
            cf[0] = 0;
        }
    }
    cli->cli_FailLevel = 10;
}

static int builtin(struct cmdline *c)
{
    if (!stricmp(c->cmd, "history")) {
        hist_print(csh.out);
        set_rc(0, 0);
        return 1;
    }
    if (!stricmp(c->cmd, "exit")) {
        LONG n = 0;
        if (StrToLong(c->args, &n) < 0) n = csh.rc;
        set_rc(n, 0);
        csh.quit = 1;
        return 1;
    }
    return 0;
}

/* One FileInfoBlock for the searches, kept while csh runs and given back
 * at its end (run_end): a kept one leaked 260 bytes each csh. */
static struct FileInfoBlock *fib;

void run_end(void)
{
    if (fib) FreeDosObject(DOS_FIB, fib);
    fib = NULL;
}

/* Is there a command of that name (resident, a file in the current dir,
 * the Path or C:)? Nothing loaded. For a pipe: every stage is looked for
 * before any starts, as the ROM Shell does. */
int run_exists(const char *name)
{
    struct Segment *seg = NULL;
    BPTR lock = 0, where;
    const char *lname;
    int f;
    if (!fib && !(fib = AllocDosObject(DOS_FIB, NULL))) return 1;
    f = find(name, 0, &seg, &lock, fib, &where, &lname);
    if (f == F_RES) { if (seg->seg_UC > 0) resident_done(seg); return 1; }
    if (lock) UnLock(lock);
    return f == F_FILE;
}

/* Where a command would be found (command -v): its file's full name, or
 * the name of a resident one; 0 when there is none. */
int run_which(const char *name, char *buf, int size)
{
    struct Segment *seg = NULL;
    BPTR lock = 0, where;
    const char *lname;
    int f;
    if (!fib && !(fib = AllocDosObject(DOS_FIB, NULL))) return 0;
    f = find(name, 0, &seg, &lock, fib, &where, &lname);
    if (f == F_RES) {
        if (seg->seg_UC > 0) resident_done(seg);
        strncpy(buf, name, size - 1); buf[size - 1] = 0;
        return 1;
    }
    if (f == F_FILE && lock && NameFromLock(lock, buf, size)) { UnLock(lock); return 1; }
    if (lock) UnLock(lock);
    return 0;
}

/* For a pipe stage or a job: the command found and loaded, not run (a
 * process of its own runs it, job.c). Fills *p; returns 0 when it cannot
 * be run (said, rc set). A script or ARexx file is given as a ROM Shell
 * line instead (p->romline), as the ROM Shell runs those. */
int run_prepare(struct cmdline *c, struct prepared *p)
{
    struct Segment *seg = NULL;
    BPTR lock = 0, where;
    const char *lname;
    int f;
    memset(p, 0, sizeof *p);
    if (!fib && !(fib = AllocDosObject(DOS_FIB, NULL))) return 0;
    f = find(c->cmd, 0, &seg, &lock, fib, &where, &lname);
    p->stack = csh.cli->cli_DefaultStack * 4;
    if (p->stack < 4000) p->stack = 4000;
    if (f == F_RES) {
        p->seg = seg->seg_Seg;
        p->res = seg;
        return 1;
    }
    if (f == F_FILE) {
        if (fib->fib_Protection & FIBF_SCRIPT) {
            char path[512];
            if (!lockname(lock, path, sizeof path)) return 0;
            if (lang_isscript(path)) {
                strcpy(p->romline, "csh FROM ");
                if (!quoted(p->romline + 9, sizeof p->romline - 9, path, c->args)) return 0;
            } else if (!quoted(p->romline, sizeof p->romline, path, c->args)) return 0;
            return 1;
        }
        if (!(fib->fib_Protection & FIBF_EXECUTE)) {
            BPTR old = where ? CurrentDir(where) : 0;
            p->seg = LoadSeg((STRPTR)lname);
            if (where) CurrentDir(old);
        }
        if (!p->seg) {
            LONG e = fib->fib_Protection & FIBF_EXECUTE ? ERROR_FILE_NOT_OBJECT : IoErr();
            if (e == ERROR_OBJECT_WRONG_TYPE) e = ERROR_FILE_NOT_OBJECT;
            UnLock(lock);
            PrintFault(e, c->cmd);
            set_rc(10, e);
            return 0;
        }
        p->progdir = ParentDir(lock);
        UnLock(lock);
        return 1;
    }
    if (f == F_DIR) UnLock(lock);
    Printf("%s: Unknown command\n", c->cmd);
    set_rc(10, ERROR_OBJECT_NOT_FOUND);
    return 0;
}

/* Runs what run_prepare took, on csh's own process, and gives it back. */
LONG run_prepared(struct cmdline *c, struct prepared *p)
{
    int det = 0;
    LONG rc;
    if (p->seg) {
        rc = run_seg(p->seg, c->cmd, c, p->progdir, &det);
        run_release(p, det);
        return rc;
    }
    Flush(Output());
    rc = SystemTags(p->romline, SYS_Input, c->inh ? (BPTR)c->inh : Input(), SYS_Output, Output(), TAG_END);
    set_rc(rc, IoErr());
    run_release(p, 0);
    return rc;
}

/* What run_prepare took: unloaded, the resident count lowered, the dir freed. */
void run_release(struct prepared *p, int detached)
{
    if (p->res) resident_done(p->res);
    else if (p->seg && !detached) UnLoadSeg(p->seg);
    if (p->progdir) UnLock(p->progdir);
    p->seg = 0; p->res = NULL; p->progdir = 0;
}

/* Runs one command already split up (the ROM's rules or csh's language):
 * the search, then RunCommand. Returns the rc (also in $RC). */
LONG run_cmd(struct cmdline *c, int depth)
{
    struct Segment *seg = NULL;
    BPTR lock = 0, where;
    const char *lname;
    int f, alone, det;
    LONG rc = 0;

    if (!fib && !(fib = AllocDosObject(DOS_FIB, NULL))) return 20;
    alone = c->args[0] == '\n' && !c->in[0] && !c->out[0];
    f = find(c->cmd, alone, &seg, &lock, fib, &where, &lname);
    if (f == F_RES) {
        rc = run_seg(seg->seg_Seg, c->cmd, c, 0, &det);
        resident_done(seg);
    } else if (f == F_DIR) {
        char path[300];
        quoted(path, sizeof path, c->cmd, "");
        UnLock(lock);
        rc = run_internal("CD", path, NULL);
    } else if (f == F_FILE) {
        static char path[512], cmd[LINE_MAX];
        if (fib->fib_Protection & FIBF_SCRIPT) {
            char head[6] = { 0 };
            if (!lockname(lock, path, sizeof path)) return 20;
            {   /* one read tells #!csh, ARexx and an Amiga script apart */
                BPTR fh = Open(path, MODE_OLDFILE);
                if (fh) { Read(fh, head, 6); Close(fh); }
            }
            run_cmdline_begin(c->cmd, c->args);     /* the script's line, not its commands' */
            if (!memcmp(head, "#!csh", 5) && (head[5] == '\n' || head[5] == ' ' || head[5] == '\r' || !head[5]))
                rc = lang_runscript(path, c);
            else if (head[0] == '/' && head[1] == '*') {
                strcpy(cmd, "RX ");
                if (!quoted(cmd + 3, sizeof cmd - 3, path, c->args)) rc = 10;
                else {
                    run_line(cmd, depth + 1);
                    rc = csh.rc;
                }
            } else if (!quoted(cmd, sizeof cmd, path, c->args)) rc = 10;
            else rc = run_internal("Execute", cmd, c);
            run_cmdline_end();
        } else {
            /* e cleared, or not a program: "file is not executable" as the ROM says */
            BPTR s = 0, dir;
            if (!(fib->fib_Protection & FIBF_EXECUTE)) {
                BPTR old = where ? CurrentDir(where) : 0;
                s = LoadSeg((STRPTR)lname);
                if (where) CurrentDir(old);
            }
            if (!s) {
                LONG e = fib->fib_Protection & FIBF_EXECUTE ? ERROR_FILE_NOT_OBJECT : IoErr();
                if (e == ERROR_OBJECT_WRONG_TYPE) e = ERROR_FILE_NOT_OBJECT;
                UnLock(lock);
                PrintFault(e, c->cmd);
                set_rc(10, e);
                rc = 10;
            } else {
                dir = ParentDir(lock);
                UnLock(lock);
                rc = run_seg(s, c->cmd, c, dir, &det);
                if (!det) UnLoadSeg(s);
                UnLock(dir);
            }
        }
    } else {
        Printf("%s: Unknown command\n", c->cmd);
        set_rc(10, ERROR_OBJECT_NOT_FOUND);
        rc = 10;
    }
    return rc;
}

/* a | b | c in an Amiga script: each stage split by the ROM's rules and
 * found and loaded first - one missing, none runs (rc 10), as the ROM Shell
 * does - then run by job.c as csh's own pipes are (the earlier stages in
 * processes of their own, the last in csh). Faster than handing the line
 * to the ROM Shell, which costs a Shell more. */
#define MAXSTAGES 16
static LONG amiga_pipe(const char *line)
{
    struct stagebuf { struct cmdline c; struct prepared p; } *sb;
    char seg[LINE_MAX];
    const char *p = line, *start = line;
    void *job;
    long in, rc = 0;
    int q = 0, n = 0, i, ok = 1;
    if (!(sb = AllocVec(MAXSTAGES * sizeof *sb, MEMF_ANY | MEMF_CLEAR))) return 20;
    for (;;) {
        int last = 0;
        for (;; p++) {
            if (!*p || *p == '\n' || (!q && *p == ';')) { last = 1; break; }
            if (*p == '"') q = !q;
            else if (q && *p == '*' && p[1]) p++;
            else if (!q && *p == '|' && (p == line || p[-1] == ' ' || p[-1] == '\t') && (p[1] == ' ' || p[1] == '\t')) break;
        }
        if (p - start >= (int)sizeof seg || n == MAXSTAGES) { Printf("csh: line too long\n"); ok = 0; break; }
        memcpy(seg, start, p - start); seg[p - start] = 0;
        if (line_parse(seg, &sb[n].c) > 0) {
            if (!run_prepare(&sb[n].c, &sb[n].p)) { ok = 0; break; }
            n++;
        }
        if (last) break;
        start = ++p;
    }
    if (!ok || !n) {
        for (i = 0; i < n; i++) run_release(&sb[i].p, 0);
        FreeVec(sb);
        return ok ? 0 : 10;
    }
    if (!(job = os_job_new(line, 0))) { for (i = 0; i < n; i++) run_release(&sb[i].p, 0); FreeVec(sb); return 20; }
    in = os_job_in(job);
    for (i = 0; i < n; i++) {
        if (i == n - 1) { rc = job_lastrun_prepared(job, &sb[i].c, in, &sb[i].p); break; }
        {
            long next, out;
            if (!os_pipe(job, &next, &out)) { int k; os_close(job, in); for (k = i; k < n; k++) run_release(&sb[k].p, 0); rc = 20; break; }
            job_spawn_prepared(job, &sb[i].c, in, out, &sb[i].p);
            in = next;
        }
    }
    FreeVec(sb);
    {
        long rcs[32];
        int nrcs;
        return os_job_end(job, rc, 1, rcs, &nrcs);
    }
}

/* A line by the ROM Shell's rules (Amiga scripts): alias, $vars, backquotes,
 * split, run; FailAt ends the script. */
void run_line(const char *line, int depth)
{
    static char a[LINE_MAX], b[LINE_MAX];
    static struct cmdline cbuf;
    struct cmdline *c = &cbuf;
    const char *l = line;
    int interactive = csh.cli->cli_CurrentInput == csh.cli->cli_StandardInput && !csh.fromfile;
    LONG rc = 0;
    char name[64];              /* for the FailAt message: RX runs a line too (cbuf) */

    if (depth > 4) { Printf("csh: too deep\n"); set_rc(20, 0); return; }
    if (line_alias(l, a, sizeof a, getalias) > 0) l = a;
    if (line_vars(l, b, sizeof b, getvar) < 0) { Printf("csh: line too long\n"); set_rc(10, 0); return; }
    l = b;
    if (strchr(l, '`')) {
        if (line_backquotes(l, a, sizeof a, run_capture) < 0) {
            Printf("csh: unmatched backquote\n"); set_rc(10, 0); return;
        }
        l = a;
    }
    switch (line_parse(l, c)) {
    case -1: Printf("csh: line too long\n"); set_rc(10, 0); return;
    case 0: return;
    }
    strncpy(name, c->cmd, sizeof name - 1); name[sizeof name - 1] = 0;

    if (c->pipe) rc = amiga_pipe(l);
    else if (builtin(c)) return;
    else rc = run_cmd(c, depth);
    /* a script ends when a command fails at or over FailAt */
    if (!interactive && rc >= csh.cli->cli_FailLevel && !csh.quit) {
        Printf("%s failed returncode %ld\n", name, (long)rc);
        script_end();
        if (csh.fromfile) csh.quit = 1;
    }
}
