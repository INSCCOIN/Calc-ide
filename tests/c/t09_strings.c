#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
int main(void) {
    char buf[64];
    char *dup, *tok;
    int a, b, n;
    float f;
    char word[16];
    sprintf(buf, "%d-%s-%c", 42, "x", 'y');
    printf("%s\n", buf);
    n = sscanf("12 34 5.5 hello", "%d %d %f %s", &a, &b, &f, word);
    printf("%d: %d %d %.1f %s\n", n, a, b, f, word);
    dup = strdup("one,two,,three");
    for (tok = strtok(dup, ","); tok; tok = strtok(0, ",")) printf("[%s]", tok);
    printf("\n");
    free(dup);
    printf("%d %d %d\n", atoi("123"), atoi("-45"), (int)strtol("ff", 0, 16));
    printf("%c%c %d %d\n", toupper('a'), tolower('Q'), isdigit('7') != 0, isalpha('7') != 0);
    memset(buf, 'z', 5);
    buf[5] = 0;
    printf("%s %d\n", buf, memcmp("abc", "abc", 3));
    strncpy(buf, "abcdefgh", 3);
    buf[3] = 0;
    printf("%s %s\n", buf, strstr("haystack", "st"));
    snprintf(buf, 6, "%s", "truncated");
    printf("%s\n", buf);
    {
        char line[] = "local array";
        char two[2][6] = {"ab", "cde"};
        printf("%s %d %s %s\n", line, (int)sizeof(line), two[0], two[1]);
    }
    printf("%s\n", "concat" "enated" " strings");
    printf("esc: \t|\\|\"|\x41|\101|%%\n");
    return 0;
}
