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
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <stdio.h>
#include <string.h>
#include "csh.h"
#include "edit.h"

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
    oldin = SelectInput(in ? in : Input());
    oldout = SelectOutput(out ? out : Output());
    oldces = csh.me->pr_CES;
    if (err) csh.me->pr_CES = err;
    else if (c->errtoout) csh.me->pr_CES = out ? out : Output();
    GetProgramName(oldname, sizeof oldname);
    SetProgramName((STRPTR)name);
    olddir = SetProgramDir(progdir);
    cli->cli_Module = seg;
    SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F);
    SetIoErr(0);
    rc = RunCommand(seg, stack, c->args, c->arglen);
    r2 = IoErr();
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

/* "name" args - a path quoted for a command line */
static void quoted(char *dst, int size, const char *path, const char *args)
{
    int n = strlen(args);
    while (n && args[n - 1] == '\n') n--;
    if ((int)strlen(path) + n + 5 > size) { dst[0] = 0; return; }
    dst[0] = '"'; strcpy(dst + 1, path); strcat(dst, "\"");
    if (n) { strcat(dst, " "); strncat(dst, args, n); }
}

int run_capture(const char *cmd, char *buf, int size)
{
    char tmp[40];
    BPTR fh, nil;
    LONG n = -1;
    strcpy(tmp, "T:csh-bq-");
    ltoa10(csh.me->pr_TaskNum, tmp + 9);
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
        DeleteFile(name);
        cf[0] = 0;
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

/* Runs one command already split up (the ROM's rules or csh's language):
 * the search, then RunCommand. Returns the rc (also in $RC). */
LONG run_cmd(struct cmdline *c, int depth)
{
    static struct FileInfoBlock *fib;
    struct Segment *seg = NULL;
    BPTR lock = 0, where;
    const char *lname;
    int f, alone, det;
    LONG rc = 0;

    if (!fib && !(fib = AllocDosObject(DOS_FIB, NULL))) return 20;   /* kept for csh's life */
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
        static char path[256], cmd[LINE_MAX];
        if (fib->fib_Protection & FIBF_SCRIPT) {
            char head[2] = { 0, 0 };
            int ischsh;
            NameFromLock(lock, path, sizeof path);
            UnLock(lock);
            ischsh = lang_isscript(path);
            if (ischsh) return lang_runscript(path, c);
            {
                BPTR fh = Open(path, MODE_OLDFILE);
                if (fh) { Read(fh, head, 2); Close(fh); }
            }
            if (head[0] == '/' && head[1] == '*') {
                strcpy(cmd, "RX ");
                quoted(cmd + 3, sizeof cmd - 3, path, c->args);
                run_line(cmd, depth + 1);
                return csh.rc;
            }
            quoted(cmd, sizeof cmd, path, c->args);
            rc = run_internal("Execute", cmd, c);
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
    if (line_parse(l, c) <= 0) return;

    if (c->pipe) {          /* for now the ROM Shell runs pipes (step 5: csh's own) */
        Flush(Output());
        rc = SystemTags((STRPTR)l, SYS_Input, Input(), SYS_Output, Output(), TAG_END);
        set_rc(rc, IoErr());
    } else if (builtin(c)) return;
    else rc = run_cmd(c, depth);
    /* a script ends when a command fails at or over FailAt */
    if (!interactive && rc >= csh.cli->cli_FailLevel && !csh.quit) {
        Printf("%s failed returncode %ld\n", c->cmd, (long)rc);
        script_end();
        if (csh.fromfile) csh.quit = 1;
    }
}
