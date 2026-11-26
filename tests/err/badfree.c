// error: free() of an invalid or already freed pointer
#include <stdlib.h>
int main(void) { char *p = malloc(10); free(p); free(p); return 0; }
