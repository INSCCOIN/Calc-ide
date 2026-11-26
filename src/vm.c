/* vm.c - runs CalcIDE bytecode (.cbc) produced by cc/cc.c.
 *
 * Memory model. A program sees one flat byte array:
 *
 *   0 .. 63          trap zone: any access is a null-pointer error
 *   64 ..            string literals, initialised data, bss
 *   heap             malloc/free, first fit with coalescing
 *   stack            frames, growing down from the top
 *
 * Pointers are offsets into that array and every load and store is bounds
 * checked, so a wild pointer is reported as a runtime error with the line it
 * happened on instead of corrupting the firmware. Expression temporaries live
 * on a separate operand stack of 32-bit cells; floats are carried as their
 * IEEE single-precision bit patterns.
 *
 * Runtime errors: null pointer, out-of-bounds access, stack overflow,
 * division by zero, bad free, invalid instruction, user break (Ctrl+C).
 */
#include "vm.h"
#include "../cc/ops.h"
#include "hal.h"
#include "console.h"
#include "vfs.h"
#include "config.h"
#include "pf.h"
#include "lineedit.h"
#include "sound.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <setjmp.h>
#include <ctype.h>

#define OSTK      1024          /* operand stack cells */
#define NFILES    8
#define FH_BASE   16            /* FILE* values for opened files */
#define LR_NATIVE 0xFFFFFFF0u   /* return address meaning "back to C" */

VmStats vm_last;

typedef struct {
    vfile *f;
    int    eof;
    int    err;
} VFile;

typedef struct VM {
    const uint8_t *code;
    uint32_t  code_size;
    uint8_t  *mem;
    uint32_t  mem_size;
    uint32_t  data_end;
    uint32_t  heap_start;
    uint32_t  stack_limit;
    uint32_t  free_list;        /* address of the first free block, 0 if none */
    uint32_t  heap_used;
    int32_t   ost[OSTK + 8];
    int       osp;
    uint32_t  pc, fp, sp, rec, lr;
    uint32_t  cur_op_pc;
    uint32_t  icount;
    /* debug info */
    const uint8_t *lines;
    uint32_t  nlines;
    const uint8_t *funcs;
    uint32_t  funcs_size;
    const char *fnames;
    uint32_t  nfiles;
    const char *name;
    /* runtime */
    VFile     files[NFILES];
    jmp_buf   jb;
    int       exit_code;
    char      errmsg[96];
    uint32_t  rand_state;
    uint32_t  strtok_pos;
    char      inbuf[160];       /* stdin line buffer */
    int       inpos, inlen;
    uint32_t  t0, last_poll;
    int       native_depth;
    int       uarts_open;       /* bit per port, closed when the program ends */
} VM;

static VM *g_vm;

/* ------------------------------------------------------------------ */
/* errors                                                              */
/* ------------------------------------------------------------------ */

static void vm_fail(VM *vm, const char *msg)
{
    snprintf(vm->errmsg, sizeof vm->errmsg, "%s", msg);
    vm->exit_code = -1;
    longjmp(vm->jb, 2);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void where(VM *vm, uint32_t pc, int *line, const char **file, const char **func, int *flen)
{
    uint32_t i;
    uint32_t best = 0, bestl = 0;
    *line = 0;
    *file = vm->name;
    *func = 0;
    *flen = 0;
    for (i = 0; i < vm->nlines; i++) {
        uint32_t p = rd32(vm->lines + i * 8);
        if (p <= pc && p >= best) { best = p; bestl = rd32(vm->lines + i * 8 + 4); }
    }
    if (bestl) {
        uint32_t fi = bestl >> 24, k;
        const char *f = vm->fnames;
        *line = (int)(bestl & 0xFFFFFF);
        for (k = 0; k < fi && k < vm->nfiles && f; k++) f += strlen(f) + 1;
        if (f && fi < vm->nfiles) *file = f;
    }
    {
        uint32_t off = 0, bestf = 0;
        while (off + 5 <= vm->funcs_size) {
            uint32_t a = rd32(vm->funcs + off);
            int n = vm->funcs[off + 4];
            if (a <= pc && a >= bestf) { bestf = a; *func = (const char *)vm->funcs + off + 5; *flen = n; }
            off += 5 + n;
        }
    }
}

static void report_error(VM *vm)
{
    int line, flen;
    const char *file, *func;
    where(vm, vm->cur_op_pc, &line, &file, &func, &flen);
    con_default_color();
    con_set_fg(9);
    con_printf("\nRuntime error: %s\n", vm->errmsg);
    con_default_color();
    if (line) con_printf("  at %s:%d", file, line);
    if (func) con_printf(" in %.*s()", flen, func);
    con_printf("\n");
}

/* ------------------------------------------------------------------ */
/* memory access                                                       */
/* ------------------------------------------------------------------ */

static inline void chk(VM *vm, uint32_t a, uint32_t n)
{
    if (a < CBC_DATA_BASE) vm_fail(vm, a == 0 ? "null pointer dereference" : "invalid pointer (near null)");
    if (a > vm->mem_size - n || n > vm->mem_size) vm_fail(vm, "memory access out of bounds");
}

static void *vptr(VM *vm, uint32_t a, uint32_t n)
{
    if (n == 0 && a == 0) return 0;
    chk(vm, a, n ? n : 1);
    return vm->mem + a;
}

/* a NUL-terminated string inside VM memory */
static char *vstr(VM *vm, uint32_t a)
{
    uint8_t *p;
    chk(vm, a, 1);
    p = memchr(vm->mem + a, 0, vm->mem_size - a);
    if (!p) vm_fail(vm, "unterminated string");
    return (char *)vm->mem + a;
}

static uint32_t vaddr(VM *vm, const void *p)
{
    return (uint32_t)((const uint8_t *)p - vm->mem);
}

static inline float i2f(int32_t b) { float f; memcpy(&f, &b, 4); return f; }
static inline int32_t f2i(float f) { int32_t b; memcpy(&b, &f, 4); return b; }

/* ------------------------------------------------------------------ */
/* heap                                                                */
/* ------------------------------------------------------------------ */

/* Block: [size | used][next if free]. size includes the 8-byte header. */
#define HDR 8

static uint32_t W(VM *vm, uint32_t a) { return rd32(vm->mem + a); }
static void SW(VM *vm, uint32_t a, uint32_t v) { memcpy(vm->mem + a, &v, 4); }

static void heap_init(VM *vm)
{
    uint32_t start = (vm->heap_start + 7) & ~7u;
    uint32_t end = vm->stack_limit & ~7u;
    vm->heap_start = start;
    vm->heap_used = 0;
    if (end > start + 16) {
        SW(vm, start, end - start);
        SW(vm, start + 4, 0);
        vm->free_list = start;
    } else vm->free_list = 0;
}

static uint32_t vm_malloc(VM *vm, uint32_t n)
{
    uint32_t need, prev = 0, b;
    if (n == 0) n = 1;
    if (n > vm->mem_size) return 0;
    need = (n + HDR + 7) & ~7u;
    for (b = vm->free_list; b; prev = b, b = W(vm, b + 4)) {
        uint32_t sz = W(vm, b);
        if (sz >= need) {
            uint32_t nx = W(vm, b + 4);
            if (sz - need >= 16) {
                uint32_t r = b + need;
                SW(vm, r, sz - need);
                SW(vm, r + 4, nx);
                nx = r;
                sz = need;
            }
            if (prev) SW(vm, prev + 4, nx);
            else vm->free_list = nx;
            SW(vm, b, sz | 1);
            SW(vm, b + 4, 0x4D414C43);   /* "CLAM": marks a live block */
            vm->heap_used += sz;
            if (b + sz - vm->heap_start > vm_last.heap_peak) vm_last.heap_peak = b + sz - vm->heap_start;
            return b + HDR;
        }
    }
    return 0;
}

static void vm_free(VM *vm, uint32_t p)
{
    uint32_t b, sz, prev = 0, cur;
    if (p == 0) return;
    b = p - HDR;
    if (p < vm->heap_start + HDR || p >= vm->stack_limit || (p & 7)) vm_fail(vm, "free() of a pointer that was not allocated");
    sz = W(vm, b);
    if (!(sz & 1) || W(vm, b + 4) != 0x4D414C43) vm_fail(vm, "free() of an invalid or already freed pointer");
    sz &= ~1u;
    vm->heap_used -= sz;
    /* insert in address order, merging with neighbours */
    for (cur = vm->free_list; cur && cur < b; prev = cur, cur = W(vm, cur + 4)) { }
    SW(vm, b, sz);
    SW(vm, b + 4, cur);
    if (cur && b + sz == cur) {
        SW(vm, b, sz + W(vm, cur));
        SW(vm, b + 4, W(vm, cur + 4));
    }
    if (prev) {
        SW(vm, prev + 4, b);
        if (prev + W(vm, prev) == b) {
            SW(vm, prev, W(vm, prev) + W(vm, b));
            SW(vm, prev + 4, W(vm, b + 4));
        }
    } else vm->free_list = b;
}

static uint32_t heap_free_bytes(VM *vm)
{
    uint32_t b, t = 0;
    for (b = vm->free_list; b; b = W(vm, b + 4)) t += W(vm, b) - HDR;
    return t;
}

static uint32_t vm_realloc(VM *vm, uint32_t p, uint32_t n)
{
    uint32_t np, old;
    if (!p) return vm_malloc(vm, n);
    old = (W(vm, p - HDR) & ~1u) - HDR;
    if (p < vm->heap_start || W(vm, p - HDR + 4) != 0x4D414C43) vm_fail(vm, "realloc() of an invalid pointer");
    if (n <= old) return p;
    np = vm_malloc(vm, n);
    if (!np) return 0;
    memcpy(vm->mem + np, vm->mem + p, old);
    vm_free(vm, p);
    return np;
}

/* ------------------------------------------------------------------ */
/* formatted output                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char *buf;          /* NULL: stream */
    int   cap;
    int   len;
    VFile *file;        /* stream to a file, or console if NULL */
} Out;

static void out_put(Out *o, const char *s, int n)
{
    if (o->buf) {
        int i;
        for (i = 0; i < n; i++) {
            if (o->len + 1 < o->cap) o->buf[o->len] = s[i];
            o->len++;
        }
        if (o->cap > 0) o->buf[o->len < o->cap ? o->len : o->cap - 1] = 0;
    } else if (o->file) {
        if (o->file->f) vfs_write(o->file->f, s, n);
        o->len += n;
    } else {
        con_write(s, n);
        o->len += n;
    }
}

static inline double u2d(uint64_t u) { double d; memcpy(&d, &u, 8); return d; }
static inline uint64_t d2u(double d) { uint64_t u; memcpy(&u, &d, 8); return u; }

/* Arguments arrive as 32-bit cells; double and long long take two (low
 * word first), exactly as C's default argument promotions lay them out. */
static int32_t arg1(const int32_t *args, int nargs, int *ai)
{
    int32_t v = *ai < nargs ? args[*ai] : 0;
    (*ai)++;
    return v;
}

static uint64_t arg2(const int32_t *args, int nargs, int *ai)
{
    uint64_t lo = (uint32_t)arg1(args, nargs, ai);
    uint64_t hi = (uint32_t)arg1(args, nargs, ai);
    return lo | (hi << 32);
}

