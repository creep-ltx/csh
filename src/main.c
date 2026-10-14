/* main.c - csh's start, prompt and main loop.
 *
 * csh is a command typed in a Shell (or the command a CTerm tab starts).
 * It owns the CLI it runs in: the Path, the current dir, the local
 * variables, the prompt are that CLI's, shared with the ROM Shell under it.
 * Like the ROM Shell it always reads the next line from cli_CurrentInput
 * (never a copy of it: Execute changes and closes it), so the ROM's own
 * Execute, If, Skip and Quit work in csh unchanged.
 *
 *   csh            the prompt, on the console csh was started from
 *   csh FROM file  runs the file's lines, then ends (rc = the last one's)
 *   exit [rc]      back to the ROM Shell; EndCLI ends the whole Shell */
#include <exec/memory.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/utility.h>
#include <dos/rdargs.h>
#include <dos/var.h>
#include <stdio.h>
#include <string.h>
#include <dos/datetime.h>
#include "csh.h"
#include "edit.h"
#include "lang.h"

#ifdef CSHDEBUG
#include <clib/debug_protos.h>
#define DBG(x) KPrintF x
#else
#define DBG(x)
#endif

#ifdef CSH_USERSHELL
static const char version[] = "$VER: cshell " CSH_VERSION " (" CSH_DATE ")";
#else
static const char version[] = "$VER: csh " CSH_VERSION " (" CSH_DATE ")";
#endif

struct csh csh;

#ifdef CSH_USERSHELL
/* ---- csh as the system's user shell (cshell; docs/design.md "csh as the
 * user shell"; the Guru Book 14.5 and its ShellInit.c). shellstart.S took
 * the start-up packet; here the CLI is made from it, and at the end the
 * streams closed and the packet answered as the type of start asks. */
extern struct DosPacket *_shellpkt;
extern long _keepdata;

#define FNF_VALIDFLAGS  0x80000000UL
#define FNF_ASYNCSYSTEM 0x00000008UL
#define FNF_SYSTEM      0x00000004UL
#define FNF_USERINPUT   0x00000002UL
#define FNF_RUNOUTPUT   0x00000001UL

enum { SH_FAILURE, SH_ASYNC, SH_RUN, SH_SYNC };
LONG cliinit(struct DosPacket *dp, LONG newcli, struct DosLibrary *dos);   /* shellstart.S */
static int shtype;
static ULONG shflags;

/* 0: no shell to run (the packet was answered) */
static int shell_init(void)
{
    struct DosPacket *dp = _shellpkt;
    struct Process *pr = (struct Process *)FindTask(NULL);
    BPTR *seg = BADDR(pr->pr_SegList);
    LONG newcli;
    if (dp->dp_Res2) { ReplyPkt(dp, dp->dp_Res1, dp->dp_Res2); return 0; }   /* a start not defined yet */
    newcli = dp->dp_Res1;
    if (!seg[4]) { seg[4] = seg[3]; seg[3] = 0; }      /* room for the commands' seglists */
    shflags = cliinit(dp, newcli, DOSBase);
    DBG(("cshell: newcli %ld flags %lx ioerr %lx pr %lx\n", newcli, shflags, IoErr(), (ULONG)pr));
    if (!(shflags & FNF_VALIDFLAGS)) {
        if ((struct Process *)IoErr() == pr) return 0;  /* failed; the packet was answered */
        shtype = newcli ? SH_ASYNC : SH_RUN;
    } else shtype = shflags & FNF_ASYNCSYSTEM ? SH_ASYNC : SH_SYNC;
    /* NewShell, an asynchronous System(), Run: the caller goes on now */
    if (shtype == SH_ASYNC || shtype == SH_RUN) ReplyPkt(dp, dp->dp_Res1, dp->dp_Res2);
    SetProgramDir(0);
    csh.usershell = 1;
    csh.system = (shflags & (FNF_VALIDFLAGS | FNF_SYSTEM)) == (FNF_VALIDFLAGS | FNF_SYSTEM);
    DBG(("cshell: type %ld cli %lx\n", (LONG)shtype, (ULONG)Cli()));
    return 1;
}

