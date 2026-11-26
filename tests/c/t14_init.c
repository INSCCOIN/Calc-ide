#include <stdio.h>
#include <string.h>
#define STR(x) #x
#define SHOW(e) printf("%s = %d\n", #e, (e))
#define NAME(n) "item_" #n
struct pt { int x, y; };
struct rect { struct pt a, b; const char *name; };
int days[12] = {[0] = 31, [1] = 28, [11] = 31};
struct pt origin = {.y = 7, .x = 3};
const char *names[] = {[2] = "two", [0] = "zero", "one"};
int sparse[] = {1, 2, [9] = 10};
int sum_pts(struct pt *p, int n) { int i, s = 0; for (i = 0; i < n; i++) s += p[i].x + p[i].y; return s; }
int len(int *a) { return a[0]; }
int main(void) {
    struct rect r = {.b = {.x = 5, .y = 6}, .name = "box", .a = {0}};
    struct pt *p;
    int i;
    printf("%d %d %d %d\n", days[0], days[1], days[2], days[11]);
    printf("%d %d\n", origin.x, origin.y);
    printf("%s %s %s %d\n", names[0], names[1], names[2], (int)(sizeof(names) / sizeof(names[0])));
    printf("%d %d %d\n", (int)(sizeof(sparse) / sizeof(int)), sparse[1], sparse[9]);
    printf("%d %d %s\n", r.b.x, r.b.y, r.name);
    printf("%s %s %s\n", STR(hello world), STR(1 + 2), NAME(42));
    SHOW(6 * 7);
    SHOW(sizeof(struct pt));
    p = &(struct pt){10, 20};
    printf("%d %d\n", p->x, p->y);
    printf("%d\n", sum_pts((struct pt[]){{1, 2}, {3, 4}, {5, 6}}, 3));
    printf("%d\n", len((int[]){99, 1}));
    printf("%d\n", ((struct pt){.y = 4}).y);
    {
        int a[5] = {[4] = 5, [1] = 2};
        for (i = 0; i < 5; i++) printf("%d", a[i]);
        printf("\n");
    }
    printf("%s\n", STR("quoted"));
    return 0;
}
