/* bootshell - makes the ROM's BootShell the user shell again (the Guru
 * Book 14.5.4): after "Resident shell RAM:cshell REPLACE", for a control. */
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
int main(void)
{
    struct Segment *boot, *user;
    int ok = 0;
    Forbid();
    if ((boot = FindSegment("BootShell", NULL, TRUE)) && (user = FindSegment("shell", NULL, TRUE))) {
        user->seg_Seg = boot->seg_Seg;
        ok = 1;
    }
    Permit();
    PutStr(ok ? "the user shell is the BootShell again\n" : "not found\n");
    return ok ? 0 : 10;
}