static void shell_end(void)
{
    struct CommandLineInterface *cli = Cli();
    BPTR cd;
    if (!(shflags & FNF_VALIDFLAGS)) {
        if (cli->cli_StandardInput) Close(cli->cli_StandardInput);
        if (cli->cli_StandardOutput) { Flush(cli->cli_StandardOutput); Close(cli->cli_StandardOutput); }
    } else {
        Flush(Output());
        if (!(shflags & FNF_USERINPUT)) Close(cli->cli_StandardInput);
        if (shflags & FNF_RUNOUTPUT) { Flush(cli->cli_StandardOutput); Close(cli->cli_StandardOutput); }
    }
    if (shtype == SH_SYNC) ReplyPkt(_shellpkt, cli->cli_ReturnCode, cli->cli_Result2);
    if ((cd = CurrentDir(0))) UnLock(cd);
}
#endif

/* ---- the prompt ----
 *
 * PROMPT (or PS1), a csh variable, wins over the Amiga's Prompt command;
 * $var and $(cmd) in it are expanded first (bash), then zsh's % escapes:
 *   %N CLI number   %S %/ the dir   %~ the dir, HOME as ~   %c its last part
 *   %V the volume   %R %? the rc    %T 12:34   %* 12:34:56   %D 2026-10-14
 *   %j running jobs %M free memory  %E how long the last command took (2 s or more)
 *   %h history number   %F{red}...%f colour   %K{blue}...%k background
 *   %B...%b bold    %U...%u underline   %(?.ok.bad) %(j.jobs.none)   %% a %
 * RPROMPT: the same, shown at the right edge (zsh). Without PROMPT: the
 * Amiga's (cli_Prompt: %N %S %R), as before. */
static long lastdur;            /* fiftieths of a second the last command took */

static int pcolour(const char *name, int n, int bg)
{
    static const char *const names[] = { "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white", NULL };
    int i, v = -1;
    for (i = 0; names[i]; i++) if ((int)strlen(names[i]) == n && !strncmp(names[i], name, n)) v = i;
    if (v < 0 && n > 0 && name[0] >= '0' && name[0] <= '9') { v = 0; for (i = 0; i < n && name[i] >= '0' && name[i] <= '9'; i++) v = v * 10 + name[i] - '0'; }
    if (v < 0 || v > 15) return bg ? 49 : 39;
    /* CTerm's 30-37 are the Amiga's pens: its theme's colours are 90-97 */
    if (v >= 8 || term_cterm) return (bg ? 100 : 90) + (v & 7);
    return (bg ? 40 : 30) + v;
}

static int addsgr(char *out, int o, int code)
{
    out[o++] = 0x1b; out[o++] = '[';
    o += ltoa10(code, out + o);
    out[o++] = 'm';
    return o;
}

static int dirname_(char *b, int size)
{
    if (!GetCurrentDirName(b, size)) { b[0] = 0; return 0; }
    return strlen(b);
}

