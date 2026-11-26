// fib.c - recursion and loops
#include <stdio.h>

int fib(int n)
{
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

int main(void)
{
    int i;
    int t0 = millis();
    for (i = 0; i <= 20; i++)
        printf("fib(%2d) = %d\n", i, fib(i));
    printf("took %d ms\n", millis() - t0);
    return 0;
}
