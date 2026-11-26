// structs.c - structs, typedefs, pointers to pointers, function pointers
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Item {
    char name[16];
    int price;
    struct Item *next;
} Item;

void push(Item **list, char *name, int price)
{
    Item *it = malloc(sizeof(Item));
    strcpy(it->name, name);
    it->price = price;
    it->next = *list;
    *list = it;
}

int by_price(void *a, void *b) { return (*(Item **)a)->price - (*(Item **)b)->price; }

int main(void)
{
    Item *list = NULL, *it, *arr[8];
    int n = 0, i, total = 0;
    push(&list, "apple", 30);
    push(&list, "bread", 120);
    push(&list, "milk", 90);
    push(&list, "tea", 250);
    for (it = list; it; it = it->next) arr[n++] = it;
    qsort(arr, n, sizeof(Item *), by_price);
    for (i = 0; i < n; i++) {
        printf("%-8s %3d.%02d\n", arr[i]->name, arr[i]->price / 100, arr[i]->price % 100);
        total += arr[i]->price;
    }
    printf("total    %3d.%02d\n", total / 100, total % 100);
    while (list) { it = list->next; free(list); list = it; }
    return 0;
}
