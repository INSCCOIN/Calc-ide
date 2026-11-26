// tune.c - play a short melody on the PicoCalc speaker
#include <stdio.h>

int notes[] = {523, 587, 659, 698, 784, 880, 988, 1047};
int song[] = {0, 0, 4, 4, 5, 5, 4, -1, 3, 3, 2, 2, 1, 1, 0};

int main(void)
{
    int i, n = sizeof(song) / sizeof(song[0]);
    for (i = 0; i < n; i++) {
        if (song[i] < 0) delay(200);
        else beep(notes[song[i]], 250);
        delay(40);
    }
    printf("done\n");
    return 0;
}
