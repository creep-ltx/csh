/* findres NAME - exec's resident module of that name: where, its version, its id. */
#include <exec/resident.h>
#include <proto/exec.h>
#include <proto/dos.h>
int main(void)
{
    LONG a[1] = { 0 };
    struct RDArgs *rd = ReadArgs("NAME/A", a, NULL);
    struct Resident *r;
    if (!rd) { PrintFault(IoErr(), "findres"); return 10; }
    if ((r = FindResident((STRPTR)a[0])))
        Printf("%s at %08lx: version %ld, %s", a[0], (LONG)r, (LONG)r->rt_Version, (LONG)r->rt_IdString);
    else Printf("%s: no such module\n", a[0]);
    FreeArgs(rd);
    return r ? 0 : 5;
}
