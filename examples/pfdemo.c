// pfdemo.c - keep a boot counter in the flash page file
#include <stdio.h>

int main(void)
{
    int count = 0;
    if (pf_read("demo/count", &count, sizeof(count)) != sizeof(count)) count = 0;
    count++;
    pf_write("demo/count", &count, sizeof(count));
    printf("This program has run %d time(s).\n", count);
    printf("(stored in the page file - try 'pf tree')\n");
    return 0;
}
