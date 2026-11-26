// keys.c - show key codes until Esc is pressed
#include <stdio.h>
#include <picocalc.h>

int main(void)
{
    int k;
    printf("Press keys (Esc quits)\n");
    while ((k = getkey()) != KEY_ESC) {
        if (k >= 32 && k < 127) printf("'%c' = %d\n", k, k);
        else printf("code 0x%02X\n", k);
    }
    return 0;
}
