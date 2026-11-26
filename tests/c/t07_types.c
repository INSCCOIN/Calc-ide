#include <stdio.h>
int main(void) {
    char c = 200;
    unsigned char uc = 200;
    short s = 40000;
    unsigned short us = 40000;
    unsigned int u = 3000000000u;
    int neg = -1;
    long l = 123456;
    printf("%d %d %d %d\n", c, uc, s, us);
    printf("%u %d\n", u, u > 2000000000u);
    printf("%d\n", (unsigned)neg > 1u);
    printf("%u %u\n", u / 7, u % 7);
    printf("%d %d\n", (char)300, (unsigned char)300);
    printf("%ld\n", l * 2);
    printf("%x %X %o %c\n", 255, 255, 8, 65);
    printf("[%5d] [%-5d] [%05d] [%+d]\n", 42, 42, 42, 42);
    printf("[%10s] [%-10s] [%.2s]\n", "abc", "abc", "abc");
    printf("%d %d %d %d\n", (int)sizeof(char), (int)sizeof(short), (int)sizeof(int), (int)sizeof(char *));
    c = 'A';
    c++;
    c += 2;
    printf("%c %d\n", c, c - 'A');
    uc = 255;
    uc++;
    printf("%d\n", uc);
    printf("%u\n", u >> 28);
    printf("%d\n", neg >> 28);
    return 0;
}