static int vm_format(VM *vm, Out *o, const char *fmt, const int32_t *args, int nargs)
{
    int ai = 0;
    char spec[40], tmp[400];
    while (*fmt) {
        const char *pct;
        int n, si, nl = 0;
        char conv;
        int width_star = 0, prec_star = 0, wv = 0, pv = 0;
        if (*fmt != '%') {
            const char *e = strchr(fmt, '%');
            n = e ? (int)(e - fmt) : (int)strlen(fmt);
            out_put(o, fmt, n);
            fmt += n;
            continue;
        }
        pct = fmt++;
        if (*fmt == '%') { out_put(o, "%", 1); fmt++; continue; }
        si = 0;
        spec[si++] = '%';
        while (*fmt && strchr("-+ #0", *fmt) && si < 10) spec[si++] = *fmt++;
        if (*fmt == '*') { width_star = 1; fmt++; spec[si++] = '*'; }
        else while (isdigit((unsigned char)*fmt) && si < 16) spec[si++] = *fmt++;
        if (*fmt == '.') {
            spec[si++] = *fmt++;
            if (*fmt == '*') { prec_star = 1; fmt++; spec[si++] = '*'; }
            else while (isdigit((unsigned char)*fmt) && si < 24) spec[si++] = *fmt++;
        }
        while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'L' || *fmt == 'q') {
            if (*fmt == 'l' || *fmt == 'q') nl++;
            if (*fmt == 'j' || *fmt == 'q') nl = 2;
            fmt++;
        }
        conv = *fmt;
        spec[si] = 0;
        if (!conv) { out_put(o, pct, (int)strlen(pct)); break; }
        fmt++;
        if (width_star) wv = arg1(args, nargs, &ai);
        if (prec_star) pv = arg1(args, nargs, &ai);
#define FMT1(v) (width_star && prec_star ? snprintf(tmp, sizeof tmp, spec, wv, pv, v) \
               : width_star ? snprintf(tmp, sizeof tmp, spec, wv, v) \
               : prec_star ? snprintf(tmp, sizeof tmp, spec, pv, v) : snprintf(tmp, sizeof tmp, spec, v))
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            if (nl >= 2) {
                uint64_t v = arg2(args, nargs, &ai);
                spec[si++] = 'l'; spec[si++] = 'l'; spec[si++] = conv; spec[si] = 0;
                if (conv == 'd' || conv == 'i') n = FMT1((long long)v);
                else n = FMT1((unsigned long long)v);
            } else {
                int32_t v = arg1(args, nargs, &ai);
                spec[si++] = conv; spec[si] = 0;
                if (conv == 'd' || conv == 'i') n = FMT1((int)v);
                else n = FMT1((unsigned)v);
            }
            out_put(o, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
            break;
        }
        case 'c': {
            int32_t v = arg1(args, nargs, &ai);
            spec[si++] = 'c'; spec[si] = 0;
            n = FMT1((int)v);
            if (v == 0) { out_put(o, "", 1); break; }
            out_put(o, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
            break;
        }
        case 'p':
            n = snprintf(tmp, sizeof tmp, "0x%x", (unsigned)arg1(args, nargs, &ai));
            out_put(o, tmp, n);
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d = u2d(arg2(args, nargs, &ai));
            spec[si++] = conv; spec[si] = 0;
            if (prec_star && pv > 60) pv = 60;
            n = FMT1(d);
            out_put(o, tmp, n < (int)sizeof tmp ? n : (int)sizeof tmp - 1);
            break;
        }
        case 's': {
            int32_t a = arg1(args, nargs, &ai);
            const char *str = a ? vstr(vm, (uint32_t)a) : "(null)";
            int len = (int)strlen(str);
            int w = 0, p = -1, left = 0, k;
            const char *q = spec + 1;
            while (*q == '-' || *q == '+' || *q == ' ' || *q == '#' || *q == '0') { if (*q == '-') left = 1; q++; }
            if (width_star) { w = wv; if (w < 0) { left = 1; w = -w; } }
            else while (*q >= '0' && *q <= '9') w = w * 10 + (*q++ - '0');
            if (*q == '*') q++;
            if (*q == '.') {
                q++;
                if (prec_star) p = pv;
                else { p = 0; while (*q >= '0' && *q <= '9') p = p * 10 + (*q++ - '0'); }
            }
            if (p >= 0 && p < len) len = p;
            if (!left) for (k = len; k < w; k++) out_put(o, " ", 1);
            out_put(o, str, len);
            if (left) for (k = len; k < w; k++) out_put(o, " ", 1);
            break;
        }
        case 'n': {
            int32_t a = arg1(args, nargs, &ai);
            int32_t v = o->len;
            memcpy(vptr(vm, (uint32_t)a, 4), &v, 4);
            break;
        }
        default:
            out_put(o, pct, (int)(fmt - pct));
            break;
        }
#undef FMT1
    }
    return o->len;
}

/* ------------------------------------------------------------------ */
/* input                                                               */
/* ------------------------------------------------------------------ */

static int stdin_getc(VM *vm)
{
    if (vm->inpos >= vm->inlen) {
        int n;
        con_flush();
        n = le_readline(vm->inbuf, sizeof vm->inbuf - 2, "", 0);
        if (n < 0) return -1;
        n = (int)strlen(vm->inbuf);
        vm->inbuf[n++] = '\n';
        vm->inbuf[n] = 0;
        vm->inlen = n;
        vm->inpos = 0;
    }
    return (unsigned char)vm->inbuf[vm->inpos++];
}

/* a small scanf: %d %i %u %x %o %c %s %f %e %g %[...] %n, with widths and * */
typedef struct {
    VM *vm;
    const char *s;      /* string source, or NULL for stdin */
    int pos;
    int peeked;
} ScanSrc;

static int sc_get(ScanSrc *ss)
{
    int c;
    if (ss->peeked != -2) { c = ss->peeked; ss->peeked = -2; ss->pos++; return c; }
    if (ss->s) {
        c = (unsigned char)ss->s[ss->pos];
        if (!c) return -1;
        ss->pos++;
        return c;
    }
    c = stdin_getc(ss->vm);
    if (c >= 0) ss->pos++;
    return c;
}

static void sc_unget(ScanSrc *ss, int c)
{
    if (c < 0) return;
    ss->pos--;
    if (ss->s) return;
    ss->peeked = c;
}

static int vm_scan(VM *vm, ScanSrc *ss, const char *fmt, const int32_t *args, int nargs)
{
    int assigned = 0, ai = 0, c;
    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*fmt)) fmt++;
            while ((c = sc_get(ss)) >= 0 && isspace(c)) { }
            sc_unget(ss, c);
            continue;
        }
        if (*fmt != '%') {
            c = sc_get(ss);
            if (c != (unsigned char)*fmt) { sc_unget(ss, c); return assigned; }
            fmt++;
            continue;
        }
        fmt++;
        {
            int suppress = 0, width = 0, nl = 0;
            char conv;
            char buf[80];
            int n = 0;
            if (*fmt == '%') {
                c = sc_get(ss);
                if (c != '%') { sc_unget(ss, c); return assigned; }
                fmt++;
                continue;
            }
            if (*fmt == '*') { suppress = 1; fmt++; }
            while (isdigit((unsigned char)*fmt)) width = width * 10 + (*fmt++ - '0');
            while (*fmt == 'l' || *fmt == 'h' || *fmt == 'L' || *fmt == 'q' || *fmt == 'j' || *fmt == 'z') {
                if (*fmt == 'l' || *fmt == 'L' || *fmt == 'q') nl++;
                if (*fmt == 'q' || *fmt == 'j') nl = 2;
                fmt++;
            }
            conv = *fmt++;
            if (!conv) break;
            if (width <= 0 || width > 78) width = 78;
            if (conv == 'n') {
                if (!suppress && ai < nargs) SW(vm, (uint32_t)args[ai++], (uint32_t)ss->pos), chk(vm, (uint32_t)args[ai - 1], 4);
                continue;
            }
            if (conv != 'c' && conv != '[') {
                while ((c = sc_get(ss)) >= 0 && isspace(c)) { }
                sc_unget(ss, c);
            }
            if (conv == 'c') {
                if (width == 78) width = 1;
                for (n = 0; n < width; n++) {
                    c = sc_get(ss);
                    if (c < 0) break;
                    buf[n] = (char)c;
                }
                if (n == 0) return assigned ? assigned : -1;
                if (!suppress && ai < nargs) { memcpy(vptr(vm, (uint32_t)args[ai++], n), buf, n); assigned++; }
                continue;
            }
            if (conv == 's' || conv == '[') {
                char set[96];
                int neg = 0, si = 0;
                if (conv == '[') {
                    if (*fmt == '^') { neg = 1; fmt++; }
                    if (*fmt == ']') set[si++] = *fmt++;
                    while (*fmt && *fmt != ']' && si < 94) set[si++] = *fmt++;
                    if (*fmt == ']') fmt++;
                }
                set[si] = 0;
                while (n < width) {
                    c = sc_get(ss);
                    if (c < 0) break;
                    if (conv == 's' && isspace(c)) { sc_unget(ss, c); break; }
                    if (conv == '[' && ((strchr(set, c) != 0) == neg)) { sc_unget(ss, c); break; }
                    buf[n++] = (char)c;
                }
                if (n == 0) return assigned ? assigned : (c < 0 ? -1 : assigned);
                buf[n] = 0;
                if (!suppress && ai < nargs) { memcpy(vptr(vm, (uint32_t)args[ai++], n + 1), buf, n + 1); assigned++; }
                continue;
            }
            /* numbers: collect characters that can belong to one */
            while (n < width) {
                c = sc_get(ss);
                if (c < 0) break;
                if (isalnum(c) || c == '.' || c == '-' || c == '+') {
                    if ((c == '-' || c == '+') && n > 0 && !(conv != 'x' && (buf[n - 1] == 'e' || buf[n - 1] == 'E'))) { sc_unget(ss, c); break; }
                    if (conv != 'f' && conv != 'e' && conv != 'g' && conv != 'a' && c == '.') { sc_unget(ss, c); break; }
                    if ((conv == 'd' || conv == 'u') && !(isdigit(c) || c == '-' || c == '+')) { sc_unget(ss, c); break; }
                    buf[n++] = (char)c;
                } else { sc_unget(ss, c); break; }
            }
            buf[n] = 0;
            if (n == 0) return assigned ? assigned : (c < 0 ? -1 : assigned);
            if (!suppress && ai < nargs) {
                uint32_t dst = (uint32_t)args[ai++];
                if (conv == 'f' || conv == 'e' || conv == 'g' || conv == 'a') {
                    if (nl) { double d = strtod(buf, 0); memcpy(vptr(vm, dst, 8), &d, 8); }
                    else { float f = strtof(buf, 0); memcpy(vptr(vm, dst, 4), &f, 4); }
                } else {
                    int base = (conv == 'x' || conv == 'X') ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
                    long long v = (conv == 'd' || conv == 'i') ? strtoll(buf, 0, base) : (long long)strtoull(buf, 0, base);
                    if (nl >= 2) memcpy(vptr(vm, dst, 8), &v, 8);
                    else { int32_t w = (int32_t)v; memcpy(vptr(vm, dst, 4), &w, 4); }
                }
                assigned++;
            }
        }
    }
    return assigned;
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

static VFile *getfile(VM *vm, int32_t h)
{
    if (h >= FH_BASE && h < FH_BASE + NFILES && vm->files[h - FH_BASE].f) return &vm->files[h - FH_BASE];
    return 0;
}

static int32_t vm_fopen(VM *vm, const char *path, const char *mode)
{
    char full[VFS_PATH];
    int i;
    for (i = 0; i < NFILES; i++) {
        if (!vm->files[i].f) {
            if (!vfs_resolve(path, full)) return 0;
            vm->files[i].f = vfs_open(full, mode);
            if (!vm->files[i].f) return 0;
            vm->files[i].eof = 0;
            vm->files[i].err = 0;
            return FH_BASE + i;
        }
    }
    return 0;
}

static void close_all(VM *vm)
{
    int i;
    for (i = 0; i < NFILES; i++)
        if (vm->files[i].f) { vfs_close(vm->files[i].f); vm->files[i].f = 0; }
}

static int vm_fgetc(VM *vm, int32_t h)
{
    VFile *f;
    unsigned char c;
    if (h == 1) return stdin_getc(vm);
    f = getfile(vm, h);
    if (!f) return -1;
    if (vfs_read(f->f, &c, 1) != 1) { f->eof = 1; return -1; }
    return c;
}

