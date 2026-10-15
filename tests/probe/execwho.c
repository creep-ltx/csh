/* execwho - starts RAM:whoshell three ways and says which Shell ran it:
 * Execute(), System() as is (the BootShell), System() with SYS_UserShell. */
#include <proto/dos.h>
#include <dos/dostags.h>
int main(void)
{
    BPTR out = Output();
    LONG rc;
    PutStr("Execute():            "); Flush(out);
    Execute("RAM:whoshell", 0, out);
    PutStr("System():             "); Flush(out);
    rc = SystemTags("RAM:whoshell", SYS_Output, out, SYS_Input, 0, TAG_END);
    PutStr("System() user shell:  "); Flush(out);
    rc |= SystemTags("RAM:whoshell", SYS_Output, out, SYS_Input, 0, SYS_UserShell, TRUE, TAG_END);
    PutStr("System() rc of Quit 7: "); Flush(out);
    rc = SystemTags("Quit 7", SYS_Output, out, SYS_Input, 0, TAG_END);
    Printf("%ld\n", rc);
    return 0;
}
