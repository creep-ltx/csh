/* whoshell - which Shell's code this command runs in: the shell process's
 * pr_SegList entries 3 and 4 (cshell moves its own from 3 to 4), "ROM"
 * for an address in Kickstart, else "RAM (cshell?)". */
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
static const char *where(BPTR b)
{
    ULONG a = (ULONG)BADDR(b);
    if (!b) return "-";
    return a >= 0xF80000 && a < 0x1000000 ? "ROM" : "RAM";
}
int main(void)
{
    struct Process *pr = (struct Process *)FindTask(NULL);
    BPTR *seg = BADDR(pr->pr_SegList);
    Printf("seg3 %s seg4 %s\n", (LONG)where(seg[3]), (LONG)where(seg[4]));
    return 0;
}
