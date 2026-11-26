#include <stdio.h>
const char *name(int d) {
    switch (d) {
    case 0: return "zero";
    case 1: case 2: return "small";
    case 10: return "ten";
    default: return "other";
    }
}
int classify(int c) {
    int r = 0;
    switch (c) {
    case 'a': r += 1;
    case 'b': r += 10; break;
    case 'c': r += 100; break;
    }
    return r;
}
int main(void) {
    int i, n = 0;
    for (i = 0; i < 12; i++) printf("%s ", name(i));
    printf("\n%d %d %d %d\n", classify('a'), classify('b'), classify('c'), classify('d'));
    for (i = 0; i < 10; i++) {
        if (i % 2) continue;
        n += i;
        if (n > 10) goto done;
    }
done:
    printf("n=%d i=%d\n", n, i);
    {
        int x = 0;
        for (;;) { if (++x == 5) break; }
        printf("x=%d\n", x);
    }
    {
        int k = 3, total = 0;
        while (k > 0) {
            int j;
            for (j = 0; j < k; j++) {
                switch (j) { case 1: continue; default: total += j; }
            }
            k--;
        }
        printf("total=%d\n", total);
    }
    for (int q = 0; q < 3; q++) printf("q%d", q);
    printf("\n");
    return 0;
}
