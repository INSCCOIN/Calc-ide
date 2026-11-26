// tune.c - melodies from tracker strings (play) and a scale by hand (beep)
#include <stdio.h>

int main(void)
{
    printf("Twinkle, twinkle...\n");
    play("T140 L4 C4 C4 G4 G4 A4 A4 G4/2 F4 F4 E4 E4 D4 D4 C4/2");
    printf("...and a scale with beep()\n");
    {
        int notes[] = {523, 587, 659, 698, 784, 880, 988, 1047};
        int i;
        for (i = 0; i < 8; i++) { beep(notes[i], 120); delay(20); }
    }
    return 0;
}
