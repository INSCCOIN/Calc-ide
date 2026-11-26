#include <stdio.h>
#include <stdlib.h>
int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }
int mul(int a, int b) { return a * b; }
int (*ops[3])(int, int) = {add, sub, mul};
typedef int (*binop)(int, int);
int apply(binop f, int a, int b) { return f(a, b); }
int cmp(void *a, void *b) { return *(int *)a - *(int *)b; }
int main(void) {
    int i;
    int v[8] = {5, 3, 9, 1, 7, 2, 8, 6};
    binop f = sub;
    for (i = 0; i < 3; i++) printf("%d ", ops[i](6, 3));
    printf("\n%d %d %d\n", apply(add, 2, 3), f(10, 4), (*f)(1, 1));
    qsort(v, 8, sizeof(int), cmp);
    for (i = 0; i < 8; i++) printf("%d ", v[i]);
    printf("\n");
    return 0;
}
