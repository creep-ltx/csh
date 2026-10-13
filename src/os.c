/* os.c - the os_ calls of lang.h on the Amiga: variables are AmigaDOS
 * variables (local unless exported), an Amiga command goes to run.c,
 * redirections and $(...) select other streams for a while.
 *
 * csh's own output (echo, printf...) is gathered here and sent in one
 * Write: a console packet a word would make a loop of echos crawl. It is
 * flushed before anything else can write (a command, a redirection, a
 * read, the prompt). */
#include <exec/memory.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>
#include "csh.h"
#include "lang.h"
#include "edit.h"

extern char *readall(const char *path, long max);

/* ---- output ---- */

static char obuf[1024];
static int olen;

void os_flush(void)
{
    if (olen) { Write(Output(), obuf, olen); olen = 0; }
}

void os_write(const char *s, int n)
{
    if (olen + n > (int)sizeof obuf) os_flush();
    if (n > (int)sizeof obuf) { Write(Output(), (APTR)s, n); return; }
    memcpy(obuf + olen, s, n);
    olen += n;
}

void *os_alloc(long n) { return AllocVec(n, MEMF_ANY | MEMF_CLEAR); }
void os_free(void *p) { if (p) FreeVec(p); }

/* ---- variables ---- */

int os_getvar(const char *name, char *buf, int size)
{
    LONG n = GetVar((STRPTR)name, buf, size, 0);
    return n < 0 ? -1 : n;
}

void os_setvar(const char *name, const char *val, int global)
{
    SetVar((STRPTR)name, (STRPTR)val, -1, global ? GVF_GLOBAL_ONLY : (LV_VAR | GVF_LOCAL_ONLY));
}

void os_unsetvar(const char *name) { DeleteVar((STRPTR)name, LV_VAR | GVF_LOCAL_ONLY); }

int os_getalias(const char *name, char *buf, int size)
{
    LONG n = GetVar((STRPTR)name, buf, size, LV_ALIAS | GVF_LOCAL_ONLY);
    return n < 0 ? -1 : n;
}

void os_setalias(const char *name, const char *val)
{
    SetVar((STRPTR)name, (STRPTR)val, -1, LV_ALIAS | GVF_LOCAL_ONLY);
}

void os_setrc(long rc) { set_rc(rc, rc ? csh.cli->cli_Result2 : 0); }

/* ---- commands ---- */

long os_run(struct cmdline *c)
{
    os_flush();
    return run_cmd(c, 0);
}

long os_rom(const char *line, int background)
{
    LONG rc;
    os_flush();
    if (background) return run_internal("Run", line, NULL);   /* the ROM's Run: [CLI n] */
    rc = SystemTags((STRPTR)line, SYS_Input, Input(), SYS_Output, Output(), TAG_END);
    set_rc(rc, IoErr());
    return rc;
}

int os_readline(char *buf, int size)
{
    os_flush();
    if (!FGets(Input(), buf, size)) return -1;
    return strlen(buf);
}

/* ---- redirections for csh's own commands and blocks ---- */

struct save { BPTR in, out, ces, oin, oout, oces; };

static BPTR openfor(const char *name, LONG mode, int append)
{
    BPTR fh;
    if (append && (fh = Open((STRPTR)name, MODE_READWRITE))) { Seek(fh, 0, OFFSET_END); return fh; }
    return Open((STRPTR)name, mode);
}

void *os_redir(struct cmdline *c)
{
    struct save *s = AllocVec(sizeof *s, MEMF_ANY | MEMF_CLEAR);
    if (!s) return NULL;
    os_flush();
    if (c->in[0] && !(s->in = Open(c->in, c->inout ? MODE_READWRITE : MODE_OLDFILE))) goto bad;
    if (c->inout) s->out = s->in;
    else if (c->out[0] && !(s->out = openfor(c->out, MODE_NEWFILE, c->out_append))) goto bad;
    if (c->err[0] && !(s->ces = openfor(c->err, MODE_NEWFILE, c->err_append))) goto bad;
    s->oces = csh.me->pr_CES;
    s->oin = s->in ? SelectInput(s->in) : 0;
    s->oout = s->out ? SelectOutput(s->out) : 0;
    if (s->ces) csh.me->pr_CES = s->ces;
    else if (c->errtoout) csh.me->pr_CES = Output();
    return s;
bad:
    PrintFault(IoErr(), c->in[0] && !s->in ? c->in : c->out[0] && !s->out ? c->out : c->err);
    if (s->in) Close(s->in);
    if (s->out && s->out != s->in) Close(s->out);
    FreeVec(s);
    return NULL;
}

void os_unredir(void *p)
{
    struct save *s = p;
    if (!s) return;
    os_flush();
    if (s->out) { Flush(s->out); SelectOutput(s->oout); }
    if (s->in) SelectInput(s->oin);
    csh.me->pr_CES = s->oces;
    if (s->ces) Close(s->ces);
    if (s->out && s->out != s->in) Close(s->out);
    if (s->in) Close(s->in);
    FreeVec(s);
}

/* ---- $(...): the output into a T: file for a while ---- */

struct cap { BPTR fh, old; char name[40]; };
static int capdepth;

