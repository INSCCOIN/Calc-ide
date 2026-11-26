// blink.c - blink an LED on a free GPIO pin (default GP2), any key stops
// Wire an LED with a ~330 ohm resistor from the pin to GND.
#include <stdio.h>
#include <stdlib.h>
#include <picocalc.h>

int main(int argc, char **argv)
{
    int pin = argc > 1 ? atoi(argv[1]) : 2;
    int on = 0;
    if (gpio_mode(pin, GPIO_OUT) < 0) {
        printf("GP%d is not free (try 0-5, 8, 9, 20-22, 28)\n", pin);
        return 1;
    }
    printf("Blinking GP%d - press a key to stop\n", pin);
    while (!kbhit()) {
        on = !on;
        gpio_write(pin, on);
        delay(250);
    }
    getkey();
    gpio_write(pin, 0);
    return 0;
}
