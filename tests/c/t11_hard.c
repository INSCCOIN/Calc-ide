#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define SWAP(T, a, b) do { T _t = (a); (a) = (b); (b) = _t; } while (0)
#define BIT(n) (1u << (n))
#define LONG_MACRO(x) \
    ((x) * 2 + \
     1)
#if (3 * 4) % 5 == 2 && !defined(NOPE)
#define CHECK_IF 1
#else
#define CHECK_IF 0
#endif
typedef enum { OP_ADD, OP_MUL, OP_COUNT } OpKind;
typedef struct { const char *name; int (*fn)(int, int); } Op;
int f_add(int a, int b) { return a + b; }
int f_mul(int a, int b) { return a * b; }
Op optab[OP_COUNT] = {{"add", f_add}, {"mul", f_mul}};
int (*getop(char c))(int, int) { return c == '+' ? f_add : f_mul; }
struct inner { short a; char b[3]; };
struct outer { int id; struct inner in[2]; union { int i; float f; } u; };
struct outer table[2] = {{1, {{10, "ab"}, {20, "cd"}}, {5}}, {2, {{30, "ef"}, {40, "gh"}}, {6}}};
extern int later;
int forward(int);
void quicksort(int *a, int lo, int hi) {
    int i = lo, j = hi, p = a[(lo + hi) / 2];
    while (i <= j) {
        while (a[i] < p) i++;
        while (a[j] > p) j--;
        if (i <= j) { SWAP(int, a[i], a[j]); i++; j--; }
    }
    if (lo < j) quicksort(a, lo, j);
    if (i < hi) quicksort(a, i, hi);
}
int count_bits(unsigned v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
char upper(char c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
int later = 99;
int forward(int x) { return x + later; }
int *find(int *arr, int n, int v) { int i; for (i = 0; i < n; i++) if (arr[i] == v) return &arr[i]; return NULL; }
int main(void) {
    int v[10] = {9, 3, 7, 1, 8, 2, 6, 0, 5, 4};
    int i, x = 1;
    char name[2][8] = {"alpha", "beta"};
    char (*pn)[8] = name;
    void *raw = v;
    quicksort(v, 0, 9);
    for (i = 0; i < 10; i++) printf("%d", v[i]);
    printf("\n%s %d %s %d\n", optab[0].name, optab[0].fn(3, 4), optab[1].name, optab[1].fn(3, 4));
    printf("%d %d\n", getop('+')(5, 6), getop('*')(5, 6));
    printf("%d %c %d %s\n", table[1].in[0].a, table[0].in[1].b[1], table[1].u.i, table[0].in[0].b);
    printf("bits %d %d %u\n", count_bits(0xF0F0), count_bits(BIT(31) | 1), BIT(4));
    printf("macro %d %d\n", LONG_MACRO(5), CHECK_IF);
    printf("%c%c%c\n", upper('h'), upper('i'), upper('!'));
    printf("fwd %d\n", forward(1));
    printf("%s %s %c\n", pn[0], pn[1], (*(pn + 1))[2]);
    printf("raw %d\n", *((int *)raw + 3));
    printf("find %d %d\n", *find(v, 10, 7), find(v, 10, 42) == NULL);
    {
        int x = 2;
        {
            int x = 3;
            printf("shadow %d", x);
        }
        printf(" %d", x);
    }
    printf(" %d\n", x);
    {
        int a = 5, b = 10, c;
        c = a > b ? a : b > 7 ? b * 2 : b;
        printf("nested ternary %d\n", c);
        for (a = 0, b = 10; a < b; a += 3, b -= 2) ;
        printf("comma for %d %d\n", a, b);
        printf("sizes %d %d %d %d\n", (int)sizeof(struct outer), (int)sizeof(table) / (int)sizeof(table[0]), (int)sizeof v[0], (int)sizeof(OpKind));
    }
    {
        float f = 7.9f;
        unsigned u = (unsigned)f;
        char ch = (char)(int)(f * 10);
        printf("casts %u %d %d %.2f\n", u, ch, (int)-f, (float)u / 2);
    }
    {
        char buf[16];
        strcpy(buf, "abc");
        printf("%d %d %d\n", (int)strlen(buf), buf[3] == '\0', 'z' - 'a');
    }
    {
        static int sarr[3] = {7, 8, 9};
        int *p = sarr, *q = &sarr[2];
        printf("ptrs %d %d %d %d\n", (int)(q - p), *q - *p, p < q, *(p + 1));
    }
    printf("neg %d %d %d\n", -7 % 3, 7 % -3, -7 / 2);
    return 0;
}
