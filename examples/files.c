// files.c - writing and reading a file on the SD card
#include <stdio.h>
#include <string.h>

int main(void)
{
    FILE *f = fopen("demo.txt", "w");
    char line[80];
    int n = 0;
    if (!f) { printf("cannot create demo.txt\n"); return 1; }
    fprintf(f, "CalcIDE file demo\n");
    for (n = 1; n <= 5; n++) fprintf(f, "line %d: %d squared is %d\n", n, n, n * n);
    fclose(f);

    f = fopen("demo.txt", "r");
    if (!f) { printf("cannot open demo.txt\n"); return 1; }
    n = 0;
    while (fgets(line, sizeof(line), f)) {
        printf("%2d| %s", ++n, line);
    }
    fclose(f);
    printf("read %d lines\n", n);
    return 0;
}