static const char *zprompt(const char *p, int colours, char *out, int size)
{
    int i, o = 0, n = strlen(p), depth = 0;
    char dir[256];
    for (i = 0; i < n && o < size - 64; i++) {
        char c;
        if (p[i] != '%' || i + 1 >= n) { out[o++] = p[i]; continue; }
        c = p[++i];
        switch (c) {
        case 'N': case 'n': o += ltoa10(csh.me->pr_TaskNum, out + o); break;
        case 'S': case 's': case '/': case 'd': {
            int l = dirname_(dir, sizeof dir);
            if (o + l < size - 64) { memcpy(out + o, dir, l); o += l; }
            break;
        }
        case '~': {
            char home[256];
            int l = dirname_(dir, sizeof dir), hl = GetVar("HOME", home, sizeof home, 0);
            const char *d = dir;
            if (hl > 0 && !Strnicmp(dir, home, hl) && l >= hl) { out[o++] = '~'; d = dir + hl; if (*d && d[-1] != ':' && d[-1] != '/' && *d != '/') { o--; d = dir; } else if (*d && home[hl - 1] == ':') out[o++] = '/'; }
            l = strlen(d);
            if (o + l < size - 64) { memcpy(out + o, d, l); o += l; }
            break;
        }
        case 'c': case 'C': {
            int l = dirname_(dir, sizeof dir);
            char *s = dir + l;
            while (s > dir && s[-1] != ':' && s[-1] != '/') s--;
            if (!*s) s = dir;                       /* Work: at the root */
            l = strlen(s);
            if (o + l < size - 64) { memcpy(out + o, s, l); o += l; }
            break;
        }
        case 'V': {
            int l = dirname_(dir, sizeof dir);
            char *col = strchr(dir, ':');
            if (col) l = col - dir;
            if (o + l < size - 64) { memcpy(out + o, dir, l); o += l; }
            break;
        }
        case 'R': case 'r': case '?': o += ltoa10(csh.cli->cli_ReturnCode, out + o); break;
        case 'j': o += ltoa10(job_count(), out + o); break;
        case 'h': case '!': o += ltoa10(hist_count() + 1, out + o); break;
        case 'T': case '*': case 'D': {
            struct DateTime dt;
            char t[16], d[16];
            DateStamp(&dt.dat_Stamp);
            dt.dat_Format = FORMAT_CDN; dt.dat_Flags = 0; dt.dat_StrDay = NULL;
            dt.dat_StrDate = d; dt.dat_StrTime = t;
            if (DateToStr(&dt)) {
                if (c == 'D') {             /* dd-mm-yy: 20yy-mm-dd */
                    out[o++] = '2'; out[o++] = '0'; out[o++] = d[6]; out[o++] = d[7]; out[o++] = '-';
                    out[o++] = d[3]; out[o++] = d[4]; out[o++] = '-'; out[o++] = d[0]; out[o++] = d[1];
                } else { memcpy(out + o, t, c == 'T' ? 5 : 8); o += c == 'T' ? 5 : 8; }
            }
            break;
        }
        case 'M': {
            ULONG m = AvailMem(MEMF_ANY);
            char u = 'K';
            m >>= 10;
            if (m >= 10240) { m >>= 10; u = 'M'; }
            if (m >= 10240) { m >>= 10; u = 'G'; }
            o += ltoa10(m, out + o);
            out[o++] = u;
            break;
        }
        case 'E':
            if (lastdur >= 100) {
                long s = lastdur / 50;
                if (s >= 60) { o += ltoa10(s / 60, out + o); out[o++] = 'm'; s %= 60; if (s < 10) out[o++] = '0'; o += ltoa10(s, out + o); out[o++] = 's'; }
                else { o += ltoa10(s, out + o); out[o++] = '.'; out[o++] = '0' + (lastdur % 50) / 5; out[o++] = 's'; }
            }
            break;
        case 'F': case 'K': {
            int bg = c == 'K';
            if (p[i + 1] == '{') {
                const char *e = strchr(p + i + 2, '}');
                if (e) { if (colours) o = addsgr(out, o, pcolour(p + i + 2, e - p - i - 2, bg)); i = e - p; }
            } else if (colours) o = addsgr(out, o, bg ? 49 : 39);
            break;
        }
        case 'f': if (colours) o = addsgr(out, o, 39); break;
        case 'k': if (colours) o = addsgr(out, o, 49); break;
        case 'B': if (colours) o = addsgr(out, o, 1); break;
        case 'b': if (colours) o = addsgr(out, o, 22); break;
        case 'U': if (colours) o = addsgr(out, o, 4); break;
        case 'u': if (colours) o = addsgr(out, o, 24); break;
        case '(': {
            /* %(?.yes.no): the rc 0; %(j.yes.no): jobs running */
            char t = p[i + 1], sep = p[i + 2];
            int truth = t == '?' ? csh.cli->cli_ReturnCode == 0 : t == 'j' ? job_count() > 0 : 0;
            const char *a = p + i + 3, *b = a, *e;
            char sub[256], res[600];
            int l;
            if (!sep || depth > 2) break;
            while (*b && *b != sep) b++;
            if (!*b) break;
            e = b + 1;
            while (*e && *e != ')') e++;
            if (!*e) break;
            l = truth ? b - a : e - b - 1;
            if (l > (int)sizeof sub - 1) l = sizeof sub - 1;
            memcpy(sub, truth ? a : b + 1, l); sub[l] = 0;
            depth++;
            zprompt(sub, colours, res, sizeof res);
            depth--;
            l = strlen(res);
            if (o + l < size - 64) { memcpy(out + o, res, l); o += l; }
            i = e - p;
            break;
        }
        case '%': out[o++] = '%'; break;
        default: break;
        }
    }
    if (colours && o) o = addsgr(out, o, 0);
    out[o] = 0;
    return out;
}

