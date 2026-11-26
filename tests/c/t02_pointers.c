#include <stdio.h>
#include <string.h>
void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
int sum(int *p, int n) { int s = 0; while (n--) s += *p++; return s; }
void fill(char **out, int n) { int i; for (i = 0; i < n; i++) out[i] = "x"; }
int grid[3][4];
int main(void) {
    int a = 1, b = 2, i, j;
    int v[6] = {5, 4, 3, 2, 1, 0};
    int *p = v, **pp = &p;
    char *names[3];
    char buf[32];
    char *s;
    swap(&a, &b);
    printf("%d %d\n", a, b);
    printf("%d %d %d\n", sum(v, 6), *(v + 2), p[4]);
    (*pp)++;
    printf("%d %d\n", **pp, *p);
    printf("diff %d\n", (int)(&v[5] - &v[1]));
    fill(names, 3);
    printf("%s%s%s\n", names[0], names[1], names[2]);
    for (i = 0; i < 3; i++) for (j = 0; j < 4; j++) grid[i][j] = i * 10 + j;
    printf("%d %d %d\n", grid[1][2], grid[2][3], *(*(grid + 2) + 1));
    strcpy(buf, "hello");
    strcat(buf, " world");
    printf("%s %d\n", buf, (int)strlen(buf));
    s = buf;
    while (*s) { if (*s == 'o') *s = '0'; s++; }
    printf("%s\n", buf);
    printf("%d\n", strcmp("abc", "abd") < 0);
    printf("%s\n", strchr(buf, 'w'));
    {
        char *q = buf + 3;
        printf("%c", q[-1]);
        printf(" %c\n", *--q);
        printf("%d\n", q > buf && q < buf + 10);
    }
    {
        int *np = 0;
        printf("null %d\n", np == NULL);
    }
    return 0;
}
