// graphics.c - pixels and rectangles straight to the 320x320 panel
#include <stdio.h>
#include <picocalc.h>

int main(void)
{
    int i;
    cls();
    for (i = 0; i < 16; i++)
        fillrect(i * 20, 0, 20, 40, rgb(i * 16, 255 - i * 16, 128));
    for (i = 0; i < 160; i++) {
        pixel(160 + i, 160 + i / 2, rgb(255, 255, 0));
        pixel(160 - i, 160 + i / 2, rgb(0, 255, 255));
    }
    line(0, 319, 319, 40, rgb(255, 0, 0));
    gotoxy(0, 30);
    printf("press a key");
    getkey();
    cls();
    return 0;
}
