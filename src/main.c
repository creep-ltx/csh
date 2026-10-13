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
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/rdargs.h>
#include <stdio.h>
#include <string.h>
#include "csh.h"

static const char version[] = "$VER: csh " CSH_VERSION " (" CSH_DATE ")";

struct csh csh;

/* The prompt from cli_Prompt (set by the ROM's Prompt command): %N the CLI
 * number, %S or %s the current dir, %R the last return code, %% a percent
 * sign; other escapes print nothing (as the 3.2 ROM Shell does). */
static void prompt(void)
{
    UBYTE *bp = BADDR(csh.cli->cli_Prompt);
    char p[256], out[600], dir[256];
    int n = bp ? bp[0] : 0, i, o = 0;
    if (n == 0) { strcpy(p, "%N.%S> "); n = 7; }
    else { memcpy(p, bp + 1, n); p[n] = 0; }
    for (i = 0; i < n && o < (int)sizeof out - 260; i++) {
        if (p[i] != '%' || i + 1 >= n) { out[o++] = p[i]; continue; }
        switch (p[++i]) {
        case 'N': case 'n':
            o += sprintf(out + o, "%ld", (long)csh.me->pr_TaskNum); break;
        case 'S': case 's':
            if (!GetCurrentDirName(dir, sizeof dir)) dir[0] = 0;
            strcpy(out + o, dir); o += strlen(dir); break;
        case 'R': case 'r':
            o += sprintf(out + o, "%ld", (long)csh.cli->cli_ReturnCode); break;
        case '%': out[o++] = '%'; break;
        default: break;
        }
    }
    Write(csh.out, out, o);
}

int main(void)
{
    static char line[LINE_MAX];
    struct CommandLineInterface *cli;
    struct RDArgs *rd;
    LONG arg[1] = { 0 };
    BPTR from = 0, oldcur;
    LONG oldinter, bg0;

    (void)version;
    csh.me = (struct Process *)FindTask(NULL);
    if (!(cli = csh.cli = Cli())) return 20;
    if (!(rd = ReadArgs("FROM", arg, NULL))) { PrintFault(IoErr(), "csh"); return 20; }
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
        if (inter) prompt();
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
            char e[8];
            if (GetVar("echo", e, sizeof e, 0) > 0 && !stricmp(e, "on")) FPuts(csh.out, line);
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

    if (!(cli->cli_Background && !bg0)) {
        if (csh.fromfile && cli->cli_CurrentInput != cli->cli_StandardInput) script_end();
        cli->cli_CurrentInput = oldcur;
        cli->cli_Interactive = oldinter;
    }
    return csh.rc;
}
