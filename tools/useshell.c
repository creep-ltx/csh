/* useshell FILE - makes every Shell after the boot run FILE (L:csh): it is
 * loaded and the system's "shell" entry (NewShell, Run, System() with the
 * user shell) and Execute()'s Shell, the BootShell entry (Execute(), a plain
 * System() - CTerm's tabs), get its code. In S:User-Startup, and by hand to
 * update.
 *
 * Not "Resident shell FILE REPLACE": REPLACE swaps the system entry only
 * while it is the ROM's, else it adds a plain entry named "shell", and on
 * the next REPLACE it frees that entry's code - which is the live Shell's
 * by then: every running Shell went on in freed memory (it took the test
 * machine down, 2026-10-15). So the code is loaded here and never freed:
 * Shells already open run the old code, which stays in memory until a
 * reboot. The boot Shell is the ROM's and runs on (entries changed, not it).
 * The file must be csh ("$VER: csh" in it). docs/design.md "One csh". */
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <string.h>

static int iscsh(BPTR seg)
{
    static const char want[] = "$VER: csh ";
    BPTR h;
    for (h = seg; h; h = *(BPTR *)BADDR(h)) {
        const char *d = (const char *)BADDR(h) + 4;
        ULONG n = ((ULONG *)BADDR(h))[-1], i;
        if (n < 16) continue;
        for (n -= 8, i = 0; i + sizeof want - 1 <= n; i++)
            if (d[i] == '$' && !memcmp(d + i, want, sizeof want - 1)) return 1;
    }
    return 0;
}

int main(void)
{
    LONG arg[1] = { 0 };
    struct RDArgs *rd = ReadArgs("FILE/A", arg, NULL);
    struct Segment *sys, *boot;
    BPTR seg;
    int ok = 0;
    if (!rd) { PrintFault(IoErr(), "useshell"); return 10; }
    if (!(seg = LoadSeg((STRPTR)arg[0]))) { PrintFault(IoErr(), (STRPTR)arg[0]); FreeArgs(rd); return 10; }
    if (!iscsh(seg)) {
        Printf("useshell: %s is not csh\n", arg[0]);
        UnLoadSeg(seg);
        FreeArgs(rd);
        return 10;
    }
    Forbid();
    sys = FindSegment("shell", NULL, TRUE);
    boot = FindSegment("BootShell", NULL, TRUE);
    if (sys && boot) {
        sys->seg_Seg = seg;
        boot->seg_Seg = seg;
        ok = 1;
    }
    Permit();
    if (!ok) { PutStr("useshell: the shell entries are not there\n"); UnLoadSeg(seg); FreeArgs(rd); return 10; }
    FreeArgs(rd);
    return 0;
}
