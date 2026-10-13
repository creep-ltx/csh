/* pcat - copy Input() to Output(), then say how many bytes ("pcat N") */
#include <proto/dos.h>
int main(void)
{
    char b[256]; LONG n, t = 0;
    while ((n = Read(Input(), b, sizeof b)) > 0) { Write(Output(), b, n); t += n; }
    Printf("pcat %ld\n", t);
    return 0;
}
