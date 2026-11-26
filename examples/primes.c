// primes.c - sieve of Eratosthenes with malloc
#include <stdio.h>
#include <stdlib.h>

#define LIMIT 10000

int main(void)
{
    char *sieve = calloc(LIMIT + 1, 1);
    int i, j, count = 0;
    if (!sieve) { printf("out of memory\n"); return 1; }
    for (i = 2; i * i <= LIMIT; i++)
        if (!sieve[i])
            for (j = i * i; j <= LIMIT; j += i) sieve[j] = 1;
    for (i = 2; i <= LIMIT; i++) {
        if (!sieve[i]) {
            count++;
            if (i > LIMIT - 200) printf("%d ", i);
        }
    }
    printf("\n%d primes below %d\n", count, LIMIT);
    free(sieve);
    return 0;
}
