#include <stdio.h>
#include <math.h>
#include <stdlib.h>
long long fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
double area(double r) { return M_PI * r * r; }
unsigned long long fib(int n) { unsigned long long a = 0, b = 1, t; while (n--) { t = a + b; a = b; b = t; } return a; }
double g_d = 1.0 / 3.0;
long long g_ll = 1LL << 40;
float g_f = 2.5f;
double tab[3] = {0.1, 0.2, 0.3};
struct mixed { char c; double d; long long l; float f; };
int main(void) {
    long long a = 123456789012LL, b = -987654321;
    unsigned long long u = 18446744073709551615ULL;
    double x = 0.1 + 0.2, y;
    float f = 0.1f;
    int i;
    struct mixed m = {'z', 2.75, -5, 1.5f};
    printf("%lld %lld %lld\n", a, b, a * b / 1000);
    printf("%lld %lld %lld\n", a / 7, a % 7, -a >> 3);
    printf("%llu %llx %llu\n", u, u, u / 3);
    printf("%lld %llu\n", fact(20), fib(90));
    printf("%.17f %.17g\n", x, x);
    printf("%d\n", x == 0.3);
    printf("%.10f %.6e\n", area(2.0), 1e300 * 1e5);
    printf("%.15f %.15f\n", sqrt(2.0), sqrtf(2.0f));
    printf("%.12f %.12f %.12f\n", sin(1.0), exp(1.0), log(10.0));
    printf("%.9f %.6f\n", (double)f, f * 2);
    printf("%.16g %lld %.1f\n", g_d, g_ll, g_f);
    printf("%.2f %.2f %.2f\n", tab[0], tab[1], tab[2]);
    printf("sizes %d %d %d %d\n", (int)sizeof(double), (int)sizeof(long long), (int)sizeof(struct mixed), (int)sizeof(float));
    printf("%c %.2f %lld %.1f\n", m.c, m.d, m.l, m.f);
    y = 3;
    y += 1.5;
    y *= 2;
    y++;
    printf("%.2f %d %lld %u\n", y, (int)y, (long long)y, (unsigned)(y * 1000));
    a = 10;
    a += 5;
    a <<= 2;
    a |= 3;
    a--;
    printf("%lld %d %d\n", a, a > 50, a == 62);
    i = (int)(a + 0.5);
    printf("%d %.3f %lld\n", i, a / 3.0, (long long)(1e15 + 7));
    {
        long long big = 0x7FFFFFFFFFFFFFFFLL;
        printf("%lld %lld\n", big, big + 1);
        printf("%d %d %d\n", -1LL < 0, (unsigned long long)-1 > 0, (int)(big >> 62));
    }
    {
        double v[5] = {3.5, -1.25, 9.0, 0.0, 2.75};
        double s = 0, mx = v[0];
        for (i = 0; i < 5; i++) { s += v[i]; if (v[i] > mx) mx = v[i]; }
        printf("%.3f %.3f %s\n", s, mx, s > 10 ? "big" : "small");
        printf("%.4f %.4f\n", fabs(-2.5), pow(2.0, 0.5));
        printf("%.2f %.2f %.2f\n", floor(-2.5), ceil(-2.5), fmod(7.5, 2.0));
    }
    {
        char buf[64];
        double d;
        long long ll;
        sprintf(buf, "%.3f|%lld|%e", 3.14159, -42LL, 0.000123);
        printf("%s\n", buf);
        sscanf("6.0221e23 9000000000", "%lf %lld", &d, &ll);
        printf("%.4e %lld\n", d, ll);
        printf("%.3f %lld\n", atof("2.718"), atoll("-123456789012"));
        printf("%.2f\n", strtod("1e-2xyz", NULL) * 100);
    }
    printf("%d %d\n", (int)3.99, (int)-3.99);
    printf("%lld\n", (long long)-2.5e18);
    printf("%.1f\n", (float)(1LL << 50));
    printf("%d\n", 7 / 2 * 2.0 == 6.0);
    return 0;
}
