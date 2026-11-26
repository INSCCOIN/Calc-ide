// mandel.c - a coloured text Mandelbrot set (floating point)
#include <stdio.h>
#include <picocalc.h>

int main(void)
{
    int cols = screen_cols(), rows = screen_rows() - 2;
    int x, y, i;
    char *shades = " .:-=+*#%@";
    for (y = 0; y < rows; y++) {
        for (x = 0; x < cols; x++) {
            float cr = -2.2f + 3.0f * x / cols;
            float ci = -1.2f + 2.4f * y / rows;
            float zr = 0, zi = 0, t;
            for (i = 0; i < 32 && zr * zr + zi * zi < 4.0f; i++) {
                t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = t;
            }
            textcolor(i >= 32 ? BLACK : 1 + i % 15);
            putchar(i >= 32 ? ' ' : shades[i % 10]);
        }
    }
    textcolor(WHITE);
    printf("Mandelbrot - press a key");
    getkey();
    setcolor(WHITE, BLACK);
    printf("\n");
    return 0;
}
