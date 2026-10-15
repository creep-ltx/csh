/* useshell - makes every Shell after the boot csh: after
 *     Resident shell L:csh REPLACE
 * (S:User-Startup, or by hand to update), the code just loaded becomes
 * the system's "shell" entry (NewShell, Run, System() with the user
 * shell) and Execute()'s Shell, the BootShell entry (Execute(), a plain
 * System() - CTerm's tabs).
 *
 * Why a tool: Resident REPLACE swaps the system "shell" entry only while
 * it is the ROM's; once csh is the Shell it adds a plain resident entry
 * named "shell" instead, which DOS never asks for - an update would not
 * reach new Shells. And nothing points BootShell anywhere else. The boot
 * Shell already runs (the ROM's code): changing the entries does not touch
 * it. The old code stays in memory (Shells still open run it) until a
 * reboot. docs/design.md "One csh". */
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
int main(void)
{
    struct Segment *sys, *user, *boot;
    BPTR code = 0;
    Forbid();
    sys = FindSegment("shell", NULL, TRUE);
    user = FindSegment("shell", NULL, FALSE);
    boot = FindSegment("BootShell", NULL, TRUE);
    if (sys && boot) {
        code = user ? user->seg_Seg : sys->seg_Seg;     /* the newest REPLACE, else the system's */
        sys->seg_Seg = code;
        boot->seg_Seg = code;
    }
    Permit();
    if (!code) { PutStr("useshell: the shell entries are not there\n"); return 10; }
    PutStr("every new Shell runs the resident \"shell\" now\n");
    return 0;
}
