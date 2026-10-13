/* peek ADDR [N] - N longwords (default 4) at a hex address. */
#include <proto/exec.h>
#include <proto/dos.h>
int main(void)
{
    LONG a[2] = { 0, 0 }, n = 4, i;
    struct RDArgs *rd = ReadArgs("ADDR/A,N/N", a, NULL);
    ULONG addr = 0;
    char *s;
    if (!rd) { PrintFault(IoErr(), "peek"); return 10; }
    for (s = (char *)a[0]; *s; s++) {
        int d = *s >= '0' && *s <= '9' ? *s - '0' : (*s | 32) >= 'a' && (*s | 32) <= 'f' ? (*s | 32) - 'a' + 10 : -1;
        if (d >= 0) addr = addr * 16 + d;
    }
    if (a[1]) n = *(LONG *)a[1];
    for (i = 0; i < n; i++) Printf("%08lx: %08lx\n", addr + i * 4, ((ULONG *)addr)[i]);
    FreeArgs(rd);
    return 0;
}
