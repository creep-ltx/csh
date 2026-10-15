/* useshell - after "Resident shell FILE REPLACE" while cshell is already
 * the Shell: makes that new code the one new Shells run.
 *
 * Resident REPLACE swaps the system's "shell" entry only while it is the
 * ROM's; once cshell is the Shell it adds a plain resident entry named
 * "shell" instead, which DOS never asks for (found 2026-10-15: NewShell
 * kept the old cshell). This copies that entry's code into the system
 * entry. The old code stays in memory (Shells still open run it) until a
 * reboot. */
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
int main(void)
{
    struct Segment *sys, *user;
    int ok = 0;
    Forbid();
    if ((sys = FindSegment("shell", NULL, TRUE)) && (user = FindSegment("shell", NULL, FALSE))
        && sys->seg_Seg != user->seg_Seg) {
        sys->seg_Seg = user->seg_Seg;
        ok = 1;
    }
    Permit();
    PutStr(ok ? "new Shells run the resident \"shell\" now\n"
              : "nothing to do (no resident \"shell\", or it is the Shell already)\n");
    return ok ? 0 : 5;
}