/* PROMPT / PS1 / RPROMPT: the variable expanded, then its escapes; NULL = not set */
static const char *varprompt(const char *name, const char *alt, int colours, char *out, int size)
{
    char raw[512], *x;
    int l = GetVar((STRPTR)name, raw, sizeof raw, GVF_BINARY_VAR);
    if (l < 0 && alt) l = GetVar((STRPTR)alt, raw, sizeof raw, GVF_BINARY_VAR);
    if (l < 0) return NULL;
    if (!(x = lang_expandstr(raw))) return NULL;
    zprompt(x, colours, out, size);
    FreeVec(x);
    os_flush();
    return out;
}

/* The prompt from cli_Prompt (set by the ROM's Prompt command): %N the CLI
 * number, %S or %s the current dir, %R the last return code, %% a percent
 * sign; other escapes print nothing (as the 3.2 ROM Shell does). With the
 * ROM's own default ("%N.%S> ") csh adds a failed return code before the
 * '>', in the promptrc colour of csh.prefs. */
static const char *prompt_text(int colours)
{
    static char out[600];
    UBYTE *bp = BADDR(csh.cli->cli_Prompt);
    char p[256], dir[256];
    int n = bp ? bp[0] : 0, i, o = 0, isdefault;
    if (varprompt("PROMPT", "PS1", colours, out, sizeof out)) return out;
    if (n == 0) { strcpy(p, "%N.%S> "); n = 7; }
    else { memcpy(p, bp + 1, n); p[n] = 0; }
    isdefault = !strcmp(p, "%N.%S> ");
    for (i = 0; i < n && o < (int)sizeof out - 300; i++) {
        if (p[i] != '%' || i + 1 >= n) {
            if (isdefault && p[i] == '>' && csh.cli->cli_ReturnCode) {
                const char *c = colours ? prefs_sgr(C_PROMPTRC) : "";
                out[o++] = ' ';
                strcpy(out + o, c); o += strlen(c);
                o += ltoa10(csh.cli->cli_ReturnCode, out + o);
                if (*c) { strcpy(out + o, "\x1b[0m"); o += 4; }
            }
            out[o++] = p[i];
            continue;
        }
        switch (p[++i]) {
        case 'N': case 'n': o += ltoa10(csh.me->pr_TaskNum, out + o); break;
        case 'S': case 's':
            if (!GetCurrentDirName(dir, sizeof dir)) dir[0] = 0;
            strcpy(out + o, dir); o += strlen(dir); break;
        case 'R': case 'r': o += ltoa10(csh.cli->cli_ReturnCode, out + o); break;
        case '%': out[o++] = '%'; break;
        default: break;
        }
    }
    out[o] = 0;
    return out;
}

