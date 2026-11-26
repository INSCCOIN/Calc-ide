// guess.c - reading numbers from the keyboard
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    int secret, guess, tries = 0;
    srand(millis());
    secret = rand() % 100 + 1;
    printf("I'm thinking of a number from 1 to 100.\n");
    while (1) {
        printf("Your guess: ");
        if (scanf("%d", &guess) != 1) break;
        tries++;
        if (guess < secret) printf("Higher!\n");
        else if (guess > secret) printf("Lower!\n");
        else {
            printf("Got it in %d tries!\n", tries);
            beep(1200, 150);
            break;
        }
    }
    return 0;
}
