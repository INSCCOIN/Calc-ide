#include <stdio.h>
#include <stdlib.h>
struct point { int x, y; };
typedef struct node { int val; struct node *next; } Node;
union u { int i; char c[4]; float f; };
struct rect { struct point a, b; char name[8]; };
struct point mk(int x, int y) { struct point p; p.x = x; p.y = y; return p; }
int area(struct rect r) { return (r.b.x - r.a.x) * (r.b.y - r.a.y); }
void move(struct point *p, int dx) { p->x += dx; p->y += dx; }
struct point origin = {3, 4};
struct rect rects[2] = {{{0, 0}, {2, 3}, "first"}, {{1, 1}, {5, 5}, "second"}};
int main(void) {
    struct point p = {1, 2}, q;
    struct rect r;
    Node *head = 0, *n;
    union u x;
    int i, total = 0;
    q = p;
    q.x = 10;
    printf("%d %d %d %d\n", p.x, p.y, q.x, q.y);
    q = mk(7, 8);
    printf("%d %d\n", q.x, q.y);
    move(&q, 2);
    printf("%d %d\n", q.x, q.y);
    r.a = mk(0, 0);
    r.b = mk(4, 5);
    printf("area %d\n", area(r));
    printf("origin %d %d\n", origin.x, origin.y);
    printf("%s %d / %s %d\n", rects[0].name, area(rects[0]), rects[1].name, area(rects[1]));
    for (i = 1; i <= 5; i++) {
        n = (Node *)malloc(sizeof(Node));
        n->val = i * i;
        n->next = head;
        head = n;
    }
    for (n = head; n; n = n->next) total += n->val;
    printf("list total %d\n", total);
    while (head) { n = head->next; free(head); head = n; }
    x.i = 0x41424344;
    printf("union %c%c%c%c size %d\n", x.c[0], x.c[1], x.c[2], x.c[3], (int)sizeof(union u));
    x.f = 1.0f;
    printf("float bits %x\n", x.i);
    printf("sizes %d %d %d\n", (int)sizeof(struct point), (int)sizeof(struct rect), (int)sizeof(Node));
    {
        struct point *pa = (struct point *)calloc(3, sizeof(struct point));
        pa[2].y = 9;
        printf("%d %d\n", pa[0].x, pa[2].y);
        free(pa);
    }
    return 0;
}
