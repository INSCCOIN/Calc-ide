/* cc.h - entry point of the native build of the compiler. */
#ifndef CC_H
#define CC_H
#include <stddef.h>
int    cc_compile(int argc, char **argv);   /* argv[0] = "cc" */
size_t cc_peak_bytes(void);
extern int (*cc_out_hook)(const char *s, int n);
#endif
