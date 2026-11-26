/* ccport.c - the native side of cc/ccport.h. */
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "vfs.h"
#include "console.h"
#include "cc.h"

typedef struct cc_FILE { vfile *f; } cc_FILE;
int cc_main(int argc, char **argv);

typedef struct Blk { struct Blk *prev, *next; } Blk;
static Blk *blocks;
static jmp_buf cc_jb;
static int cc_code;
static cc_FILE *open_files[16];
static size_t cc_bytes, cc_peak;

int (*cc_out_hook)(const char *s, int n);

int cc_printf(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    if (cc_out_hook) cc_out_hook(buf, n);
    else con_write(buf, n);
    return n;
}

cc_FILE *cc_fopen(const char *path, const char *mode)
{
    char full[VFS_PATH];
    cc_FILE *f;
    int i;
    if (!vfs_resolve(path, full)) return 0;
    for (i = 0; i < 16; i++) if (!open_files[i]) break;
    if (i == 16) return 0;
    f = (cc_FILE *)malloc(sizeof *f);
    if (!f) return 0;
    f->f = vfs_open(full, mode);
    if (!f->f) { free(f); return 0; }
    open_files[i] = f;
    return f;
}

int cc_fclose(cc_FILE *f)
{
    int i;
    if (!f) return -1;
    for (i = 0; i < 16; i++) if (open_files[i] == f) open_files[i] = 0;
    vfs_close(f->f);
    free(f);
    return 0;
}

int cc_fread(void *p, int size, int n, cc_FILE *f)
{
    int r = vfs_read(f->f, p, size * n);
    return r <= 0 ? 0 : r / size;
}

int cc_fwrite(const void *p, int size, int n, cc_FILE *f)
{
    int r = vfs_write(f->f, p, size * n);
    return r <= 0 ? 0 : r / size;
}

void *cc_malloc(int n)
{
    Blk *b;
    if (n < 0) return 0;
    b = (Blk *)malloc(sizeof(Blk) + (size_t)n + sizeof(size_t));
    if (!b) return 0;
    b->prev = 0;
    b->next = blocks;
    if (blocks) blocks->prev = b;
    blocks = b;
    cc_bytes += (size_t)n;
    if (cc_bytes > cc_peak) cc_peak = cc_bytes;
    *(size_t *)(b + 1) = (size_t)n;
    return (char *)(b + 1) + sizeof(size_t);
}

void cc_free(void *p)
{
    Blk *b;
    if (!p) return;
    b = (Blk *)((char *)p - sizeof(size_t)) - 1;
    cc_bytes -= *(size_t *)(b + 1);
    if (b->prev) b->prev->next = b->next;
    else blocks = b->next;
    if (b->next) b->next->prev = b->prev;
    free(b);
}

void cc_exit(int code)
{
    cc_code = code;
    longjmp(cc_jb, 1);
}

double cc_atof(const char *s)
{
    return strtod(s, 0);
}

float cc_strtof(const char *s, char **end)
{
    return strtof(s, end);
}

int cc_sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsprintf(buf, fmt, ap);
    va_end(ap);
    return n;
}

static void cc_cleanup(void)
{
    int i;
    for (i = 0; i < 16; i++)
        if (open_files[i]) { vfs_close(open_files[i]->f); free(open_files[i]); open_files[i] = 0; }
    while (blocks) {
        Blk *n = blocks->next;
        free(blocks);
        blocks = n;
    }
    cc_bytes = 0;
}

int cc_compile(int argc, char **argv)
{
    int rc;
    cc_peak = 0;
    if (setjmp(cc_jb) == 0) rc = cc_main(argc, argv);
    else rc = cc_code;
    cc_cleanup();
    return rc;
}

size_t cc_peak_bytes(void) { return cc_peak; }