void *os_capture(void)
{
    struct cap *c = AllocVec(sizeof *c, MEMF_ANY | MEMF_CLEAR);
    if (!c) return NULL;
    os_flush();
    strcpy(c->name, "T:csh-cap-");
    ltoa10(csh.me->pr_TaskNum, c->name + strlen(c->name));
    strcat(c->name, "-");
    ltoa10(++capdepth, c->name + strlen(c->name));
    if (!(c->fh = Open(c->name, MODE_NEWFILE))) { capdepth--; FreeVec(c); return NULL; }
    c->old = SelectOutput(c->fh);
    return c;
}

int os_captured(void *p, char **text)
{
    struct cap *c = p;
    LONG n;
    os_flush();
    Flush(c->fh);
    SelectOutput(c->old);
    n = Seek(c->fh, 0, OFFSET_BEGINNING);
    *text = NULL;
    if (n >= 0 && (*text = AllocVec(n + 1, MEMF_ANY | MEMF_CLEAR))) n = Read(c->fh, *text, n);
    if (n < 0) n = 0;
    Close(c->fh);
    DeleteFile(c->name);
    capdepth--;
    FreeVec(c);
    return n;
}

int os_break(void)
{
    return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}

/* ---- files ---- */

int os_test(const char *path, int op)
{
    struct FileInfoBlock *fib;
    APTR win = csh.me->pr_WindowPtr;
    BPTR l;
    int r = 0;
    csh.me->pr_WindowPtr = (APTR)-1;
    l = Lock((STRPTR)path, SHARED_LOCK);
    csh.me->pr_WindowPtr = win;
    if (!l) return 0;
    if (op == 'e') { UnLock(l); return 1; }
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (Examine(l, fib)) {
            switch (op) {
            case 'f': r = fib->fib_DirEntryType < 0; break;
            case 'd': r = fib->fib_DirEntryType > 0; break;
            case 'r': r = !(fib->fib_Protection & FIBF_READ); break;
            case 'w': r = !(fib->fib_Protection & FIBF_WRITE); break;
            case 'x': r = !(fib->fib_Protection & FIBF_EXECUTE); break;
            case 's': r = fib->fib_DirEntryType < 0 && fib->fib_Size > 0; break;
            }
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return r;
}

/* The names a pattern matches (sh's * ? [..], Amiga case-blind), with the
 * drawer part as typed. */
int os_glob(const char *pattern, void (*fn)(const char *name, void *u), void *u)
{
    char dir[256], apat[300], parsed[620], full[300];
    struct FileInfoBlock *fib;
    const char *name = pattern, *p;
    APTR win = csh.me->pr_WindowPtr;
    BPTR l;
    int dl, o = 0, n = 0;
    for (p = pattern; *p; p++) if (*p == ':' || *p == '/') name = p + 1;
    dl = name - pattern;
    if (dl >= (int)sizeof dir) return 0;
    memcpy(dir, pattern, dl); dir[dl] = 0;
    for (p = name; *p && o < (int)sizeof apat - 3; p++) {
        if (*p == '*') { apat[o++] = '#'; apat[o++] = '?'; }
        else apat[o++] = *p;
    }
    apat[o] = 0;
    if (ParsePatternNoCase(apat, parsed, sizeof parsed) < 0) return 0;
    csh.me->pr_WindowPtr = (APTR)-1;
    l = Lock(dir, SHARED_LOCK);
    csh.me->pr_WindowPtr = win;
    if (!l) return 0;
    if ((fib = AllocDosObject(DOS_FIB, NULL))) {
        if (Examine(l, fib) && fib->fib_DirEntryType > 0)
            while (ExNext(l, fib))
                if (MatchPatternNoCase(parsed, fib->fib_FileName) && dl + strlen(fib->fib_FileName) < sizeof full) {
                    strcpy(full, dir);
                    strcat(full, fib->fib_FileName);
                    fn(full, u);
                    n++;
                }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(l);
    return n;
}

char *os_readfile(const char *path, long *len)
{
    char *t = readall(path, 4L * 1024 * 1024);
    if (t) *len = strlen(t);
    return t;
}

int os_tempfile(const char *text, int n, char *name, int size)
{
    static int k;
    BPTR fh;
    if (size < 40) return 0;
    strcpy(name, "T:csh-hd-");
    ltoa10(csh.me->pr_TaskNum, name + strlen(name));
    strcat(name, "-");
    ltoa10(++k, name + strlen(name));
    if (!(fh = Open(name, MODE_NEWFILE))) return 0;
    Write(fh, (APTR)text, n);
    Close(fh);
    return 1;
}

void os_delete(const char *name) { DeleteFile((STRPTR)name); }

int os_cd(const char *dir)
{
    char b[300];
    strcpy(b, "\"");
    strncat(b, dir, 290);
    strcat(b, "\"");
    return run_internal("CD", b, NULL) == 0;
}

long os_number(void) { return csh.me->pr_TaskNum; }

void os_quit(long rc)
{
    set_rc(rc, 0);
    csh.quit = 1;
}

int os_interactive(void) { return csh.cli->cli_CurrentInput == csh.cli->cli_StandardInput && !csh.fromfile; }

void os_history(void)
{
    os_flush();
    hist_print(Output());
}

long os_ticks(void)
{
    struct DateStamp d;
    DateStamp(&d);
    return (d.ds_Days * 1440 + d.ds_Minute) * 3000 + d.ds_Tick;
}

int os_pwd(char *b, int n)
{
    return GetCurrentDirName(b, n) != 0;
}
