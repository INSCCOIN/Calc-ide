#include <stdio.h>
#include <stdarg.h>
#include <setjmp.h>
#include <string.h>
int sum(int n, ...) {
    va_list ap;
    int i, s = 0;
    va_start(ap, n);
    for (i = 0; i < n; i++) s += va_arg(ap, int);
    va_end(ap);
    return s;
}
double avg(int n, ...) {
    va_list ap;
    double s = 0;
    int i;
    va_start(ap, n);
    for (i = 0; i < n; i++) s += va_arg(ap, double);
    va_end(ap);
    return s / n;
}
void logmsg(const char *level, const char *fmt, ...) {
    va_list ap;
    char buf[100];
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    printf("[%s] %s\n", level, buf);
}
int mixed(const char *types, ...) {
    va_list ap;
    int n = 0;
    va_start(ap, types);
    while (*types) {
        if (*types == 'i') printf("i=%d ", va_arg(ap, int));
        else if (*types == 'd') printf("d=%.2f ", va_arg(ap, double));
        else if (*types == 's') printf("s=%s ", va_arg(ap, char *));
        else if (*types == 'l') printf("l=%lld ", va_arg(ap, long long));
        types++;
        n++;
    }
    va_end(ap);
    printf("\n");
    return n;
}
jmp_buf env;
int depth;
void dive(int n) {
    depth = n;
    if (n == 5) longjmp(env, 42);
    dive(n + 1);
}
int (*fp)(int, ...) = sum;
int main(void) {
    int r;
    printf("%d %d %d\n", sum(3, 1, 2, 3), sum(0), sum(5, 10, 20, 30, 40, 50));
    printf("%.3f\n", avg(4, 1.0, 2.0, 3.5, 4.5));
    printf("%.3f\n", avg(2, 1.5f, 2.0));
    logmsg("info", "%d apples and %s, %.1f%%", 3, "pears", 99.5);
    printf("%d\n", mixed("idsl", 7, 2.5, "hi", 123456789012LL));
    printf("%d\n", fp(2, 100, 23));
    r = setjmp(env);
    if (r == 0) {
        dive(1);
        printf("not reached\n");
    } else {
        printf("longjmp returned %d at depth %d\n", r, depth);
    }
    return 0;
}