/* PS2: the prompt of a line that goes on (if ...; then) */
static const char *ps2(void)
{
    static char b[64];
    int l = GetVar("PS2", b, sizeof b, 0);
    return l >= 0 ? b : "> ";
}

/* A whole line in the handle's DOS buffer (fh_Pos..fh_End)? */
static int buffered_line(BPTR f)
{
    struct FileHandle *fh = BADDR(f);
    UBYTE *b;
    LONG i;
    if (!fh || !fh->fh_Buf || fh->fh_Pos < 0 || fh->fh_Pos >= fh->fh_End) return 0;
    b = BADDR(fh->fh_Buf);
    for (i = fh->fh_Pos; i < fh->fh_End; i++) if (b[i] == '\n') return 1;
    return 0;
}

static int editing = -1;     /* csh edits the line itself (an interactive console) */

static const char *histback(int k)
{
    int n = hist_count();
    return k >= 1 && k <= n ? hist_get(n - k) : NULL;
}

/* A line typed: !! and friends made into what they stand for (shown, as
 * bash does); 0 = do not run it (an event not found, said) */
static int bang(char *line, int size)
{
    static char out[LINE_MAX];
    int r;
    if (!sh_opt_histexp || (!strchr(line, '!') && line[0] != '^')) return 1;
    if (editing <= 0) hist_load_once();
    r = bang_expand(line, out, sizeof out, histback);
    os_flush();
    if (r < 0) { set_rc(10, 0); return 0; }
    if (r > 0) {
        strncpy(line, out, size - 1);
        line[size - 1] = 0;
        FPuts(csh.out, line);
        Flush(csh.out);
    }
    return 1;
}

/* A typed line run, its time kept (%E, $CMD_DURATION in ms as fish's) */
static void runtyped(char *line)
{
    long t0 = os_ticks(), d;
    char b[16];
    csh.broke = 0;
    lang_line(line, 1);
    os_flush();
    if (lang_more) return;
    d = os_ticks() - t0;
    lastdur = d < 0 ? 0 : d;
    ltoa10(lastdur * 20, b);
    SetVar("CMD_DURATION", b, -1, LV_VAR | GVF_LOCAL_ONLY);
}

static void editor_start(void)
{
    keys_load();
    prefs_load();
    hist_load();
    if (keys_bad[0]) { FPuts(csh.out, keys_bad); FPutC(csh.out, '\n'); }
}

/* A line from fh into line (size bytes): 1, 0 at the end, -1 when it was
 * longer than csh's line (the rest read and left out, said): never split
 * into two commands. */
static int getline_(BPTR fh, char *line, int size)
{
    int n;
    if (!FGets(fh, line, size - 1)) return 0;
    n = strlen(line);
    if (n == size - 2 && line[n - 1] != '\n') {
        char rest[128];
        while (FGets(fh, rest, sizeof rest) && rest[strlen(rest) - 1] != '\n') ;
        FPuts(csh.out, "csh: line too long (left out)\n");
        return -1;
    }
    return 1;
}

/* One line of a script (cli_CurrentInput is not the console): Ctrl-D,
 * read, echoed with Set echo on, "+" joined, run by the ROM's rules.
 * 0 = the console's own end (Ctrl-\): csh ends. */