static int vm_fputs(VM *vm, int32_t h, const char *s, int n)
{
    VFile *f;
    if (h == 2 || h == 3) {
        if (h == 3) con_set_fg(9);
        con_write(s, n);
        if (h == 3) con_default_color();
        return n;
    }
    f = getfile(vm, h);
    if (!f) return -1;
    return vfs_write(f->f, s, n);
}

/* ------------------------------------------------------------------ */
/* calling back into bytecode (qsort)                                  */
/* ------------------------------------------------------------------ */

static void vm_exec(VM *vm);

static int32_t vm_callback(VM *vm, uint32_t fn, int nargs, const int32_t *args)
{
    uint32_t spc = vm->pc, slr = vm->lr;
    int i;
    if (fn >= vm->code_size) vm_fail(vm, "call through an invalid function pointer");
    if (vm->osp + nargs >= OSTK) vm_fail(vm, "operand stack overflow");
    for (i = 0; i < nargs; i++) vm->ost[vm->osp++] = args[i];
    vm->lr = LR_NATIVE;
    vm->pc = fn;
    vm->native_depth++;
    vm_exec(vm);
    vm->native_depth--;
    vm->pc = spc;
    vm->lr = slr;
    return vm->ost[--vm->osp];
}

static void vm_qsort(VM *vm, uint32_t base, int n, int size, uint32_t cmp)
{
    /* insertion sort for small arrays, shell sort otherwise: no recursion */
    uint8_t tmp[256];
    int gap, i, j;
    if (n < 2 || size <= 0) return;
    if (size > (int)sizeof tmp) vm_fail(vm, "qsort element too large");
    vptr(vm, base, (uint32_t)(n * size));
    for (gap = n / 2; gap > 0; gap /= 2) {
        for (i = gap; i < n; i++) {
            memcpy(tmp, vm->mem + base + (uint32_t)(i * size), size);
            for (j = i; j >= gap; j -= gap) {
                int32_t a[2];
                uint32_t tmpaddr;
                /* the comparator needs both elements in VM memory: park tmp on the stack */
                if (vm->sp - (uint32_t)size < vm->stack_limit) vm_fail(vm, "stack overflow");
                vm->sp -= ((uint32_t)size + 7) & ~7u;
                tmpaddr = vm->sp;
                memcpy(vm->mem + tmpaddr, tmp, size);
                a[0] = (int32_t)(base + (uint32_t)((j - gap) * size));
                a[1] = (int32_t)tmpaddr;
                {
                    int32_t r = vm_callback(vm, cmp, 2, a);
                    vm->sp += ((uint32_t)size + 7) & ~7u;
                    if (r <= 0) break;
                }
                memcpy(vm->mem + base + (uint32_t)(j * size), vm->mem + base + (uint32_t)((j - gap) * size), size);
            }
            memcpy(vm->mem + base + (uint32_t)(j * size), tmp, size);
        }
    }
}

/* ------------------------------------------------------------------ */
/* builtins                                                            */
/* ------------------------------------------------------------------ */

enum {
    SYS_PRINTF = 1, SYS_SPRINTF, SYS_SNPRINTF, SYS_FPRINTF, SYS_PUTCHAR, SYS_PUTS,
    SYS_GETCHAR, SYS_FGETS, SYS_GETS, SYS_SCANF, SYS_SSCANF, SYS_FOPEN, SYS_FCLOSE,
    SYS_FGETC, SYS_FPUTC, SYS_FPUTS, SYS_FREAD, SYS_FWRITE, SYS_FSEEK, SYS_FTELL,
    SYS_FEOF, SYS_FFLUSH, SYS_REMOVE, SYS_RENAME, SYS_MKDIR, SYS_REWIND, SYS_FSIZE,
    SYS_STRLEN = 30, SYS_STRCPY, SYS_STRNCPY, SYS_STRCAT, SYS_STRNCAT, SYS_STRCMP,
    SYS_STRNCMP, SYS_STRCHR, SYS_STRRCHR, SYS_STRSTR, SYS_MEMCPY, SYS_MEMMOVE,
    SYS_MEMSET, SYS_MEMCMP, SYS_STRDUP, SYS_STRTOK, SYS_STRCASECMP, SYS_STRERROR,
    SYS_TOUPPER = 50, SYS_TOLOWER, SYS_ISDIGIT, SYS_ISALPHA, SYS_ISALNUM, SYS_ISSPACE,
    SYS_ISUPPER, SYS_ISLOWER, SYS_ISXDIGIT, SYS_ISPUNCT, SYS_ISPRINT,
    SYS_MALLOC = 70, SYS_CALLOC, SYS_REALLOC, SYS_FREE, SYS_ATOI, SYS_ATOF, SYS_STRTOL,
    SYS_ABS, SYS_RAND, SYS_SRAND, SYS_EXIT, SYS_ABORT, SYS_QSORT, SYS_HEAPFREE, SYS_ASSERT,
    SYS_STRTOF = 85, SYS_STRTOD, SYS_ATOLL, SYS_STRTOLL, SYS_LLABS,
    SYS_SIN = 90, SYS_COS, SYS_TAN, SYS_ASIN, SYS_ACOS, SYS_ATAN, SYS_ATAN2, SYS_SQRT,
    SYS_POW, SYS_EXP, SYS_LOG, SYS_LOG10, SYS_FLOOR, SYS_CEIL, SYS_FABS, SYS_FMOD,
    SYS_ROUND, SYS_FMIN, SYS_FMAX,
    SYS_CLS = 120, SYS_GOTOXY, SYS_WHEREX, SYS_WHEREY, SYS_SETCOLOR, SYS_GETKEY,
    SYS_KBHIT, SYS_INKEY, SYS_BEEP, SYS_DELAY, SYS_MILLIS, SYS_BACKLIGHT, SYS_KBDLIGHT,
    SYS_BATTERY, SYS_CPUMHZ, SYS_TEMPERATURE, SYS_SCOLS, SYS_SROWS, SYS_PIXEL,
    SYS_FILLRECT, SYS_RGB, SYS_CURSOR, SYS_PFREAD, SYS_PFWRITE, SYS_TIME, SYS_LINE,
    SYS_TEXTCOLOR, SYS_TEXTBG, SYS_CLOCK, SYS_REFRESH, SYS_CHARGING, SYS_SLEEP,
    SYS_CPUTEMP,
    SYS_SINF = 160, SYS_COSF, SYS_TANF, SYS_ASINF, SYS_ACOSF, SYS_ATANF, SYS_ATAN2F, SYS_SQRTF,
    SYS_POWF, SYS_EXPF, SYS_LOGF, SYS_LOG10F, SYS_FLOORF, SYS_CEILF, SYS_FABSF, SYS_FMODF,
    SYS_ROUNDF, SYS_FMINF, SYS_FMAXF,
    SYS_SETJMP = 180, SYS_LONGJMP, SYS_VPRINTF, SYS_VSPRINTF, SYS_VSNPRINTF, SYS_VFPRINTF,
    SYS_PLAY = 190, SYS_PLAYWAV, SYS_GPIOMODE, SYS_GPIOWRITE, SYS_GPIOREAD, SYS_UARTOPEN,
    SYS_UARTCLOSE, SYS_UARTPUTC, SYS_UARTGETC, SYS_UARTWRITE, SYS_UARTREAD, SYS_UARTAVAIL, SYS_UARTPUTS,
    SYS_PLAYTOWAV,
    SYS_COUNT
};

static void vm_delay(VM *vm, int ms)
{
    uint32_t end;
    con_flush();
    if (ms <= 0) return;
    end = hal_millis() + (uint32_t)ms;
    while ((int32_t)(end - hal_millis()) > 0) {
        if (con_poll_break()) vm_fail(vm, "break");
        hal_sleep_ms(1);
    }
}

/* the cells of a va_list, copied out of VM memory */
static int va_cells(VM *vm, uint32_t ap, int32_t *out, int max)
{
    uint32_t n;
    if (ap < CBC_DATA_BASE || ap >= vm->mem_size) vm_fail(vm, "invalid va_list");
    n = (vm->mem_size - ap) / 4;
    if (n > (uint32_t)max) n = (uint32_t)max;
    memcpy(out, vm->mem + ap, n * 4);
    return (int)n;
}

#define ARGD(i) u2d(((uint64_t)(uint32_t)a[(i) + 1] << 32) | (uint32_t)a[i])
#define RETD(x) return (int64_t)d2u(x)

