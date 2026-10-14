/* phead N - copy the first N lines of Input() to Output(), then end (a
 * reader that leaves early: what is still written into the pipe after it).
 * Read() a byte at a time: RunCommand leaves the argument line in Input()'s
 * buffer, which FGets would hand back as the first line. */
#include <proto/dos.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    char c;
    int n = argc > 1 ? atoi(argv[1]) : 10;
    while (n > 0 && Read(Input(), &c, 1) == 1) { Write(Output(), &c, 1); if (c == '\n') n--; }
    PutStr("phead done\n");
    return 0;
}
