#include <stdio.h>
#include <math.h>
float avg(float *v, int n) { float s = 0; int i; for (i = 0; i < n; i++) s += v[i]; return s / n; }
float half = 0.5f;
float table[3] = {1.5f, 2.25f, -3.0f};
int main(void) {
    float a = 3.0f, b = 4.0f;
    float v[4] = {1, 2, 3, 4};
    double d = 2.5;
    int i = 7;
    printf("%.3f\n", sqrtf(a * a + b * b));
    printf("%.2f %.2f\n", avg(v, 4), half);
    printf("%.2f %.2f %.2f\n", table[0], table[1], table[2]);
    printf("%d %d\n", (int)(a / b * 100), (int)-2.7f);
    printf("%.1f\n", i / 2.0f);
    printf("%.1f\n", (float)(i / 2));
    printf("%d %d %d\n", a < b, a == 3, b >= 4.5f);
    printf("%.4f %.4f\n", sinf(0.5f), cosf(0.5f));
    printf("%.2f\n", d * 2);
    a += 1;
    a++;
    printf("%.1f %e\n", a, 12345.678f);
    printf("%g %g\n", 0.0001f, 100000.0f);
    printf("%.3f\n", powf(2.0f, 0.5f));
    printf("%.0f %.0f\n", floorf(2.7f), ceilf(2.1f));
    {
        float x = -1.25f;
        printf("%.2f %.2f\n", -x, fabsf(x));
        if (x) printf("nonzero\n");
        if (!x) printf("zero\n");
    }
    printf("%8.3f|%-8.2f|\n", 3.14159f, 2.5f);
    return 0;
}