static int64_t vm_sys(VM *vm, int id, int32_t *a, int n)
{
    char buf[128];
    switch (id) {
    case SYS_PRINTF: {
        Out o = {0, 0, 0, 0};
        if (n < 1) return 0;
        return vm_format(vm, &o, vstr(vm, a[0]), a + 1, n - 1);
    }
    case SYS_SPRINTF: {
        Out o;
        if (n < 2) return 0;
        o.buf = (char *)vptr(vm, a[0], 1);
        o.cap = (int)(vm->mem_size - (uint32_t)a[0]);
        o.len = 0;
        o.file = 0;
        return vm_format(vm, &o, vstr(vm, a[1]), a + 2, n - 2);
    }
    case SYS_SNPRINTF: {
        Out o;
        if (n < 3) return 0;
        o.buf = a[1] > 0 ? (char *)vptr(vm, a[0], a[1]) : buf;
        o.cap = a[1] > 0 ? a[1] : 0;
        o.len = 0;
        o.file = 0;
        return vm_format(vm, &o, vstr(vm, a[2]), a + 3, n - 3);
    }
    case SYS_FPRINTF: {
        Out o = {0, 0, 0, 0};
        if (n < 2) return 0;
        if (a[0] == 2 || a[0] == 3) {
            if (a[0] == 3) con_set_fg(9);
            n = vm_format(vm, &o, vstr(vm, a[1]), a + 2, n - 2);
            if (a[0] == 3) con_default_color();
            return n;
        }
        o.file = getfile(vm, a[0]);
        if (!o.file) return -1;
        return vm_format(vm, &o, vstr(vm, a[1]), a + 2, n - 2);
    }
    case SYS_PUTCHAR: con_putc(a[0] & 255); return a[0] & 255;
    case SYS_PUTS: con_puts(vstr(vm, a[0])); con_putc('\n'); return 1;
    case SYS_GETCHAR: return stdin_getc(vm);
    case SYS_FGETS: {
        int i = 0, c, max = a[1];
        char *d;
        if (max <= 0) return 0;
        d = (char *)vptr(vm, a[0], max);
        while (i < max - 1) {
            c = vm_fgetc(vm, a[2]);
            if (c < 0) break;
            d[i++] = (char)c;
            if (c == '\n') break;
        }
        d[i] = 0;
        return i ? a[0] : 0;
    }
    case SYS_GETS: {
        int i = 0, c;
        char *d = (char *)vptr(vm, a[0], 1);
        uint32_t room = vm->mem_size - (uint32_t)a[0];
        while ((uint32_t)i + 1 < room) {
            c = stdin_getc(vm);
            if (c < 0 || c == '\n') break;
            d[i++] = (char)c;
        }
        d[i] = 0;
        return a[0];
    }
    case SYS_SCANF: {
        ScanSrc ss;
        ss.vm = vm; ss.s = 0; ss.pos = 0; ss.peeked = -2;
        n = vm_scan(vm, &ss, vstr(vm, a[0]), a + 1, n - 1);
        if (ss.peeked >= 0) vm->inpos--;      /* give back the lookahead */
        return n;
    }
    case SYS_SSCANF: {
        ScanSrc ss;
        ss.vm = vm; ss.s = vstr(vm, a[0]); ss.pos = 0; ss.peeked = -2;
        return vm_scan(vm, &ss, vstr(vm, a[1]), a + 2, n - 2);
    }
    case SYS_FOPEN: return vm_fopen(vm, vstr(vm, a[0]), vstr(vm, a[1]));
    case SYS_FCLOSE: {
        VFile *f = getfile(vm, a[0]);
        if (!f) return -1;
        vfs_close(f->f);
        f->f = 0;
        return 0;
    }
    case SYS_FGETC: return vm_fgetc(vm, a[0]);
    case SYS_FPUTC: { char c = (char)a[0]; return vm_fputs(vm, a[1], &c, 1) == 1 ? (a[0] & 255) : -1; }
    case SYS_FPUTS: { char *s = vstr(vm, a[0]); return vm_fputs(vm, a[1], s, (int)strlen(s)) >= 0 ? 1 : -1; }
    case SYS_FREAD: {
        VFile *f;
        int total = a[1] * a[2], got;
        if (total <= 0 || a[1] <= 0) return 0;
        if (a[3] == 1) {
            char *d = (char *)vptr(vm, a[0], total);
            int i, c;
            for (i = 0; i < total; i++) { c = stdin_getc(vm); if (c < 0) break; d[i] = (char)c; }
            return i / a[1];
        }
        f = getfile(vm, a[3]);
        if (!f) return 0;
        got = vfs_read(f->f, vptr(vm, a[0], total), total);
        if (got < total) f->eof = 1;
        return got > 0 ? got / a[1] : 0;
    }
    case SYS_FWRITE: {
        int total = a[1] * a[2], put;
        if (total <= 0 || a[1] <= 0) return 0;
        put = vm_fputs(vm, a[3], (char *)vptr(vm, a[0], total), total);
        return put > 0 ? put / a[1] : 0;
    }
    case SYS_FSEEK: {
        VFile *f = getfile(vm, a[0]);
        if (!f) return -1;
        f->eof = 0;
        return vfs_seek(f->f, a[1], a[2]);
    }
    case SYS_FTELL: { VFile *f = getfile(vm, a[0]); return f ? vfs_tell(f->f) : -1; }
    case SYS_FEOF: {
        VFile *f = getfile(vm, a[0]);
        if (a[0] == 1) return 0;
        return f ? (f->eof || vfs_eof(f->f)) : 1;
    }
    case SYS_FFLUSH: { VFile *f = getfile(vm, a[0]); if (f) vfs_flush(f->f); else con_flush(); return 0; }
    case SYS_REMOVE: {
        char full[VFS_PATH];
        if (!vfs_resolve(vstr(vm, a[0]), full)) return -1;
        return vfs_remove(full) ? 0 : -1;
    }
    case SYS_RENAME: {
        char f1[VFS_PATH], f2[VFS_PATH];
        if (!vfs_resolve(vstr(vm, a[0]), f1) || !vfs_resolve(vstr(vm, a[1]), f2)) return -1;
        return vfs_rename(f1, f2) ? 0 : -1;
    }
    case SYS_MKDIR: {
        char full[VFS_PATH];
        if (!vfs_resolve(vstr(vm, a[0]), full)) return -1;
        return vfs_mkdir(full) ? 0 : -1;
    }
    case SYS_REWIND: { VFile *f = getfile(vm, a[0]); if (f) { f->eof = 0; vfs_seek(f->f, 0, 0); } return 0; }
    case SYS_FSIZE: { VFile *f = getfile(vm, a[0]); return f ? vfs_size(f->f) : -1; }

    case SYS_STRLEN: return (int32_t)strlen(vstr(vm, a[0]));
    case SYS_STRCPY: {
        char *s = vstr(vm, a[1]);
        size_t l = strlen(s) + 1;
        memmove(vptr(vm, a[0], (uint32_t)l), s, l);
        return a[0];
    }
    case SYS_STRNCPY: {
        char *s = vstr(vm, a[1]);
        char *d;
        int l = (int)strlen(s);
        if (a[2] <= 0) return a[0];
        d = (char *)vptr(vm, a[0], a[2]);
        if (l > a[2]) l = a[2];
        memmove(d, s, l);
        if (l < a[2]) memset(d + l, 0, a[2] - l);
        return a[0];
    }
    case SYS_STRCAT: {
        char *d = vstr(vm, a[0]);
        char *s = vstr(vm, a[1]);
        size_t dl = strlen(d), sl = strlen(s) + 1;
        vptr(vm, a[0] + (uint32_t)dl, (uint32_t)sl);
        memmove(d + dl, s, sl);
        return a[0];
    }
    case SYS_STRNCAT: {
        char *d = vstr(vm, a[0]);
        char *s = vstr(vm, a[1]);
        size_t dl = strlen(d), sl = strlen(s);
        if (a[2] < 0) return a[0];
        if (sl > (size_t)a[2]) sl = (size_t)a[2];
        vptr(vm, a[0] + (uint32_t)dl, (uint32_t)sl + 1);
        memmove(d + dl, s, sl);
        d[dl + sl] = 0;
        return a[0];
    }
    case SYS_STRCMP: { int r = strcmp(vstr(vm, a[0]), vstr(vm, a[1])); return r < 0 ? -1 : r > 0; }
    case SYS_STRNCMP: {
        int r;
        if (a[2] <= 0) return 0;
        r = strncmp(vstr(vm, a[0]), vstr(vm, a[1]), (size_t)a[2]);
        return r < 0 ? -1 : r > 0;
    }
    case SYS_STRCHR: {
        char *s = vstr(vm, a[0]);
        char *p = strchr(s, a[1] & 255);
        return p ? (int32_t)vaddr(vm, p) : 0;
    }
    case SYS_STRRCHR: {
        char *s = vstr(vm, a[0]);
        char *p = strrchr(s, a[1] & 255);
        return p ? (int32_t)vaddr(vm, p) : 0;
    }
    case SYS_STRSTR: {
        char *s = vstr(vm, a[0]);
        char *p = strstr(s, vstr(vm, a[1]));
        return p ? (int32_t)vaddr(vm, p) : 0;
    }
    case SYS_MEMCPY:
    case SYS_MEMMOVE:
        if (a[2] <= 0) return a[0];
        memmove(vptr(vm, a[0], a[2]), vptr(vm, a[1], a[2]), (size_t)a[2]);
        return a[0];
    case SYS_MEMSET:
        if (a[2] <= 0) return a[0];
        memset(vptr(vm, a[0], a[2]), a[1] & 255, (size_t)a[2]);
        return a[0];
    case SYS_MEMCMP: {
        int r;
        if (a[2] <= 0) return 0;
        r = memcmp(vptr(vm, a[0], a[2]), vptr(vm, a[1], a[2]), (size_t)a[2]);
        return r < 0 ? -1 : r > 0;
    }
    case SYS_STRDUP: {
        char *s = vstr(vm, a[0]);
        uint32_t l = (uint32_t)strlen(s) + 1, p = vm_malloc(vm, l);
        if (p) memcpy(vm->mem + p, vm->mem + a[0], l);
        return (int32_t)p;
    }
    case SYS_STRTOK: {
        char *delim = vstr(vm, a[1]);
        uint32_t p = a[0] ? (uint32_t)a[0] : vm->strtok_pos;
        char *s;
        if (!p) return 0;
        s = vstr(vm, p);
        s += strspn(s, delim);
        if (!*s) { vm->strtok_pos = 0; return 0; }
        {
            char *e = s + strcspn(s, delim);
            if (*e) { *e = 0; vm->strtok_pos = vaddr(vm, e + 1); }
            else vm->strtok_pos = 0;
        }
        return (int32_t)vaddr(vm, s);
    }
    case SYS_STRCASECMP: {
        const char *x = vstr(vm, a[0]), *y = vstr(vm, a[1]);
        while (*x && tolower((unsigned char)*x) == tolower((unsigned char)*y)) { x++; y++; }
        return tolower((unsigned char)*x) - tolower((unsigned char)*y);
    }
    case SYS_STRERROR: return 0;

    case SYS_TOUPPER: return (a[0] >= 'a' && a[0] <= 'z') ? a[0] - 32 : a[0];
    case SYS_TOLOWER: return (a[0] >= 'A' && a[0] <= 'Z') ? a[0] + 32 : a[0];
    case SYS_ISDIGIT: return a[0] >= '0' && a[0] <= '9';
    case SYS_ISALPHA: return (a[0] >= 'a' && a[0] <= 'z') || (a[0] >= 'A' && a[0] <= 'Z');
    case SYS_ISALNUM: return (a[0] >= 'a' && a[0] <= 'z') || (a[0] >= 'A' && a[0] <= 'Z') || (a[0] >= '0' && a[0] <= '9');
    case SYS_ISSPACE: return a[0] == ' ' || (a[0] >= 9 && a[0] <= 13);
    case SYS_ISUPPER: return a[0] >= 'A' && a[0] <= 'Z';
    case SYS_ISLOWER: return a[0] >= 'a' && a[0] <= 'z';
    case SYS_ISXDIGIT: return (a[0] >= '0' && a[0] <= '9') || (a[0] >= 'a' && a[0] <= 'f') || (a[0] >= 'A' && a[0] <= 'F');
    case SYS_ISPUNCT: return a[0] > 32 && a[0] < 127 && !((a[0] >= 'a' && a[0] <= 'z') || (a[0] >= 'A' && a[0] <= 'Z') || (a[0] >= '0' && a[0] <= '9'));
    case SYS_ISPRINT: return a[0] >= 32 && a[0] < 127;

    case SYS_MALLOC: return a[0] < 0 ? 0 : (int32_t)vm_malloc(vm, (uint32_t)a[0]);
    case SYS_CALLOC: {
        uint32_t tot, p;
        if (a[0] < 0 || a[1] < 0) return 0;
        tot = (uint32_t)a[0] * (uint32_t)a[1];
        if (a[1] && tot / (uint32_t)a[1] != (uint32_t)a[0]) return 0;
        p = vm_malloc(vm, tot);
        if (p) memset(vm->mem + p, 0, tot);
        return (int32_t)p;
    }
    case SYS_REALLOC: return a[1] < 0 ? 0 : (int32_t)vm_realloc(vm, (uint32_t)a[0], (uint32_t)a[1]);
    case SYS_FREE: vm_free(vm, (uint32_t)a[0]); return 0;
    case SYS_ATOI: return (int32_t)strtol(vstr(vm, a[0]), 0, 10);
    case SYS_ATOF: RETD(strtod(vstr(vm, a[0]), 0));
    case SYS_STRTOF: case SYS_STRTOD: case SYS_STRTOLL: {
        char *str = vstr(vm, a[0]);
        char *e;
        int64_t r;
        if (id == SYS_STRTOF) r = (uint32_t)f2i(strtof(str, &e));
        else if (id == SYS_STRTOD) r = (int64_t)d2u(strtod(str, &e));
        else r = (int64_t)strtoll(str, &e, a[2]);
        if (a[1]) { uint32_t ea = vaddr(vm, e); memcpy(vptr(vm, a[1], 4), &ea, 4); }
        return r;
    }
    case SYS_ATOLL: return (int64_t)strtoll(vstr(vm, a[0]), 0, 10);
    case SYS_LLABS: { int64_t v = (int64_t)(((uint64_t)(uint32_t)a[1] << 32) | (uint32_t)a[0]); return v < 0 ? -v : v; }
    case SYS_SETJMP: {
        /* jmp_buf: pc fp sp rec lr osp depth magic, then the 8 operand
         * cells under the call (the expression setjmp sits in) */
        uint32_t b[16];
        int k;
        b[0] = vm->pc; b[1] = vm->fp; b[2] = vm->sp; b[3] = vm->rec; b[4] = vm->lr;
        b[5] = (uint32_t)vm->osp; b[6] = (uint32_t)vm->native_depth; b[7] = 0x4A4D5042;
        for (k = 0; k < 8; k++) b[8 + k] = vm->osp - 8 + k >= 0 ? (uint32_t)vm->ost[vm->osp - 8 + k] : 0;
        memcpy(vptr(vm, a[0], 64), b, 64);
        return 0;
    }
    case SYS_LONGJMP: {
        uint32_t b[16];
        int k;
        memcpy(b, vptr(vm, a[0], 64), 64);
        if (b[7] != 0x4A4D5042 || (int)b[6] != vm->native_depth || b[5] > OSTK) vm_fail(vm, "longjmp to an invalid jmp_buf");
        vm->pc = b[0]; vm->fp = b[1]; vm->sp = b[2]; vm->rec = b[3]; vm->lr = b[4];
        vm->osp = (int)b[5];
        for (k = 0; k < 8; k++) if (vm->osp - 8 + k >= 0) vm->ost[vm->osp - 8 + k] = (int32_t)b[8 + k];
        return a[1] ? a[1] : 1;
    }
    case SYS_VPRINTF: case SYS_VSPRINTF: case SYS_VSNPRINTF: case SYS_VFPRINTF: {
        int32_t cells[64];
        Out o = {0, 0, 0, 0};
        int fi = id == SYS_VPRINTF ? 0 : id == SYS_VSNPRINTF ? 2 : 1;
        int nc = va_cells(vm, (uint32_t)a[fi + 1], cells, 64);
        if (id == SYS_VSPRINTF) { o.buf = (char *)vptr(vm, a[0], 1); o.cap = (int)(vm->mem_size - (uint32_t)a[0]); }
        if (id == SYS_VSNPRINTF) { o.buf = a[1] > 0 ? (char *)vptr(vm, a[0], a[1]) : buf; o.cap = a[1] > 0 ? a[1] : 0; }
        if (id == SYS_VFPRINTF) {
            if (a[0] != 2 && a[0] != 3) { o.file = getfile(vm, a[0]); if (!o.file) return -1; }
        }
        return vm_format(vm, &o, vstr(vm, a[fi]), cells, nc);
    }
    case SYS_STRTOL: {
        char *s = vstr(vm, a[0]);
        char *e;
        long v = strtol(s, &e, a[2]);
        if (a[1]) { uint32_t ea = vaddr(vm, e); memcpy(vptr(vm, a[1], 4), &ea, 4); }
        return (int32_t)v;
    }
    case SYS_ABS: return a[0] < 0 ? -a[0] : a[0];
    case SYS_RAND:
        vm->rand_state = vm->rand_state * 1103515245u + 12345u;
        return (int32_t)((vm->rand_state >> 16) & 0x7FFF);
    case SYS_SRAND: vm->rand_state = (uint32_t)a[0]; return 0;
    case SYS_EXIT: vm->exit_code = a[0]; longjmp(vm->jb, 1);
    case SYS_ABORT: vm_fail(vm, "abort() called"); return 0;
    case SYS_QSORT: vm_qsort(vm, (uint32_t)a[0], a[1], a[2], (uint32_t)a[3]); return 0;
    case SYS_HEAPFREE: return (int32_t)heap_free_bytes(vm);
    case SYS_ASSERT: if (!a[0]) vm_fail(vm, "assertion failed"); return 0;

    case SYS_SIN: RETD(sin(ARGD(0)));
    case SYS_COS: RETD(cos(ARGD(0)));
    case SYS_TAN: RETD(tan(ARGD(0)));
    case SYS_ASIN: RETD(asin(ARGD(0)));
    case SYS_ACOS: RETD(acos(ARGD(0)));
    case SYS_ATAN: RETD(atan(ARGD(0)));
    case SYS_ATAN2: RETD(atan2(ARGD(0), ARGD(2)));
    case SYS_SQRT: RETD(sqrt(ARGD(0)));
    case SYS_POW: RETD(pow(ARGD(0), ARGD(2)));
    case SYS_EXP: RETD(exp(ARGD(0)));
    case SYS_LOG: RETD(log(ARGD(0)));
    case SYS_LOG10: RETD(log10(ARGD(0)));
    case SYS_FLOOR: RETD(floor(ARGD(0)));
    case SYS_CEIL: RETD(ceil(ARGD(0)));
    case SYS_FABS: RETD(fabs(ARGD(0)));
    case SYS_FMOD: RETD(fmod(ARGD(0), ARGD(2)));
    case SYS_ROUND: RETD(round(ARGD(0)));
    case SYS_FMIN: RETD(fmin(ARGD(0), ARGD(2)));
    case SYS_FMAX: RETD(fmax(ARGD(0), ARGD(2)));
    case SYS_SINF: return (uint32_t)f2i(sinf(i2f(a[0])));
    case SYS_COSF: return (uint32_t)f2i(cosf(i2f(a[0])));
    case SYS_TANF: return (uint32_t)f2i(tanf(i2f(a[0])));
    case SYS_ASINF: return (uint32_t)f2i(asinf(i2f(a[0])));
    case SYS_ACOSF: return (uint32_t)f2i(acosf(i2f(a[0])));
    case SYS_ATANF: return (uint32_t)f2i(atanf(i2f(a[0])));
    case SYS_ATAN2F: return (uint32_t)f2i(atan2f(i2f(a[0]), i2f(a[1])));
    case SYS_SQRTF: return (uint32_t)f2i(sqrtf(i2f(a[0])));
    case SYS_POWF: return (uint32_t)f2i(powf(i2f(a[0]), i2f(a[1])));
    case SYS_EXPF: return (uint32_t)f2i(expf(i2f(a[0])));
    case SYS_LOGF: return (uint32_t)f2i(logf(i2f(a[0])));
    case SYS_LOG10F: return (uint32_t)f2i(log10f(i2f(a[0])));
    case SYS_FLOORF: return (uint32_t)f2i(floorf(i2f(a[0])));
    case SYS_CEILF: return (uint32_t)f2i(ceilf(i2f(a[0])));
    case SYS_FABSF: return (uint32_t)f2i(fabsf(i2f(a[0])));
    case SYS_FMODF: return (uint32_t)f2i(fmodf(i2f(a[0]), i2f(a[1])));
    case SYS_ROUNDF: return (uint32_t)f2i(roundf(i2f(a[0])));
    case SYS_FMINF: return (uint32_t)f2i(fminf(i2f(a[0]), i2f(a[1])));
    case SYS_FMAXF: return (uint32_t)f2i(fmaxf(i2f(a[0]), i2f(a[1])));

    case SYS_CLS: con_clear(); return 0;
    case SYS_GOTOXY: con_gotoxy(a[0], a[1]); return 0;
    case SYS_WHEREX: return con_x();
    case SYS_WHEREY: return con_y();
    case SYS_SETCOLOR: con_set_color(a[0], a[1]); return 0;
    case SYS_GETKEY: {
        int k;
        con_flush();
        while ((k = con_inkey()) < 0) {
            if (con_poll_break()) vm_fail(vm, "break");
            hal_sleep_ms(2);
        }
        if (conf_is_break(k)) vm_fail(vm, "break");
        return k;
    }
    case SYS_KBHIT: {
        int k;
        if (!con_kbhit()) return 0;
        k = con_inkey();
        if (conf_is_break(k)) vm_fail(vm, "break");
        con_unget(k);
        return 1;
    }
    case SYS_INKEY: {
        int k = con_inkey();
        if (k >= 0 && conf_is_break(k)) vm_fail(vm, "break");
        return k;
    }
    case SYS_BEEP:
        if (!g_conf.mute) {
            if (a[1] > 0) { hal_tone(a[0], 0); vm_delay(vm, a[1] > 10000 ? 10000 : a[1]); hal_tone_off(); }
            else hal_tone(a[0], 0);
        } else if (a[1] > 0) vm_delay(vm, a[1] > 10000 ? 10000 : a[1]);
        return 0;
    case SYS_DELAY: vm_delay(vm, a[0]); return 0;
    case SYS_MILLIS: case SYS_CLOCK: return (int32_t)(hal_millis() - vm->t0);
    case SYS_BACKLIGHT: hal_backlight(a[0] < 0 ? 0 : a[0] > 255 ? 255 : a[0]); return 0;
    case SYS_KBDLIGHT: hal_kbdlight(a[0] < 0 ? 0 : a[0] > 255 ? 255 : a[0]); return 0;
    case SYS_BATTERY: return hal_battery(0);
    case SYS_CHARGING: { int ch = 0; hal_battery(&ch); return ch; }
    case SYS_CPUMHZ: return hal_cpu_khz() / 1000;
    case SYS_TEMPERATURE: return (uint32_t)f2i(hal_temp_c());
    case SYS_PLAY: case SYS_PLAYWAV: {
        int r = id == SYS_PLAY ? play_notes(vstr(vm, a[0])) : play_wav(vstr(vm, a[0]));
        if (r == 1) vm_fail(vm, "break");
        return r;
    }
    case SYS_PLAYTOWAV: {
        char path[VFS_PATH];
        int r;
        snprintf(path, sizeof path, "%s", vstr(vm, a[1]));
        r = play_to_wav(vstr(vm, a[0]), path, 0);
        if (r == 1) vm_fail(vm, "break");
        return r;
    }
    case SYS_GPIOMODE: return hal_gpio_mode(a[0], a[1]) ? 0 : -1;
    case SYS_GPIOWRITE: return hal_gpio_write(a[0], a[1]) ? 0 : -1;
    case SYS_GPIOREAD: return hal_gpio_read(a[0]);
    case SYS_UARTOPEN:
        if (!hal_uart_open(a[0], a[1])) return -1;
        vm->uarts_open |= 1 << (a[0] & 1);
        return 0;
    case SYS_UARTCLOSE: hal_uart_close(a[0]); vm->uarts_open &= ~(1 << (a[0] & 1)); return 0;
    case SYS_UARTPUTC: return hal_uart_putc(a[0], a[1]);
    case SYS_UARTGETC: return hal_uart_getc(a[0]);
    case SYS_UARTAVAIL: return hal_uart_available(a[0]);
    case SYS_UARTWRITE: case SYS_UARTPUTS: {
        const char *p = id == SYS_UARTPUTS ? vstr(vm, a[1]) : (const char *)vptr(vm, a[1], a[2] > 0 ? a[2] : 0);
        int k, len = id == SYS_UARTPUTS ? (int)strlen(p) : a[2];
        for (k = 0; k < len; k++) if (hal_uart_putc(a[0], (unsigned char)p[k]) < 0) return -1;
        return len;
    }
    case SYS_UARTREAD: {
        char *d;
        int k = 0, c;
        if (a[2] <= 0) return 0;
        d = (char *)vptr(vm, a[1], a[2]);
        while (k < a[2] && (c = hal_uart_getc(a[0])) >= 0) d[k++] = (char)c;
        return k;
    }
    case SYS_CPUTEMP: return (int32_t)(hal_temp_c() + 0.5f);
    case SYS_SCOLS: return con_cols();
    case SYS_SROWS: return con_rows();
    case SYS_PIXEL: con_flush(); hal_pixel(a[0], a[1], (uint16_t)a[2]); return 0;
    case SYS_FILLRECT: con_flush(); hal_fill(a[0], a[1], a[2], a[3], (uint16_t)a[4]); return 0;
    case SYS_LINE: con_flush(); hal_line(a[0], a[1], a[2], a[3], (uint16_t)a[4]); return 0;
    case SYS_RGB: return ((a[0] & 0xF8) << 8) | ((a[1] & 0xFC) << 3) | ((a[2] & 0xF8) >> 3);
    case SYS_CURSOR: con_cursor(a[0]); return 0;
    case SYS_PFREAD: {
        int len, kind;
        const void *p = pf_find(vstr(vm, a[0]), &len, &kind);
        if (!p) return -1;
        if (len > a[2]) len = a[2];
        if (len > 0) memcpy(vptr(vm, a[1], len), p, len);
        return len;
    }
    case SYS_PFWRITE: {
        int r;
        if (a[2] < 0) return -1;
        r = pf_put(vstr(vm, a[0]), a[2] ? vptr(vm, a[1], a[2]) : "", a[2], PF_RAW);
        return r == PF_OK ? a[2] : r;
    }
    case SYS_TIME: return (int32_t)(hal_millis() / 1000);
    case SYS_TEXTCOLOR: con_set_fg(a[0]); return 0;
    case SYS_TEXTBG: con_set_bg(a[0]); return 0;
    case SYS_REFRESH: con_flush(); return 0;
    case SYS_SLEEP: vm_delay(vm, a[0] * 1000); return 0;
    default:
        snprintf(buf, sizeof buf, "unknown builtin %d", id);
        vm_fail(vm, buf);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* the interpreter                                                     */
/* ------------------------------------------------------------------ */

/* conversions shared by the direct opcodes and OP_CVTU */
static int conv_apply(int32_t *ost, int *posp, int op)
{
    int osp = *posp;
    uint64_t u;
    double d;
    float f;
#define LO ost[osp - 2]
#define HI ost[osp - 1]
#define GET64() (((uint64_t)(uint32_t)HI << 32) | (uint32_t)LO)
#define SET64(v) do { uint64_t _v = (v); LO = (int32_t)(uint32_t)_v; HI = (int32_t)(uint32_t)(_v >> 32); } while (0)
    switch (op) {
    case OP_ITOF: ost[osp - 1] = f2i((float)ost[osp - 1]); break;
    case OP_UTOF: ost[osp - 1] = f2i((float)(uint32_t)ost[osp - 1]); break;
    case OP_FTOI: case OP_FTOU: {
        f = i2f(ost[osp - 1]);
        if (f != f) ost[osp - 1] = 0;
        else if (op == OP_FTOI) ost[osp - 1] = f >= 2147483647.0f ? 2147483647 : f <= -2147483648.0f ? (int32_t)0x80000000u : (int32_t)f;
        else ost[osp - 1] = f <= 0 ? 0 : f >= 4294967295.0f ? -1 : (int32_t)(uint32_t)f;
        break;
    }
    case OP_SEXT8: ost[osp - 1] = (int8_t)ost[osp - 1]; break;
    case OP_ZEXT8: ost[osp - 1] = (uint8_t)ost[osp - 1]; break;
    case OP_SEXT16: ost[osp - 1] = (int16_t)ost[osp - 1]; break;
    case OP_ZEXT16: ost[osp - 1] = (uint16_t)ost[osp - 1]; break;
    case OP_I2L: ost[osp] = ost[osp - 1] < 0 ? -1 : 0; osp++; break;
    case OP_U2L: ost[osp] = 0; osp++; break;
    case OP_I2D: d = (double)ost[osp - 1]; u = d2u(d); ost[osp - 1] = (int32_t)(uint32_t)u; ost[osp] = (int32_t)(uint32_t)(u >> 32); osp++; break;
    case OP_U2D: d = (double)(uint32_t)ost[osp - 1]; u = d2u(d); ost[osp - 1] = (int32_t)(uint32_t)u; ost[osp] = (int32_t)(uint32_t)(u >> 32); osp++; break;
    case OP_F2D: d = (double)i2f(ost[osp - 1]); u = d2u(d); ost[osp - 1] = (int32_t)(uint32_t)u; ost[osp] = (int32_t)(uint32_t)(u >> 32); osp++; break;
    case OP_L2I: osp--; break;
    case OP_L2B: u = GET64(); osp--; ost[osp - 1] = u != 0; break;
    case OP_D2B: d = u2d(GET64()); osp--; ost[osp - 1] = d != 0; break;
    case OP_L2D: SET64(d2u((double)(int64_t)GET64())); break;
    case OP_UL2D: SET64(d2u((double)GET64())); break;
    case OP_D2F: d = u2d(GET64()); osp--; ost[osp - 1] = f2i((float)d); break;
    case OP_D2I: case OP_D2U: {
        d = u2d(GET64());
        osp--;
        if (d != d) ost[osp - 1] = 0;
        else if (op == OP_D2I) ost[osp - 1] = d >= 2147483647.0 ? 2147483647 : d <= -2147483648.0 ? (int32_t)0x80000000u : (int32_t)d;
        else ost[osp - 1] = d <= 0 ? 0 : d >= 4294967295.0 ? -1 : (int32_t)(uint32_t)d;
        break;
    }
    case OP_D2L: case OP_D2UL: {
        d = u2d(GET64());
        if (d != d) u = 0;
        else if (op == OP_D2L) u = d >= 9.2233720368547758e18 ? 0x7FFFFFFFFFFFFFFFull : d <= -9.2233720368547758e18 ? 0x8000000000000000ull : (uint64_t)(int64_t)d;
        else u = d <= 0 ? 0 : d >= 1.8446744073709552e19 ? ~0ull : (uint64_t)d;
        SET64(u);
        break;
    }
    default: return 0;
    }
#undef LO
#undef HI
#undef GET64
#undef SET64
    *posp = osp;
    return 1;
}

#define PUSH(v) do { if (osp >= OSTK) goto ostk_over; ost[osp++] = (int32_t)(v); } while (0)
#define POP() (ost[--osp])
#define TOP ost[osp - 1]
#define NXT ost[osp - 2]
#define RD8() ((int8_t)code[pc++])
#define RD16() (pc += 2, (int16_t)(code[pc - 2] | (code[pc - 1] << 8)))
#define RD32() (pc += 4, (int32_t)rd32(code + pc - 4))
#define MCHK(a, n) do { uint32_t _a = (a); if (_a < CBC_DATA_BASE || _a > memlim - (n)) { vm->cur_op_pc = opc; chk(vm, _a, (n)); } } while (0)
/* 64-bit values: low word below, high word on top */
#define TOP64 (((uint64_t)(uint32_t)ost[osp - 1] << 32) | (uint32_t)ost[osp - 2])
#define POP64(v) do { v = TOP64; osp -= 2; } while (0)
#define SETTOP64(v) do { uint64_t _v = (v); ost[osp - 2] = (int32_t)(uint32_t)_v; ost[osp - 1] = (int32_t)(uint32_t)(_v >> 32); } while (0)
#define PUSH64(v) do { uint64_t _w = (v); if (osp >= OSTK - 1) goto ostk_over; ost[osp] = (int32_t)(uint32_t)_w; ost[osp + 1] = (int32_t)(uint32_t)(_w >> 32); osp += 2; } while (0)
#define REC 24                  /* frame record: lr fp rec sp vacount pad */

static void vm_exec(VM *vm)
{
    const uint8_t *code = vm->code;
    uint8_t *mem = vm->mem;
    int32_t *ost = vm->ost;
    int osp = vm->osp;
    uint32_t pc = vm->pc, fp = vm->fp, sp = vm->sp, rec = vm->rec, lr = vm->lr;
    uint32_t memlim = vm->mem_size;
    uint32_t csize = vm->code_size;
    uint32_t opc = pc;
    uint32_t budget = 4096;
    int32_t a, b;
    uint32_t ua, ub;
    uint64_t la, lb;

#define SYNC() do { vm->osp = osp; vm->pc = pc; vm->fp = fp; vm->sp = sp; vm->rec = rec; vm->lr = lr; vm->cur_op_pc = opc; \
                     vm->icount += 4096 - budget; budget = 4096; } while (0)
#define LOAD() do { osp = vm->osp; pc = vm->pc; fp = vm->fp; sp = vm->sp; rec = vm->rec; lr = vm->lr; } while (0)

#if !defined(__GNUC__)
#error "the VM uses GNU C computed goto"
#endif
    /* computed goto: one indirect jump per instruction */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Woverride-init"
    static void *const tab[256] = {
        [0 ... 255] = &&op_bad,
#define T(o) [OP_##o] = &&op_##o
        T(HALT), T(IMM), T(IMM8), T(IMM16), T(LEA), T(LEA32), T(LDB), T(LDBU), T(LDH), T(LDHU), T(LDW),
        T(STB), T(STH), T(STW), T(ADD), T(SUB), T(MUL), T(DIV), T(MOD), T(DIVU), T(MODU), T(AND), T(OR),
        T(XOR), T(SHL), T(SHR), T(SAR), T(NEG), T(NOT), T(LNOT), T(EQ), T(NE), T(LT), T(LE), T(GT), T(GE),
        T(LTU), T(LEU), T(GTU), T(GEU), T(FADD), T(FSUB), T(FMUL), T(FDIV), T(FNEG), T(FEQ), T(FNE), T(FLT),
        T(FLE), T(FGT), T(FGE), T(ITOF), T(UTOF), T(FTOI), T(FTOU), T(ITOF2), T(UTOF2), T(SEXT8), T(ZEXT8),
        T(SEXT16), T(ZEXT16), T(JMP), T(JZ), T(JNZ), T(CALL), T(CALLI), T(ENTER), T(RET), T(RETV), T(POP),
        T(DUP), T(SWAP), T(MEMCPY), T(SYS), T(INC), T(LLW), T(MEMSET), T(ADDI), T(LGW), T(LDD), T(STD),
        T(LADD), T(LSUB), T(LMUL), T(LDIV), T(LMOD), T(LDIVU), T(LMODU), T(LAND), T(LOR), T(LXOR), T(LSHL),
        T(LSHR), T(LSAR), T(LNEG), T(LCOM), T(LEQ), T(LNE), T(LLT), T(LLE), T(LGT), T(LGE), T(LLTU), T(LLEU),
        T(LGTU), T(LGEU), T(DADD), T(DSUB), T(DMUL), T(DDIV), T(DNEG), T(DEQ), T(DNE), T(DLT), T(DLE), T(DGT),
        T(DGE), T(I2L), T(U2L), T(L2I), T(I2D), T(U2D), T(L2D), T(UL2D), T(F2D), T(D2F), T(D2I), T(D2U),
        T(D2L), T(D2UL), T(L2B), T(D2B), T(CVTU), T(ENTERV), T(VASTART), T(STWP), T(STBP), T(STHP), T(STDP),
        T(JEQ), T(JNE), T(JLT), T(JLE), T(JGT), T(JGE),
#undef T
    };
#pragma GCC diagnostic pop
#define CASE(o) op_##o:
#define NEXT do { if (--budget == 0) goto housekeeping; opc = pc; if (pc >= csize) goto bad_pc; goto *tab[code[pc++]]; } while (0)

    for (;;) {
        NEXT;
        CASE(HALT) SYNC(); return;
        CASE(IMM) a = RD32(); PUSH(a); NEXT;
        CASE(IMM8) a = RD8(); PUSH(a); NEXT;
        CASE(IMM16) a = RD16(); PUSH(a); NEXT;
        CASE(LEA) a = RD16(); PUSH(fp + (uint32_t)a); NEXT;
        CASE(LEA32) a = RD32(); PUSH(fp + (uint32_t)a); NEXT;
        CASE(LLW) a = RD16(); ua = fp + (uint32_t)a; MCHK(ua, 4); PUSH(rd32(mem + ua)); NEXT;
        CASE(LGW) ua = (uint32_t)RD32(); MCHK(ua, 4); PUSH(rd32(mem + ua)); NEXT;
        CASE(LDB) ua = (uint32_t)TOP; MCHK(ua, 1); TOP = (int8_t)mem[ua]; NEXT;
        CASE(LDBU) ua = (uint32_t)TOP; MCHK(ua, 1); TOP = mem[ua]; NEXT;
        CASE(LDH) ua = (uint32_t)TOP; MCHK(ua, 2); TOP = (int16_t)(mem[ua] | (mem[ua + 1] << 8)); NEXT;
        CASE(LDHU) ua = (uint32_t)TOP; MCHK(ua, 2); TOP = (uint16_t)(mem[ua] | (mem[ua + 1] << 8)); NEXT;
        CASE(LDW) ua = (uint32_t)TOP; MCHK(ua, 4); TOP = (int32_t)rd32(mem + ua); NEXT;
        CASE(LDD) ua = (uint32_t)TOP; MCHK(ua, 8); if (osp >= OSTK) goto ostk_over;
            TOP = (int32_t)rd32(mem + ua); ost[osp++] = (int32_t)rd32(mem + ua + 4); NEXT;
        CASE(STB) a = POP(); ua = (uint32_t)TOP; MCHK(ua, 1); mem[ua] = (uint8_t)a; TOP = a; NEXT;
        CASE(STH) a = POP(); ua = (uint32_t)TOP; MCHK(ua, 2); mem[ua] = (uint8_t)a; mem[ua + 1] = (uint8_t)(a >> 8); TOP = a; NEXT;
        CASE(STW) a = POP(); ua = (uint32_t)TOP; MCHK(ua, 4); memcpy(mem + ua, &a, 4); TOP = a; NEXT;
        CASE(STD) ua = (uint32_t)ost[osp - 3]; MCHK(ua, 8); memcpy(mem + ua, &ost[osp - 2], 8);
            ost[osp - 3] = ost[osp - 2]; ost[osp - 2] = ost[osp - 1]; osp--; NEXT;
        CASE(STBP) a = POP(); ua = (uint32_t)POP(); MCHK(ua, 1); mem[ua] = (uint8_t)a; NEXT;
        CASE(STHP) a = POP(); ua = (uint32_t)POP(); MCHK(ua, 2); mem[ua] = (uint8_t)a; mem[ua + 1] = (uint8_t)(a >> 8); NEXT;
        CASE(STWP) a = POP(); ua = (uint32_t)POP(); MCHK(ua, 4); memcpy(mem + ua, &a, 4); NEXT;
        CASE(STDP) ua = (uint32_t)ost[osp - 3]; MCHK(ua, 8); memcpy(mem + ua, &ost[osp - 2], 8); osp -= 3; NEXT;
        CASE(ADD) b = POP(); TOP = (int32_t)((uint32_t)TOP + (uint32_t)b); NEXT;
        CASE(ADDI) a = RD32(); TOP = (int32_t)((uint32_t)TOP + (uint32_t)a); NEXT;
        CASE(SUB) b = POP(); TOP = (int32_t)((uint32_t)TOP - (uint32_t)b); NEXT;
        CASE(MUL) b = POP(); TOP = (int32_t)((uint32_t)TOP * (uint32_t)b); NEXT;
        CASE(DIV) b = POP(); if (b == 0) goto div_zero; TOP = b == -1 ? (int32_t)(0u - (uint32_t)TOP) : TOP / b; NEXT;
        CASE(MOD) b = POP(); if (b == 0) goto mod_zero; TOP = b == -1 ? 0 : TOP % b; NEXT;
        CASE(DIVU) ub = (uint32_t)POP(); if (ub == 0) goto div_zero; TOP = (int32_t)((uint32_t)TOP / ub); NEXT;
        CASE(MODU) ub = (uint32_t)POP(); if (ub == 0) goto mod_zero; TOP = (int32_t)((uint32_t)TOP % ub); NEXT;
        CASE(AND) b = POP(); TOP &= b; NEXT;
        CASE(OR) b = POP(); TOP |= b; NEXT;
        CASE(XOR) b = POP(); TOP ^= b; NEXT;
        CASE(SHL) b = POP(); TOP = (int32_t)((uint32_t)TOP << (b & 31)); NEXT;
        CASE(SHR) b = POP(); TOP = (int32_t)((uint32_t)TOP >> (b & 31)); NEXT;
        CASE(SAR) b = POP(); TOP = TOP >> (b & 31); NEXT;
        CASE(NEG) TOP = (int32_t)(0u - (uint32_t)TOP); NEXT;
        CASE(NOT) TOP = ~TOP; NEXT;
        CASE(LNOT) TOP = !TOP; NEXT;
        CASE(EQ) b = POP(); TOP = TOP == b; NEXT;
        CASE(NE) b = POP(); TOP = TOP != b; NEXT;
        CASE(LT) b = POP(); TOP = TOP < b; NEXT;
        CASE(LE) b = POP(); TOP = TOP <= b; NEXT;
        CASE(GT) b = POP(); TOP = TOP > b; NEXT;
        CASE(GE) b = POP(); TOP = TOP >= b; NEXT;
        CASE(LTU) ub = (uint32_t)POP(); TOP = (uint32_t)TOP < ub; NEXT;
        CASE(LEU) ub = (uint32_t)POP(); TOP = (uint32_t)TOP <= ub; NEXT;
        CASE(GTU) ub = (uint32_t)POP(); TOP = (uint32_t)TOP > ub; NEXT;
        CASE(GEU) ub = (uint32_t)POP(); TOP = (uint32_t)TOP >= ub; NEXT;
        CASE(FADD) b = POP(); TOP = f2i(i2f(TOP) + i2f(b)); NEXT;
        CASE(FSUB) b = POP(); TOP = f2i(i2f(TOP) - i2f(b)); NEXT;
        CASE(FMUL) b = POP(); TOP = f2i(i2f(TOP) * i2f(b)); NEXT;
        CASE(FDIV) b = POP(); TOP = f2i(i2f(TOP) / i2f(b)); NEXT;
        CASE(FNEG) TOP = (int32_t)((uint32_t)TOP ^ 0x80000000u); NEXT;
        CASE(FEQ) b = POP(); TOP = i2f(TOP) == i2f(b); NEXT;
        CASE(FNE) b = POP(); TOP = i2f(TOP) != i2f(b); NEXT;
        CASE(FLT) b = POP(); TOP = i2f(TOP) < i2f(b); NEXT;
        CASE(FLE) b = POP(); TOP = i2f(TOP) <= i2f(b); NEXT;
        CASE(FGT) b = POP(); TOP = i2f(TOP) > i2f(b); NEXT;
        CASE(FGE) b = POP(); TOP = i2f(TOP) >= i2f(b); NEXT;
        CASE(ITOF2) NXT = f2i((float)NXT); NEXT;
        CASE(UTOF2) NXT = f2i((float)(uint32_t)NXT); NEXT;
        CASE(ITOF) CASE(UTOF) CASE(FTOI) CASE(FTOU) CASE(SEXT8) CASE(ZEXT8) CASE(SEXT16) CASE(ZEXT16)
        CASE(L2I) CASE(L2B) CASE(D2B) CASE(L2D) CASE(UL2D) CASE(D2F) CASE(D2I) CASE(D2U) CASE(D2L) CASE(D2UL)
            conv_apply(ost, &osp, code[opc]); NEXT;
        CASE(I2L) CASE(U2L) CASE(I2D) CASE(U2D) CASE(F2D)
            if (osp >= OSTK) goto ostk_over;
            conv_apply(ost, &osp, code[opc]); NEXT;
        CASE(CVTU) {
            int cop = code[pc++], n = code[pc++], k;
            int32_t keep[2];
            if (n > 2 || osp >= OSTK || osp < n + 1) goto bad_op;
            for (k = 0; k < n; k++) keep[k] = ost[osp - n + k];
            osp -= n;
            if (!conv_apply(ost, &osp, cop)) goto bad_op;
            for (k = 0; k < n; k++) ost[osp++] = keep[k];
            NEXT;
        }
        CASE(LADD) POP64(lb); SETTOP64(TOP64 + lb); NEXT;
        CASE(LSUB) POP64(lb); SETTOP64(TOP64 - lb); NEXT;
        CASE(LMUL) POP64(lb); SETTOP64(TOP64 * lb); NEXT;
        CASE(LDIV) POP64(lb); if (!lb) goto div_zero; la = TOP64;
            SETTOP64(lb == ~0ull ? 0 - la : (uint64_t)((int64_t)la / (int64_t)lb)); NEXT;
        CASE(LMOD) POP64(lb); if (!lb) goto mod_zero; la = TOP64;
            SETTOP64(lb == ~0ull ? 0 : (uint64_t)((int64_t)la % (int64_t)lb)); NEXT;
        CASE(LDIVU) POP64(lb); if (!lb) goto div_zero; SETTOP64(TOP64 / lb); NEXT;
        CASE(LMODU) POP64(lb); if (!lb) goto mod_zero; SETTOP64(TOP64 % lb); NEXT;
        CASE(LAND) POP64(lb); SETTOP64(TOP64 & lb); NEXT;
        CASE(LOR) POP64(lb); SETTOP64(TOP64 | lb); NEXT;
        CASE(LXOR) POP64(lb); SETTOP64(TOP64 ^ lb); NEXT;
        CASE(LSHL) b = POP(); SETTOP64(TOP64 << (b & 63)); NEXT;
        CASE(LSHR) b = POP(); SETTOP64(TOP64 >> (b & 63)); NEXT;
        CASE(LSAR) b = POP(); SETTOP64((uint64_t)((int64_t)TOP64 >> (b & 63))); NEXT;
        CASE(LNEG) SETTOP64(0 - TOP64); NEXT;
        CASE(LCOM) SETTOP64(~TOP64); NEXT;
#define LCMP(o, expr) CASE(o) POP64(lb); la = TOP64; osp--; TOP = (expr); NEXT;
        LCMP(LEQ, la == lb)
        LCMP(LNE, la != lb)
        LCMP(LLT, (int64_t)la < (int64_t)lb)
        LCMP(LLE, (int64_t)la <= (int64_t)lb)
        LCMP(LGT, (int64_t)la > (int64_t)lb)
        LCMP(LGE, (int64_t)la >= (int64_t)lb)
        LCMP(LLTU, la < lb)
        LCMP(LLEU, la <= lb)
        LCMP(LGTU, la > lb)
        LCMP(LGEU, la >= lb)
        LCMP(DEQ, u2d(la) == u2d(lb))
        LCMP(DNE, u2d(la) != u2d(lb))
        LCMP(DLT, u2d(la) < u2d(lb))
        LCMP(DLE, u2d(la) <= u2d(lb))
        LCMP(DGT, u2d(la) > u2d(lb))
        LCMP(DGE, u2d(la) >= u2d(lb))
#undef LCMP
        CASE(DADD) POP64(lb); SETTOP64(d2u(u2d(TOP64) + u2d(lb))); NEXT;
        CASE(DSUB) POP64(lb); SETTOP64(d2u(u2d(TOP64) - u2d(lb))); NEXT;
        CASE(DMUL) POP64(lb); SETTOP64(d2u(u2d(TOP64) * u2d(lb))); NEXT;
        CASE(DDIV) POP64(lb); SETTOP64(d2u(u2d(TOP64) / u2d(lb))); NEXT;
        CASE(DNEG) TOP = (int32_t)((uint32_t)TOP ^ 0x80000000u); NEXT;
        CASE(JMP) pc = (uint32_t)RD32(); NEXT;
        CASE(JZ) a = RD32(); if (POP() == 0) pc = (uint32_t)a; NEXT;
        CASE(JNZ) a = RD32(); if (POP() != 0) pc = (uint32_t)a; NEXT;
#define JCMP(o, expr) CASE(o) a = RD32(); b = POP(); ua = (uint32_t)POP(); if (expr) pc = (uint32_t)a; NEXT;
        JCMP(JEQ, (int32_t)ua == b)
        JCMP(JNE, (int32_t)ua != b)
        JCMP(JLT, (int32_t)ua < b)
        JCMP(JLE, (int32_t)ua <= b)
        JCMP(JGT, (int32_t)ua > b)
        JCMP(JGE, (int32_t)ua >= b)
#undef JCMP
        CASE(CALL) a = RD32(); lr = pc; pc = (uint32_t)a; NEXT;
        CASE(CALLI) {
            int n = code[pc++], i;
            ua = (uint32_t)ost[osp - n - 1];
            for (i = osp - n - 1; i < osp - 1; i++) ost[i] = ost[i + 1];
            osp--;
            if (ua >= csize || ua == 0) {
                SYNC();
                vm_fail(vm, ua == 0 ? "call through a null function pointer" : "call through an invalid function pointer");
            }
            lr = pc;
            pc = ua;
            NEXT;
        }
        CASE(ENTER) CASE(ENTERV) {
            int np = code[pc++], nv = 0, total, i;
            uint32_t size = (uint32_t)RD32(), r;
            if (code[opc] == OP_ENTERV) {
                nv = POP();
                if (nv < 0 || nv > osp) goto bad_op;
            }
            total = np + nv;
            if (total > osp) { SYNC(); vm_fail(vm, "internal: missing arguments"); }
            if (size > memlim || sp < vm->stack_limit + size + REC + (uint32_t)nv * 4u) {
                SYNC();
                vm_fail(vm, "stack overflow (recursion too deep?)");
            }
            r = sp - REC;
            memcpy(mem + r, &lr, 4);
            memcpy(mem + r + 4, &fp, 4);
            memcpy(mem + r + 8, &rec, 4);
            memcpy(mem + r + 12, &sp, 4);
            memcpy(mem + r + 16, &nv, 4);
            rec = r;
            /* variadic cells sit just below the record */
            if (nv) memcpy(mem + r - (uint32_t)nv * 4u, &ost[osp - nv], (size_t)nv * 4);
            sp = r - (uint32_t)nv * 4u - size;
            fp = sp;
            if (memlim - sp > vm_last.stack_peak) vm_last.stack_peak = memlim - sp;
            for (i = 0; i < np; i++) memcpy(mem + fp + (uint32_t)(i * 4), &ost[osp - total + i], 4);
            osp -= total;
            if (size > (uint32_t)np * 4) memset(mem + fp + (uint32_t)np * 4, 0, size - (uint32_t)np * 4);
            if (osp > OSTK - 64) goto ostk_over;
            NEXT;
        }
        CASE(VASTART) {
            int32_t nv;
            memcpy(&nv, mem + rec + 16, 4);
            PUSH(rec - (uint32_t)nv * 4u);
            NEXT;
        }
        CASE(RET) PUSH(0);
        /* fall through */
        CASE(RETV)
            pc = rd32(mem + rec);
            fp = rd32(mem + rec + 4);
            sp = rd32(mem + rec + 12);
            rec = rd32(mem + rec + 8);
            if (pc == LR_NATIVE) { SYNC(); return; }
            NEXT;
        CASE(POP) osp--; NEXT;
        CASE(DUP) a = TOP; PUSH(a); NEXT;
        CASE(SWAP) a = TOP; TOP = NXT; NXT = a; NEXT;
        CASE(MEMCPY) {
            uint32_t n = (uint32_t)RD32();
            uint32_t src = (uint32_t)POP();
            uint32_t dst = (uint32_t)TOP;
            MCHK(src, n);
            MCHK(dst, n);
            memmove(mem + dst, mem + src, n);
            NEXT;
        }
        CASE(MEMSET) {
            uint32_t n = (uint32_t)RD32();
            uint32_t dst = (uint32_t)POP();
            MCHK(dst, n);
            memset(mem + dst, 0, n);
            NEXT;
        }
        CASE(SYS) {
            int id = (uint16_t)RD16();
            int n = code[pc++];
            int64_t r;
            if (n > osp) { SYNC(); vm_fail(vm, "internal: missing arguments"); }
            osp -= n;
            SYNC();
            r = vm_sys(vm, id & 0x7FFF, ost + osp, n);
            LOAD();
            if (id & 0x8000) PUSH64((uint64_t)r);
            else PUSH((int32_t)r);
            NEXT;
        }
        CASE(INC) {
            int mode = code[pc++];
            int32_t d = RD32();
            int32_t old, nw;
            ua = (uint32_t)TOP;
            switch (mode & 7) {
            case INC_B: MCHK(ua, 1); old = (int8_t)mem[ua]; nw = (int8_t)(old + d); mem[ua] = (uint8_t)nw; break;
            case INC_BU: MCHK(ua, 1); old = mem[ua]; nw = (uint8_t)(old + d); mem[ua] = (uint8_t)nw; break;
            case INC_H: MCHK(ua, 2); old = (int16_t)(mem[ua] | (mem[ua + 1] << 8)); nw = (int16_t)(old + d);
                mem[ua] = (uint8_t)nw; mem[ua + 1] = (uint8_t)(nw >> 8); break;
            case INC_HU: MCHK(ua, 2); old = (uint16_t)(mem[ua] | (mem[ua + 1] << 8)); nw = (uint16_t)(old + d);
                mem[ua] = (uint8_t)nw; mem[ua + 1] = (uint8_t)(nw >> 8); break;
            case INC_W: MCHK(ua, 4); old = (int32_t)rd32(mem + ua); nw = (int32_t)((uint32_t)old + (uint32_t)d);
                memcpy(mem + ua, &nw, 4); break;
            case INC_F: MCHK(ua, 4); old = (int32_t)rd32(mem + ua); nw = f2i(i2f(old) + (float)d);
                memcpy(mem + ua, &nw, 4); break;
            default: {
                /* 64-bit: integer or double */
                uint64_t o64, n64;
                MCHK(ua, 8);
                memcpy(&o64, mem + ua, 8);
                n64 = (mode & 7) == INC_L ? o64 + (uint64_t)(int64_t)d : d2u(u2d(o64) + (double)d);
                memcpy(mem + ua, &n64, 8);
                if (osp >= OSTK) goto ostk_over;
                la = (mode & INC_POST) ? o64 : n64;
                ost[osp - 1] = (int32_t)(uint32_t)la;
                ost[osp++] = (int32_t)(uint32_t)(la >> 32);
                NEXT;
            }
            }
            TOP = (mode & INC_POST) ? old : nw;
            NEXT;
        }
op_bad:
bad_op:
        SYNC();
        vm_fail(vm, "invalid instruction");
bad_pc:
        SYNC();
        vm_fail(vm, "jump outside the program");
div_zero:
        SYNC();
        vm_fail(vm, "division by zero");
mod_zero:
        SYNC();
        vm_fail(vm, "division by zero (modulo)");
ostk_over:
        SYNC();
        vm_fail(vm, "expression stack overflow (recursion too deep?)");
housekeeping:
        budget = 4096;
        vm->icount += 4096;
        if (hal_millis() - vm->last_poll >= 50) {
            vm->last_poll = hal_millis();
            SYNC();
            if (con_poll_break()) vm_fail(vm, "break");
            con_flush();
        }
        opc = pc;
        if (pc >= csize) goto bad_pc;
        goto *tab[code[pc++]];
    }
}
#undef REC

/* ------------------------------------------------------------------ */
/* loading                                                             */
/* ------------------------------------------------------------------ */

int vm_run_image(const uint8_t *img, int len, const char *name, int argc, char **argv)
{
    VM *vm;
    uint32_t code_size, data_init, data_total, entry, nlines, nfuncs, fsize, nfiles, lsize;
    uint32_t off, want, stack;
    int i, rc;
    if (len < 48 || rd32(img) != CBC_MAGIC) { con_printf("%s: not a CalcIDE program (.cbc)\n", name); return -1; }
    if (rd32(img + 4) != CBC_VERSION) { con_printf("%s: compiled for another version, recompile it\n", name); return -1; }
    code_size = rd32(img + 8);
    data_init = rd32(img + 12);
    data_total = rd32(img + 16);
    entry = rd32(img + 20);
    nlines = rd32(img + 24);
    nfuncs = rd32(img + 28);
    fsize = rd32(img + 32);
    nfiles = rd32(img + 36);
    lsize = rd32(img + 40);
    off = 48;
    if (code_size > (uint32_t)len || data_init > (uint32_t)len || off + code_size + data_init + nlines * 8 + fsize + lsize > (uint32_t)len
        || entry >= code_size || data_init > data_total) {
        con_printf("%s: damaged program file\n", name);
        return -1;
    }
    (void)nfuncs;

    vm = (VM *)calloc(1, sizeof(VM));
    if (!vm) { con_printf("run: out of memory\n"); return -1; }
    vm->name = name;
    vm->code_size = code_size;
    /* code runs in place: from the loaded file, or straight from flash */
    vm->code = img + off;
    off += code_size;

    /* memory: data + heap + stack, as much as is configured and available */
    stack = (uint32_t)g_conf.vm_stack_kb * 1024u;
    if (stack < 4096) stack = 4096;
    {
        /* vm_mem_kb = 0: everything free, less a reserve for the file system */
        size_t largest = hal_ram_largest();
        want = g_conf.vm_mem_kb ? (uint32_t)g_conf.vm_mem_kb * 1024u
                                : (largest > 24 * 1024 ? (uint32_t)(largest - 16 * 1024) : 0);
        if (want < CBC_DATA_BASE + data_total + stack + 4096) want = CBC_DATA_BASE + data_total + stack + 4096;
        if (largest > 24 * 1024 && want > largest - 16 * 1024) want = (uint32_t)(largest - 16 * 1024);
    }
    want &= ~7u;
    vm->mem = (uint8_t *)malloc(want);
    while (!vm->mem && want > CBC_DATA_BASE + data_total + stack + 4096) {
        want = (want * 3 / 4) & ~7u;
        vm->mem = (uint8_t *)malloc(want);
    }
    if (!vm->mem || want < CBC_DATA_BASE + data_total + stack + 1024) {
        free(vm->mem);
        free(vm);
        con_printf("run: not enough memory (program needs %u KB)\n", (unsigned)((data_total + stack) / 1024 + 1));
        return -1;
    }
    vm->mem_size = want;
    memset(vm->mem, 0, want);
    memcpy(vm->mem + CBC_DATA_BASE, img + off, data_init);
    off += data_init;
    vm->data_end = CBC_DATA_BASE + data_total;
    vm->lines = img + off;
    vm->nlines = nlines;
    off += nlines * 8;
    vm->funcs = img + off;
    vm->funcs_size = fsize;
    off += fsize;
    vm->fnames = (const char *)img + off;
    vm->nfiles = nfiles;

    vm->stack_limit = want - stack;
    vm->sp = want;
    vm->heap_start = vm->data_end;

    /* argv lives at the start of the heap area */
    {
        uint32_t p = (vm->data_end + 3) & ~3u;
        uint32_t av = p;
        uint32_t need = (uint32_t)(argc + 1) * 4;
        for (i = 0; i < argc; i++) need += (uint32_t)strlen(argv[i]) + 1;
        if (p + need + 64 >= vm->stack_limit) { argc = 0; need = 4; }
        p += (uint32_t)(argc + 1) * 4;
        for (i = 0; i < argc; i++) {
            uint32_t l = (uint32_t)strlen(argv[i]) + 1;
            memcpy(vm->mem + p, argv[i], l);
            SW(vm, av + (uint32_t)i * 4, p);
            p += l;
        }
        SW(vm, av + (uint32_t)argc * 4, 0);
        vm->heap_start = (p + 7) & ~7u;
        vm->ost[0] = argc;
        vm->ost[1] = (int32_t)av;
        vm->osp = 2;
    }
    heap_init(vm);
    vm->pc = entry;
    vm->rand_state = 1;
    vm->t0 = hal_millis();
    vm->last_poll = vm->t0;
    memset(&vm_last, 0, sizeof vm_last);
    vm_last.mem_size = want;

    g_vm = vm;
    rc = setjmp(vm->jb);
    if (rc == 0) {
        vm_exec(vm);
        vm->exit_code = vm->osp > 0 ? vm->ost[vm->osp - 1] : 0;
    } else if (rc == 2) {
        report_error(vm);
    }
    vm_last.instructions = vm->icount;
    vm_last.ms = hal_millis() - vm->t0;
    hal_tone_off();
    close_all(vm);
    if (vm->uarts_open & 1) hal_uart_close(0);
    if (vm->uarts_open & 2) hal_uart_close(1);
    con_flush();
    rc = vm->exit_code;
    g_vm = 0;
    free(vm->mem);
    free(vm);
    return rc;
}

int vm_run_file(const char *path, int argc, char **argv)
{
    int len, rc;
    uint8_t *img;
    if (strncmp(path, "pf:", 3) == 0) {
        int kind;
        const void *p = pf_find(path + 3, &len, &kind);
        if (!p) { con_printf("run: '%s' is not in the page file\n", path + 3); return -1; }
        return vm_run_image((const uint8_t *)p, len, path, argc, argv);
    }
    img = (uint8_t *)vfs_load(path, &len, 0);
    if (!img) { con_printf("run: cannot open %s\n", path); return -1; }
    rc = vm_run_image(img, len, path, argc, argv);
    free(img);
    return rc;
}
