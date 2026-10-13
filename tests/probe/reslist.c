/* reslist - print the resident list: use count and name. seg_UC -2 =
 * internal (CMD_INTERNAL), -1 = system, -999 = disabled. The list's
 * header is at DosInfo+$10 (Knowledge/amiga/amigados-handlers.md). */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static char names[200][40]; static LONG ucs[200]; int n = 0, i;
    struct RootNode *rn = (struct RootNode *)DOSBase->dl_Root;
    struct DosInfo *di = BADDR(rn->rn_Info);
    BPTR s;
    Forbid();
    s = *(BPTR *)((UBYTE *)di + 0x10);
    while (s && n < 200) {
        struct Segment *sg = BADDR(s);
        UBYTE *b = sg->seg_Name;
        int l = b[0] < 39 ? b[0] : 39;
        memcpy(names[n], b + 1, l); names[n][l] = 0;
        ucs[n] = sg->seg_UC; n++;
        s = sg->seg_Next;
    }
    Permit();
    for (i = 0; i < n; i++) printf("%5ld %s\n", (long)ucs[i], names[i]);
    return 0;
}
