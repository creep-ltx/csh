/* sigs - prints this process's allocated signal bits (tc_SigAlloc), in hex:
 * a command runs on its Shell's process, so this is the Shell's. */
#include <proto/exec.h>
#include <proto/dos.h>
int main(void)
{
    Printf("sigalloc %08lx\n", FindTask(NULL)->tc_SigAlloc);
    return 0;
}
