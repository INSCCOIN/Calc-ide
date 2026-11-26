/* ccport.h - lets cc.c be compiled by gcc as part of the firmware.
 *
 * cc.c is written in the Tiny-C dialect and uses a handful of library
 * calls. Under gcc they are renamed onto src/ccport.c, which routes output
 * to the console, files to the SD card and allocation to a tracked pool
 * that is released when the compiler returns, even after an error.
 */
#ifndef CCPORT_H
#define CCPORT_H

#include <string.h>
#include <limits.h>
#include <setjmp.h>

typedef struct cc_FILE cc_FILE;
#define FILE    cc_FILE
#define printf  cc_printf
#define fopen   cc_fopen
#define fclose  cc_fclose
#define fread   cc_fread
#define fwrite  cc_fwrite
#define malloc  cc_malloc
#define free    cc_free
#define exit    cc_exit
#define atof    cc_atof
#define strtof  cc_strtof
#define sprintf cc_sprintf
#define main    cc_main

int      cc_printf(const char *fmt, ...);
cc_FILE *cc_fopen(const char *path, const char *mode);
int      cc_fclose(cc_FILE *f);
int      cc_fread(void *p, int size, int n, cc_FILE *f);
int      cc_fwrite(const void *p, int size, int n, cc_FILE *f);
void    *cc_malloc(int n);
void     cc_free(void *p);
void     cc_exit(int code) __attribute__((noreturn));
double   cc_atof(const char *s);
float    cc_strtof(const char *s, char **end);
int      cc_sprintf(char *buf, const char *fmt, ...);
int      cc_main(int argc, char **argv);

#endif
