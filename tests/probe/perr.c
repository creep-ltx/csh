/* perr - write "to-out" to Output() and "to-err" to the error stream
 * (pr_CES, falling back to Output() as dos does), then show both handles */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
int main(void)
{
    struct Process *p = (struct Process *)FindTask(NULL);
    BPTR err = p->pr_CES ? p->pr_CES : Output();
    FPuts(Output(), "to-out\n"); Flush(Output());
    FPuts(err, "to-err\n"); Flush(err);
    Printf("CES %s, COS %s\n", p->pr_CES ? "set" : "0", p->pr_COS == Output() ? "= Output()" : "other");
    return 0;
}
