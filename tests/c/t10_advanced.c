#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct { char name[12]; int score; } Player;
typedef enum { NORTH, EAST, SOUTH, WEST } Dir;
struct matrix { int rows, cols; float *data; };
int cmp_player(void *a, void *b) { return ((Player *)b)->score - ((Player *)a)->score; }
struct matrix *mat_new(int r, int c) {
    struct matrix *m = malloc(sizeof(struct matrix));
    m->rows = r; m->cols = c;
    m->data = calloc(r * c, sizeof(float));
    return m;
}
float *at(struct matrix *m, int r, int c) { return &m->data[r * m->cols + c]; }
char **split(char *s, int *count) {
    char **out = malloc(8 * sizeof(char *));
    int n = 0;
    char *t = strtok(s, " ");
    while (t && n < 8) { out[n++] = t; t = strtok(NULL, " "); }
    *count = n;
    return out;
}
int counter(void) { static int c; return ++c; }
int sum_va(int n, int *v) { return n ? v[0] + sum_va(n - 1, v + 1) : 0; }
const char *dirs[] = {"north", "east", "south", "west"};
int primes[] = {2, 3, 5, 7, 11, 13};
int main(void) {
    Player ps[4] = {{"ann", 30}, {"bob", 70}, {"cy", 50}, {"di", 10}};
    struct matrix *m = mat_new(3, 3);
    char text[] = "the quick brown fox";
    char **words;
    int i, n;
    Dir d = SOUTH;
    unsigned int hash = 5381;
    qsort(ps, 4, sizeof(Player), cmp_player);
    for (i = 0; i < 4; i++) printf("%s:%d ", ps[i].name, ps[i].score);
    printf("\n");
    for (i = 0; i < 3; i++) *at(m, i, i) = i + 1.5f;
    printf("%.1f %.1f %.1f\n", *at(m, 0, 0), *at(m, 1, 1), *at(m, 2, 1));
    words = split(text, &n);
    for (i = n - 1; i >= 0; i--) printf("%s%c", words[i], i ? ' ' : '\n');
    free(words);
    printf("%s %d\n", dirs[d], (int)(sizeof(primes) / sizeof(primes[0])));
    counter(); counter();
    printf("counter %d\n", counter());
    printf("sum %d\n", sum_va(6, primes));
    for (i = 0; text[i]; i++) hash = hash * 33 + text[i];
    printf("hash %u\n", hash);
    {
        int a[3][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
        int (*row)[3] = a;
        int t = 0, j;
        for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) t += a[i][j] * (i == j);
        printf("trace %d %d\n", t, row[2][0]);
    }
    {
        char buf[40];
        int len = sprintf(buf, "%5.2f|%-4d|%04x", 3.14159f, 7, 255);
        printf("%s (%d)\n", buf, len);
    }
    {
        int x = 10;
        int *px = &x;
        int **ppx = &px;
        int ***pppx = &ppx;
        ***pppx = 20;
        printf("x=%d\n", x);
    }
    {
        long big = 1;
        for (i = 0; i < 20; i++) big *= 2;
        printf("%ld %d\n", big, big > 1000000);
    }
    {
        unsigned char bytes[4] = {0x12, 0x34, 0x56, 0x78};
        unsigned int word = 0;
        for (i = 0; i < 4; i++) word = (word << 8) | bytes[i];
        printf("%08x\n", word);
    }
    free(m->data);
    free(m);
    return 0;
}