static int script_step(char *line, int size)
{
    struct CommandLineInterface *cli = csh.cli;
    int g;
    if (SetSignal(0, SIGBREAKF_CTRL_D) & SIGBREAKF_CTRL_D) {
        Printf("***Break\n");
        script_end();
        return 1;
    }
    if (!(g = getline_(cli->cli_CurrentInput, line, size))) {
        DBG(("cshell: end of input %lx (std %lx)\n", cli->cli_CurrentInput, cli->cli_StandardInput));
        if (cli->cli_CurrentInput != cli->cli_StandardInput) { script_end(); return 1; }
        return 0;                                          /* Ctrl-\ on the console */
    }
    if (g < 0) return 1;
    DBG(("cshell: line [%s] cur %lx std %lx\n", line, cli->cli_CurrentInput, cli->cli_StandardInput));
    {   /* FindVar: the local list only, no copy, no ENV: file opened */
        struct LocalVar *e = FindVar("echo", LV_VAR);
        if (e && e->lv_Len >= 2 && (e->lv_Value[0] | 32) == 'o' && (e->lv_Value[1] | 32) == 'n')
            FPuts(csh.out, line);
    }
    /* "+" at the end joins the next line, with the LF kept (line.c) */
    for (;;) {
        int n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        if (!n || line[n - 1] != '+') break;
        if (n + 2 >= size) { FPuts(csh.out, "csh: line too long (+ lines)\n"); return 1; }
        line[n - 1] = '\n';
        if (!FGets(cli->cli_CurrentInput, line + n, size - n - 1)) { line[n] = 0; break; }
    }
    run_line(line, 0);
    DBG(("cshell: rc %ld r2 %ld fail %ld cur %lx\n", cli->cli_ReturnCode, cli->cli_Result2, cli->cli_FailLevel, cli->cli_CurrentInput));
    SetSignal(0, SIGBREAKF_CTRL_C);
    return 1;
}

/* An Execute (or an Amiga script) typed in the csh language: its lines run
 * now, before the rest of the csh line, until the input is back to until
 * (or to the console). The ROM Shell would read them after the line. */
LONG script_now(BPTR until)
{
    struct CommandLineInterface *cli = csh.cli;
    char *line = AllocVec(LINE_MAX, MEMF_ANY);
    if (!line) return 20;
    while (!csh.quit && cli->cli_CurrentInput != until && cli->cli_CurrentInput != cli->cli_StandardInput) {
        if (cli->cli_Background && !csh.bg0) break;           /* EndCLI */
        if (!script_step(line, LINE_MAX)) break;
    }
    FreeVec(line);
    return csh.rc;
}

