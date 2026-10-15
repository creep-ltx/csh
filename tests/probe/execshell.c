/* execshell - points the BootShell entry (the Shell Execute() and a plain
 * System() start) at the "shell" entry's code: after cshell is the Shell,
 * every Shell is cshell then. Only after the boot (the boot Shell needs
 * the ROM's: docs/design.md "csh in a ROM"). A reboot undoes it. */
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
int main(void)
{
    struct Segment *boot, *user;
    int ok = 0;
    Forbid();
    boot = FindSegment("BootShell", NULL, TRUE);
    user = FindSegment("shell", NULL, TRUE);
    if (boot && user) { boot->seg_Seg = user->seg_Seg; ok = 1; }
    Permit();
    PutStr(ok ? "BootShell runs the \"shell\" entry's code now\n" : "not found\n");
    return ok ? 0 : 10;
}
