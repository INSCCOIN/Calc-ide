#include <stdio.h>
int g = 10;
int arr[5] = {1, 2, 3, 4, 5};
char msg[] = "global string";
char *ptr = "pointer string";
static int counter;
enum { RED, GREEN = 5, BLUE };
typedef int myint;
int add(int a, int b) { return a + b; }
int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
int next_id(void) { static int id = 100; return id++; }
int main(void) {
    int i, sum = 0;
    myint m = 7;
    for (i = 0; i < 5; i++) sum += arr[i];
    printf("sum=%d g=%d m=%d\n", sum, g, m);
    printf("%s / %s / %d\n", msg, ptr, (int)sizeof(msg));
    printf("enum %d %d %d\n", RED, GREEN, BLUE);
    printf("fact(10)=%d fib(20)=%d\n", fact(10), fib(20));
    i = next_id();
    printf("ids %d", i);
    i = next_id();
    printf(" %d", i);
    printf(" %d\n", next_id());
    i = 0;
    while (1) { i++; if (i == 3) continue; if (i > 6) break; counter += i; }
    printf("counter=%d\n", counter);
    do { i--; } while (i > 2);
    printf("i=%d\n", i);
    printf("%d %d %d %d\n", 17 / 5, 17 % 5, -17 / 5, -17 % 5);
    printf("%d %d %d\n", 1 << 10, -16 >> 2, 0xFF ^ 0x0F);
    printf("%d %d %d %d\n", 3 && 0, 3 || 0, !5, ~0);
    printf("%d\n", add(add(1, 2), add(3, 4)));
    {
        int x = 5;
        x += 3; x -= 1; x *= 4; x /= 2; x %= 5; x <<= 3; x >>= 1; x |= 1; x &= 13; x ^= 6;
        printf("x=%d\n", x);
    }
    printf("ternary %s\n", g > 5 ? "big" : "small");
    printf("comma %d\n", (i = 3, i + 4));
    return 0;
}
