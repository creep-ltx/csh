/* runint [-in FILE] NAME [ARGS...] - run a ROM internal command (seg_UC
 * -2) with RunCommand on this process, as csh would, then show what it
 * changed: rc, IoErr, the current dir, FailLevel, the variable FOO, and
 * with -in FILE (made cli_CurrentInput first) the lines left unread. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include <dos/var.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    struct CommandLineInterface *cli = Cli();
    struct Segment *sg; BPTR seg = 0, in = 0, oldin = 0;
    char args[256] = "", buf[256]; int i = 1; LONG rc;
    if (!cli) { printf("no CLI\n"); return 20; }
    int inter = 0;
    if (argc > 2 && !strcmp(argv[1], "-in")) { in = Open(argv[2], MODE_OLDFILE); i = 3; }
    else if (argc > 1 && !strcmp(argv[1], "-inter")) { inter = 1; i = 2; }
    if (i >= argc) return 20;
    for (int j = i + 1; j < argc; j++) { strcat(args, argv[j]); if (j < argc - 1) strcat(args, " "); }
    strcat(args, "\n");
    Forbid();
    sg = FindSegment(argv[i], NULL, TRUE);
    if (sg) seg = sg->seg_Seg;
    Permit();
    printf("seg %s: UC %ld seg %lx\n", argv[i], sg ? (long)sg->seg_UC : 0L, (unsigned long)seg);
    if (!seg) return 20;
    oldin = cli->cli_CurrentInput;
    if (in) cli->cli_CurrentInput = in;
    if (inter) cli->cli_CurrentInput = cli->cli_StandardInput;
    printf("CurrentInput before %lx (Standard %lx)\n", (unsigned long)cli->cli_CurrentInput,
           (unsigned long)cli->cli_StandardInput);
    Flush(Output());
    SetIoErr(0);
    rc = RunCommand(seg, 16000, args, strlen(args));
    Flush(Output());
    printf("rc %ld IoErr %ld ReturnCode %ld FailLevel %ld\n", (long)rc, (long)IoErr(),
           (long)cli->cli_ReturnCode, (long)cli->cli_FailLevel);
    if (GetCurrentDirName(buf, sizeof buf)) printf("dir %s\n", buf);
    if (GetVar("FOO", buf, sizeof buf, GVF_LOCAL_ONLY) >= 0) printf("FOO=%s\n", buf);
    printf("CurrentInput after %lx CommandFile %s\n", (unsigned long)cli->cli_CurrentInput,
           cli->cli_CommandFile ? (char *)BADDR(cli->cli_CommandFile) + 1 : "-");
    if (cli->cli_CurrentInput != oldin) {
        BPTR fh = cli->cli_CurrentInput;
        while (FGets(fh, buf, sizeof buf)) printf("left: %s", buf);
        if (fh != in && fh != cli->cli_StandardInput) Close(fh);
        cli->cli_CurrentInput = oldin;
    }
    /* in may have been closed by a nested Execute: never close it twice */
    if (inter) cli->cli_CurrentInput = oldin;
    return 0;
}
