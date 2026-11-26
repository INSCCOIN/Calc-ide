#include <stdio.h>
#define SQUARE(x) ((x) * (x))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define N 4
#define GREETING "hi"
## this is a comment in the CalcIDE dialect
#ifdef N
int have_n = 1;
#else
int have_n = 0;
#endif
#if N > 3 && defined(SQUARE)
int big = 1;
#elif N > 1
int big = 2;
#else
int big = 3;
#endif
#ifndef UNDEFINED_THING
#define LATE 42
#endif
#undef GREETING
#define GREETING "hello"
int main(void) {
    int a[N];
    int i;
    for (i = 0; i < N; i++) a[i] = SQUARE(i + 1);
    printf("%d %d %d %d\n", a[0], a[1], a[2], a[3]);
    printf("%d %d %d\n", MAX(3, 7), MAX(MAX(1, 9), 4), SQUARE(SQUARE(2)));
    printf("%s %d %d %d\n", GREETING, have_n, big, LATE);
    /* block comment */ // line comment
    printf("line %d\n", __LINE__);
#if 0
    printf("never\n");
#endif
    return 0;
}
