/* segs - the resident list's system entries (shell, BootShell, CLI) and
 * where their seg_Seg points (BPTR and address): which ROM segment is which. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dosextens.h>
int main(void)
{
    struct DosInfo *di = BADDR(((struct RootNode *)DOSBase->dl_Root)->rn_Info);
    BPTR s;
    Forbid();
    for (s = *(BPTR *)((UBYTE *)di + 0x10); s; s = ((struct Segment *)BADDR(s))->seg_Next) {
        struct Segment *sg = BADDR(s);
        if (sg->seg_UC == CMD_SYSTEM) {
            char n[40]; int l = sg->seg_Name[0] < 39 ? sg->seg_Name[0] : 39;
            CopyMem(sg->seg_Name + 1, n, l); n[l] = 0;
            Permit();
            Printf("%-10s seg %08lx = %08lx\n", (LONG)n, sg->seg_Seg, (LONG)BADDR(sg->seg_Seg));
            Forbid();
        }
    }
    Permit();
    return 0;
}
