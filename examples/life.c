// life.c - Conway's Game of Life in text mode (any key stops)
#include <stdio.h>
#include <stdlib.h>
#include <picocalc.h>

#define W 40
#define H 28

char grid[H][W], next_grid[H][W];

int neighbours(int y, int x)
{
    int dy, dx, n = 0;
    for (dy = -1; dy <= 1; dy++)
        for (dx = -1; dx <= 1; dx++)
            if (dy || dx) n += grid[(y + dy + H) % H][(x + dx + W) % W];
    return n;
}

int main(void)
{
    int x, y, gen = 0;
    srand(millis());
    for (y = 0; y < H; y++)
        for (x = 0; x < W; x++) grid[y][x] = rand() % 4 == 0;
    cursor(0);
    while (!kbhit()) {
        gotoxy(0, 0);
        textcolor(BRIGHT_GREEN);
        for (y = 0; y < H; y++) {
            for (x = 0; x < W && x < screen_cols(); x++) putchar(grid[y][x] ? 'O' : ' ');
            if (screen_cols() > W) putchar('\n');
        }
        textcolor(WHITE);
        printf("generation %d", gen++);
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) {
                int n = neighbours(y, x);
                next_grid[y][x] = n == 3 || (n == 2 && grid[y][x]);
            }
        for (y = 0; y < H; y++)
            for (x = 0; x < W; x++) grid[y][x] = next_grid[y][x];
        refresh();
    }
    getkey();
    cursor(1);
    printf("\n");
    return 0;
}
