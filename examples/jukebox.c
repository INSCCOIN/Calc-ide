// jukebox.c - everything play() can do from C (the demo in SONGS.md)
//   1. play a fixed tune         2. build a tune at run time
//   3. a drum pad on the keys    4. a jukebox of .song files
#include <stdio.h>
#include <string.h>
#include <picocalc.h>

#define NSONGS 8
char *titles[NSONGS] = {"sunrise", "lavalamp", "tangerine", "velvetfuzz",
                        "caravan", "mandala", "chiptune", "ode"};

char lead[400], bass[200], drums[300], groove[1000];

// Four bars over C - Am - F - G. Each channel must add up to the same
// length: here every bar is 4 beats on every channel.
void build_groove(int tempo)
{
    char *arp[4]  = {"C4 E4 G4 E4", "A3 C4 E4 C4", "F3 A3 C4 A3", "G3 B3 D4 B3"};
    char *root[4] = {"C3", "A2", "F2", "G2"};
    int i;

    sprintf(lead, "T%d IPLS V5 L8 ", tempo);
    strcpy(bass, "ITRI V9 ");
    strcpy(drums, "V6 ");
    for (i = 0; i < 4; i++) {
        sprintf(lead + strlen(lead), "[%s]x2 ", arp[i]);                 // 8 x 1/8
        sprintf(bass + strlen(bass), "%s/4. %s/8 %s/2 ", root[i], root[i], root[i]);
        strcat(drums, i < 3 ? "K/4 HH/8 HH/8 SN/4 HH/8 OH/8 "
                            : "SN/16 SN/16 SN/16 SN/16 TH/8 TH/8 TL/8 TL/8 CY/4 ");
    }
    sprintf(groove, "%s | %s | %s", lead, bass, drums);
}

void drum_pad(void)
{
    int k;
    printf("\nDrum pad: k kick  s snare  h hat  o open hat\n");
    printf("          c clap  t/g toms  x crash   q done\n");
    for (;;) {
        k = getkey();
        switch (k) {
        case 'k': play("K/16"); break;
        case 's': play("SN/16"); break;
        case 'h': play("HH/16"); break;
        case 'o': play("OH/8"); break;
        case 'c': play("CP/16"); break;
        case 't': play("TH/16"); break;
        case 'g': play("TL/16"); break;
        case 'x': play("CY/4"); break;
        case 'q': case KEY_ESC: return;
        }
    }
}

void jukebox(void)
{
    char path[64];
    int i, k;
    for (;;) {
        printf("\nJukebox\n");
        for (i = 0; i < NSONGS; i++) printf("  %d  %s\n", i + 1, titles[i]);
        printf("  q  quit\n> ");
        k = getkey();
        if (k == 'q' || k == KEY_ESC) { printf("bye\n"); return; }
        if (k < '1' || k >= '1' + NSONGS) { printf("?\n"); continue; }
        sprintf(path, "@/calcide/songs/%s.song", titles[k - '1']);
        printf("playing %s\n", path + 1);
        if (play(path) < 0) printf("can't play it - run 'install' first\n");
    }
}

int main(void)
{
    printf("1. A fixed tune, three channels\n");
    play("T140 ISQ L8 C5 E5 G5 C6/4 G5 C6/2 | ITRI V9 C3/2 G2/2 | K/4 SN/4 K/4 CY/4");

    printf("2. A groove built by the program\n");
    build_groove(100);
    printf("   %d characters of notes, slow...\n", strlen(groove));
    play(groove);
    build_groove(150);
    printf("   ...and fast\n");
    play(groove);

    drum_pad();
    jukebox();
    return 0;
}
