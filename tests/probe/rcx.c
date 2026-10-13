/* rcx N - exit with return code N (and Result2 = 205, "object not found") */
#include <proto/dos.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    SetIoErr(205);
    return argc > 1 ? atoi(argv[1]) : 0;
}