static int smain(void)
{
    static char line[LINE_MAX];
    struct CommandLineInterface *cli;
    struct RDArgs *rd;
    LONG arg[3] = { 0, 0, 0 };
    BPTR from = 0, oldcur;
    LONG oldinter, oldfail;

    (void)version;
    csh.me = (struct Process *)FindTask(NULL);
    if (!(cli = csh.cli = Cli())) return 20;
    {   /* started inside an Amiga script: its T: file stays that script's */
        UBYTE *cf = BADDR(cli->cli_CommandFile);
        if (cf && cf[0]) { memcpy(csh.outercf, cf + 1, cf[0]); csh.outercf[cf[0]] = 0; }
    }
    oldfail = cli->cli_FailLevel;
    csh.bg0 = cli->cli_Background;
    if (!csh.usershell) {   /* this very file, for the csh a job of csh's own needs (not C:'s, maybe older) */
        char nm[108];
        BPTR pd = GetProgramDir();
        if (pd && GetProgramName(nm, sizeof nm) && NameFromLock(pd, csh.self, sizeof csh.self - 40) &&
            strlen(FilePart(nm)) < 38) AddPart(csh.self, FilePart(nm), sizeof csh.self);
        else csh.self[0] = 0;
    }
    if (csh.usershell) {
        /* the Shell itself: no arguments; a command's streams are the CLI's
         * standard ones (the Guru Book 14.5.2), not the stream of command
         * lines (System()'s, a script's) */
        SelectInput(cli->cli_StandardInput);
        SelectOutput(cli->cli_StandardOutput);
        rd = NULL;
        goto shell;
    }
    if (!(rd = ReadArgs("FROM,ARGS/M,COMMAND/K", arg, NULL))) { PrintFault(IoErr(), "csh"); return 20; }
    if (arg[2] || (arg[0] && lang_isscript((char *)arg[0]))) {
        /* csh COMMAND "text", or csh FROM a #!csh script [args]: run, end */
        LONG r;
        char **args = (char **)arg[1];
        int n = 0;
        while (args && args[n]) n++;
        csh.in = Input(); csh.out = Output(); csh.module = cli->cli_Module;
        r = lang_script((char *)arg[0], n, args, (char *)arg[2]);
        os_flush();
        FreeArgs(rd);
        /* the break relay and the jobs end with csh: their code is csh's (a
         * relay left behind waits in freed memory) */
        job_leave();
        lang_end();
        os_end();
        run_end();
        comp_end();             /* the templates * looked at (Dir takes one name) */
        if (!csh.module) cli->cli_Module = 0;
        return r;
    }
    if (arg[0] && !(from = Open((STRPTR)arg[0], MODE_OLDFILE))) {
        PrintFault(IoErr(), (STRPTR)arg[0]); FreeArgs(rd); return 20;
    }
    FreeArgs(rd);
shell:
    csh.in = Input();
    csh.out = Output();
    csh.module = cli->cli_Module;     /* 0 for the Shell itself: nothing of csh's to keep loaded */
    oldcur = cli->cli_CurrentInput;
    oldinter = cli->cli_Interactive;
    if (from) { csh.fromfile = 1; cli->cli_CurrentInput = from; }
    else if (!csh.usershell) {        /* the Shell itself reads what CliInit gave it (Shell-Startup first) */
        cli->cli_CurrentInput = cli->cli_StandardInput;
        if (IsInteractive(cli->cli_StandardInput)) SetMode(cli->cli_StandardInput, 0);
    }

    for (;;) {
        if (csh.quit) {
            /* exit with jobs running: said once; a second exit leaves them be */
            static int warned;
            if (job_running() && !warned && !csh.fromfile) {
                warned = 1;
                csh.quit = 0;
                FPuts(csh.out, "csh: jobs still run (jobs, fg, kill); exit again to leave them\n");
                continue;
            }
            break;
        }
        /* a background Shell (Run, System()) never has a prompt */
        int inter = !csh.fromfile && cli->cli_CurrentInput == cli->cli_StandardInput
                    && !(csh.usershell && cli->cli_Background);
        if (cli->cli_Background && !csh.bg0) break;       /* EndCLI / EndShell */
        /* System(): done when its command lines are (it never reads the console) */
        if (csh.system && cli->cli_CurrentInput == cli->cli_StandardInput) break;
        if (csh.fromfile && cli->cli_CurrentInput == cli->cli_StandardInput) break;
        cli->cli_Interactive = inter ? DOSTRUE : DOSFALSE;
        if (inter) {
            static int startup;
            int g;
            if (!startup) {     /* S:CShell-Startup once, at the first prompt (aliases, prompt...) */
                BPTR l;
                startup = 1;
                if ((l = Lock("S:CShell-Startup", SHARED_LOCK))) {
                    UnLock(l);
                    lang_source("S:CShell-Startup", 0, NULL);
                    os_flush();
                }
            }
            DBG(("cshell: interactive, out %lx\n", csh.out));
            if (editing < 0) {
                editing = term_possible(cli->cli_StandardInput, csh.out);
                if (editing) editor_start();
            }
            job_news();
            if (editing) {
                int n;
                SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F);
                /* lines typed ahead may already sit in the handle's buffer (the
                 * ROM Shell's FGets took them with its own line): raw reads go
                 * around it, so they are run from there. The rest typed ahead
                 * comes through raw mode as keys. */
                if (buffered_line(cli->cli_StandardInput)) {
                    if (!(g = getline_(cli->cli_StandardInput, line, sizeof line))) break;
                    if (g < 0) continue;
                    if (!lang_more && !bang(line, sizeof line)) continue;
                    hist_add(line);
                    runtyped(line);
                    SetSignal(0, SIGBREAKF_CTRL_C);
                    continue;
                }
                /* opened first: it finds out whether this is CTerm, which the
                 * prompt's colours depend on */
                if (!term_open(cli->cli_StandardInput, csh.out)) { editing = 0; continue; }
                {   /* the window's width for programs like ls (only when it changed) */
                    static int lastcols;
                    if (term_cols != lastcols) {
                        char b[12];
                        ltoa10(term_cols, b);
                        SetVar("COLUMNS", b, -1, LV_VAR | GVF_LOCAL_ONLY);
                        lastcols = term_cols;
                    }
                }
                {
                    static char rp[300];
                    edit_rprompt = lang_more ? NULL : varprompt("RPROMPT", NULL, 1, rp, sizeof rp);
                }
                n = edit_line(lang_more ? ps2() : prompt_text(1), line, sizeof line - 1);
                term_close();
                if (n == -2) { lang_reset(); continue; }
                if (n < 0) break;
                if (!lang_more && !bang(line, sizeof line)) continue;
                hist_add(line);
                runtyped(line);
                /* Ctrl-C in what a pasted line ran: the paste's other lines not run */
                if (csh.broke || (cli->cli_ReturnCode && cli->cli_Result2 == ERROR_BREAK)) edit_droppaste();
                SetSignal(0, SIGBREAKF_CTRL_C);
                continue;
            }
            FPuts(csh.out, lang_more ? ps2() : prompt_text(0));
            Flush(csh.out);
            if (!(g = getline_(cli->cli_CurrentInput, line, sizeof line))) break;
            if (g < 0) continue;
            if (!lang_more && !bang(line, sizeof line)) continue;
            runtyped(line);
            SetSignal(0, SIGBREAKF_CTRL_C);
            continue;
        }
        DBG(("cshell: script line\n"));
        if (!script_step(line, sizeof line)) break;
    }
    DBG(("cshell: loop ends\n"));
    lang_exittrap();
    os_flush();

    job_leave();
    if (!csh.module) cli->cli_Module = 0;   /* jobs still run csh's code: it stays in memory */
