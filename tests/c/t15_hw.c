#include <stdio.h>
#include <string.h>
#include <picocalc.h>
void write_wav(char *path, int rate, int bits, int chans, int frames)
{
    FILE *f = fopen(path, "w");
    int data = frames * chans * bits / 8, v, i;
    int hdr[11];
    fwrite("RIFF", 1, 4, f);
    v = 36 + data; fwrite(&v, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    v = 1 | (chans << 16); fwrite(&v, 4, 1, f);
    fwrite(&rate, 4, 1, f);
    v = rate * chans * bits / 8; fwrite(&v, 4, 1, f);
    v = (chans * bits / 8) | (bits << 16); fwrite(&v, 4, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data, 4, 1, f);
    for (i = 0; i < data; i++) fputc((i * 37) & 255, f);
    fclose(f);
}
int main(void)
{
    char buf[16];
    int n;
    printf("gpio free pin: %d %d\n", gpio_mode(2, GPIO_OUT), gpio_write(2, 1));
    printf("gpio read: %d\n", gpio_read(2));
    printf("gpio pullup: %d %d\n", gpio_mode(3, GPIO_PULLUP), gpio_read(3));
    printf("gpio reserved: %d %d %d\n", gpio_mode(10, GPIO_OUT), gpio_write(17, 1), gpio_read(26));
    printf("uart open: %d %d\n", uart_open(0, 115200), uart_open(5, 9600));
    uart_puts(0, "hello");
    uart_putc(0, '!');
    printf("uart avail: %d\n", uart_available(0));
    n = uart_read(0, buf, 15);
    buf[n] = 0;
    printf("uart loopback: %d '%s' %d\n", n, buf, uart_getc(0));
    uart_close(0);
    printf("play: %d\n", play("T600 L16 C4 D#4 Eb4 R G4/32 C5. O5 A B"));
    printf("bad note: %d\n", play("C4 H4"));
    write_wav("t.wav", 8000, 16, 2, 400);
    printf("wav 16-bit stereo: %d\n", play_wav("t.wav"));
    write_wav("t8.wav", 4000, 8, 1, 200);
    printf("wav 8-bit mono: %d\n", play_wav("t8.wav"));
    printf("wav missing: %d\n", play_wav("nope.wav"));
    remove("t.wav");
    remove("t8.wav");
    return 0;
}
