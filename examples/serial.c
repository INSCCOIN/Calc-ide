// serial.c - a tiny serial terminal on UART0 (GP0 = TX, GP1 = RX)
// usage: run serial.cbc [baud]     Esc quits
#include <stdio.h>
#include <stdlib.h>
#include <picocalc.h>

int main(int argc, char **argv)
{
    int baud = argc > 1 ? atoi(argv[1]) : 115200;
    int k, c;
    if (uart_open(0, baud) < 0) { printf("cannot open UART0\n"); return 1; }
    printf("UART0 at %d baud - Esc quits\n", baud);
    for (;;) {
        while ((c = uart_getc(0)) >= 0) putchar(c);
        k = inkey();
        if (k == KEY_ESC) break;
        if (k == KEY_ENTER) uart_puts(0, "\r\n");
        else if (k >= 0 && k < 128) uart_putc(0, k);
        refresh();
    }
    uart_close(0);
    return 0;
}
