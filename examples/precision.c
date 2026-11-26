// precision.c - float vs double, long long, and a variadic function
#include <stdio.h>
#include <stdarg.h>
#include <math.h>

double mean(int n, ...)
{
    va_list ap;
    double s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) s += va_arg(ap, double);
    va_end(ap);
    return s / n;
}

int main(void)
{
    float f = 1.0f / 3.0f;
    double d = 1.0 / 3.0;
    long long fact = 1;
    int i;
    printf("float : %.17f\n", f);
    printf("double: %.17f\n", d);
    printf("pi    : %.15f\n", M_PI);
    printf("sqrt2 : %.15f\n", sqrt(2.0));
    for (i = 1; i <= 20; i++) fact *= i;
    printf("20!   : %lld\n", fact);
    printf("2^62  : %lld\n", 1LL << 62);
    printf("mean  : %.3f\n", mean(4, 1.5, 2.5, 3.5, 4.5));
    return 0;
}
