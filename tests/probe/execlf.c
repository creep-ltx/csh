/* execlf - what Execute() and SystemTags() do with a command string that
 * holds LFs: two Echo lines, then a Set and an Echo of the variable. */
#include <proto/dos.h>
#include <dos/dostags.h>
int main(void)
{
    BPTR out = Output();
    Printf("-- Execute:\n"); Flush(out);
    Execute("Echo one\nEcho two\nSet ZZ three\nEcho $ZZ", 0, out);
    Printf("-- SystemTags:\n"); Flush(out);
    SystemTags("Echo one\nEcho two\nSet ZZ three\nEcho $ZZ", SYS_Output, out, SYS_Input, 0, TAG_END);
    Printf("-- SystemTags with ;:\n"); Flush(out);
    SystemTags("Echo one ; Echo two", SYS_Output, out, SYS_Input, 0, TAG_END);
    return 0;
}