#ifdef CSH_USERSHELL
    if (csh.usershell && job_running()) _keepdata = 1;   /* and its data: they use it */
#endif
    if (editing > 0) { hist_save(); term_cleanup(); edit_end(); }
    lang_end();
    os_end();
    run_end();
    comp_end();
    if (!csh.usershell && !(cli->cli_Background && !csh.bg0)) {
        if (csh.fromfile && cli->cli_CurrentInput != cli->cli_StandardInput) script_end();
        cli->cli_CurrentInput = oldcur;
        cli->cli_Interactive = oldinter;
        if (csh.fromfile) cli->cli_FailLevel = oldfail;   /* script_end set 10: the calling script's own back */
    }
    return csh.rc;
}

/* A stack of its own: csh runs on the Shell's (often 4 KB), and the csh
 * language calls itself (functions, $(...)). Commands get their own stack
 * from RunCommand as always. */
#define STACKSZ 65536
static struct StackSwapStruct sss;
static int sret;

static void __attribute__((noinline)) runswapped(void) { sret = smain(); }

int main(void)
{
    struct Task *me = FindTask(NULL);
    char *big;
#ifdef CSH_USERSHELL
    if (!shell_init()) return 20;
#ifdef CSH_NOSMAIN
    shell_end();
    return 0;
#endif
#endif
    if ((char *)me->tc_SPUpper - (char *)me->tc_SPLower >= STACKSZ || !(big = AllocVec(STACKSZ, MEMF_ANY)))
        sret = smain();
    else {
        sss.stk_Lower = big;
        sss.stk_Upper = (ULONG)(big + STACKSZ);
        sss.stk_Pointer = (APTR)(big + STACKSZ);
        StackSwap(&sss);
        runswapped();
        StackSwap(&sss);
        FreeVec(big);
    }
#ifdef CSH_USERSHELL
    shell_end();
#endif
    return sret;
}
