// drums.c - melody, bass and a drum beat at once with play()
// Channels are separated by '|'; see 'help play' and compiler.md.
#include <stdio.h>

int main(void)
{
    printf("Three channels: lead, bass, drums\n");
    play("T128 IPLS V6 L8 [E5 G5 A5 G5 E5 D5 C5 D5]x2 E5/2 R/2"
         " | ITRI V9 L4 [A2 A2 F2 G2]x2 A2/2 R/2"
         " | V6 [K HH SN HH K K SN HH]x4 CY/2 R/2");
    printf("Now a song file: /calcide/songs/chiptune.song\n");
    if (play("@/calcide/songs/chiptune.song") < 0) printf("(run 'install' first)\n");
    return 0;
}
