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
#include <dos/rdargs.h>
#include <dos/var.h>
#include <stdio.h>
#include <string.h>
#include "csh.h"
#include "edit.h"
#include "lang.h"

static const char version[] = "$VER: csh " CSH_VERSION " (" CSH_DATE ")";

struct csh csh;

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

static void editor_start(void)
{
    keys_load();
    prefs_load();
    hist_load();
    if (keys_bad[0]) { FPuts(csh.out, keys_bad); FPutC(csh.out, '\n'); }
}

static int smain(void)
{
    static char line[LINE_MAX];
    struct CommandLineInterface *cli;
    struct RDArgs *rd;
    LONG arg[3] = { 0, 0, 0 };
    BPTR from = 0, oldcur;
    LONG oldinter, bg0;

    (void)version;
    csh.me = (struct Process *)FindTask(NULL);
    if (!(cli = csh.cli = Cli())) return 20;
    if (!(rd = ReadArgs("FROM,ARGS/M,COMMAND/K", arg, NULL))) { PrintFault(IoErr(), "csh"); return 20; }
    if (arg[2] || (arg[0] && lang_isscript((char *)arg[0]))) {
        /* csh COMMAND "text", or csh FROM a #!csh script [args]: run, end */
        LONG r;
        char **args = (char **)arg[1];
        int n = 0;
        while (args && args[n]) n++;
        csh.in = Input(); csh.out = Output(); csh.module = cli->cli_Module;
        r = arg[2] ? lang_eval((char *)arg[2]) : lang_source((char *)arg[0], n, args);
        os_flush();
        FreeArgs(rd);
        return r;
    }
    if (arg[0] && !(from = Open((STRPTR)arg[0], MODE_OLDFILE))) {
        PrintFault(IoErr(), (STRPTR)arg[0]); FreeArgs(rd); return 20;
    }
    FreeArgs(rd);
    csh.in = Input();
    csh.out = Output();
    csh.module = cli->cli_Module;
    oldcur = cli->cli_CurrentInput;
    oldinter = cli->cli_Interactive;
    bg0 = cli->cli_Background;
    if (from) { csh.fromfile = 1; cli->cli_CurrentInput = from; }
    else {
        cli->cli_CurrentInput = cli->cli_StandardInput;
        if (IsInteractive(cli->cli_StandardInput)) SetMode(cli->cli_StandardInput, 0);
    }

    while (!csh.quit) {
        int inter = !csh.fromfile && cli->cli_CurrentInput == cli->cli_StandardInput;
        if (cli->cli_Background && !bg0) break;           /* EndCLI / EndShell */
        if (csh.fromfile && cli->cli_CurrentInput == cli->cli_StandardInput) break;
        cli->cli_Interactive = inter ? DOSTRUE : DOSFALSE;
        if (inter) {
            if (editing < 0) {
                editing = term_possible(cli->cli_StandardInput, csh.out);
                if (editing) editor_start();
            }
            if (editing) {
                int n;
                SetSignal(0, SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E | SIGBREAKF_CTRL_F);
                /* lines typed ahead may already sit in the handle's buffer (the
                 * ROM Shell's FGets took them with its own line): raw reads go
                 * around it, so they are run from there. The rest typed ahead
                 * comes through raw mode as keys. */
                if (buffered_line(cli->cli_StandardInput)) {
                    if (!FGets(cli->cli_StandardInput, line, sizeof line - 1)) break;
                    hist_add(line);
                    lang_line(line, 1);
                    os_flush();
                    SetSignal(0, SIGBREAKF_CTRL_C);
                    continue;
                }
                /* opened first: it finds out whether this is CTerm, which the
                 * prompt's colours depend on */
                if (!term_open(cli->cli_StandardInput, csh.out)) { editing = 0; continue; }
                n = edit_line(lang_more ? "> " : prompt_text(1), line, sizeof line - 1);
                term_close();
                if (n == -2) { lang_reset(); continue; }
                if (n < 0) break;
                hist_add(line);
                lang_line(line, 1);
                os_flush();
                SetSignal(0, SIGBREAKF_CTRL_C);
                continue;
            }
            FPuts(csh.out, lang_more ? "> " : prompt_text(0));
            Flush(csh.out);
            if (!FGets(cli->cli_CurrentInput, line, sizeof line - 1)) break;
            lang_line(line, 1);
            os_flush();
            SetSignal(0, SIGBREAKF_CTRL_C);
            continue;
        }
        else if (SetSignal(0, SIGBREAKF_CTRL_D) & SIGBREAKF_CTRL_D) {
            Printf("***Break\n");
            script_end();
            continue;
        }
        if (!FGets(cli->cli_CurrentInput, line, sizeof line - 1)) {
            if (cli->cli_CurrentInput != cli->cli_StandardInput) { script_end(); continue; }
            break;                                         /* Ctrl-\ on the console */
        }
        if (!inter) {
            /* FindVar: the local list only, no copy, no ENV: file opened */
            struct LocalVar *e = FindVar("echo", LV_VAR);
            if (e && e->lv_Len >= 2 && (e->lv_Value[0] | 32) == 'o' && (e->lv_Value[1] | 32) == 'n')
                FPuts(csh.out, line);
        }
        /* "+" at the end joins the next line, with the LF kept (line.c) */
        for (;;) {
            int n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
            if (!n || line[n - 1] != '+' || n + 2 >= (int)sizeof line) break;
            line[n - 1] = '\n';
            if (!FGets(cli->cli_CurrentInput, line + n, sizeof line - n - 1)) { line[n] = 0; break; }
        }
        run_line(line, 0);
        SetSignal(0, SIGBREAKF_CTRL_C);
    }

    if (editing > 0) { hist_save(); term_cleanup(); }
    if (!(cli->cli_Background && !bg0)) {
        if (csh.fromfile && cli->cli_CurrentInput != cli->cli_StandardInput) script_end();
        cli->cli_CurrentInput = oldcur;
        cli->cli_Interactive = oldinter;
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
    if ((char *)me->tc_SPUpper - (char *)me->tc_SPLower >= STACKSZ || !(big = AllocVec(STACKSZ, MEMF_ANY)))
        return smain();
    sss.stk_Lower = big;
    sss.stk_Upper = (ULONG)(big + STACKSZ);
    sss.stk_Pointer = (APTR)(big + STACKSZ);
    StackSwap(&sss);
    runswapped();
    StackSwap(&sss);
    FreeVec(big);
    return sret;
}
