/* cc.c - the CalcIDE Tiny-C compiler.
 *
 * A single-pass compiler from a C dialect to CalcIDE bytecode (.cbc), run
 * by the VM in src/vm.c. It is written in the same dialect it accepts, so it
 * compiles itself:
 *
 *     cc /calcide/cc/cc.c -o /cc.cbc      (native compiler builds the VM one)
 *     run /cc.cbc hello.c -o hello.cbc    (VM compiler builds a program)
 *
 * There is no syntax tree. Expressions emit stack-machine code as they are
 * parsed; an lvalue leaves its address on the stack and the load is only
 * emitted once the value is actually needed (see rv()). Constant expressions
 * fold as they are emitted, which is also how array sizes, case labels,
 * enum values and global initializers are evaluated.
 *
 * Compiled natively, ccport.h maps the library calls used here onto the
 * firmware's console, file system and an arena allocator.
 */
#ifndef __CALCIDE__
#include "ccport.h"
#endif
#include "ops.h"
#include "prelude.h"

#define CC_VERSION "0.1"

/* ------------------------------------------------------------------ */
/* tokens                                                              */
/* ------------------------------------------------------------------ */

#define TK_EOF     0
#define TK_NUM     128
#define TK_FNUM    129
#define TK_STR     130
#define TK_ID      131
#define TK_NL      132   /* only while reading a directive line */
#define TK_EOA     133   /* end of a macro argument being expanded */
#define TK_EQ      134
#define TK_NE      135
#define TK_LE      136
#define TK_GE      137
#define TK_SHL     138
#define TK_SHR     139
#define TK_ANDAND  140
#define TK_OROR    141
#define TK_INC     142
#define TK_DEC     143
#define TK_ARROW   144
#define TK_ADDEQ   145
#define TK_SUBEQ   146
#define TK_MULEQ   147
#define TK_DIVEQ   148
#define TK_MODEQ   149
#define TK_ANDEQ   150
#define TK_OREQ    151
#define TK_XOREQ   152
#define TK_SHLEQ   153
#define TK_SHREQ   154
#define TK_ELLIPSIS 155
/* keywords */
#define KW_INT      160
#define KW_CHAR     161
#define KW_SHORT    162
#define KW_LONG     163
#define KW_SIGNED   164
#define KW_UNSIGNED 165
#define KW_VOID     166
#define KW_FLOAT    167
#define KW_DOUBLE   168
#define KW_STRUCT   169
#define KW_UNION    170
#define KW_ENUM     171
#define KW_TYPEDEF  172
#define KW_STATIC   173
#define KW_EXTERN   174
#define KW_CONST    175
#define KW_VOLATILE 176
#define KW_AUTO     177
#define KW_REGISTER 178
#define KW_IF       179
#define KW_ELSE     180
#define KW_WHILE    181
#define KW_DO       182
#define KW_FOR      183
#define KW_SWITCH   184
#define KW_CASE     185
#define KW_DEFAULT  186
#define KW_BREAK    187
#define KW_CONTINUE 188
#define KW_RETURN   189
#define KW_GOTO     190
#define KW_SIZEOF   191
#define KW_INLINE   192
#define KW_BOOL     193

/* types */
#define T_VOID   0
#define T_CHAR   1
#define T_SHORT  2
#define T_INT    3
#define T_FLOAT  4
#define T_PTR    5
#define T_ARRAY  6
#define T_FUNC   7
#define T_STRUCT 8
#define T_HOLE   9

/* symbol kinds */
#define S_GLOBAL  1
#define S_LOCAL   2
#define S_FUNC    3
#define S_TYPEDEF 4
#define S_ENUM    5

/* segments for relocation */
#define SEG_STR  1
#define SEG_DATA 2
#define SEG_BSS  3

/* sources */
#define SRC_FILE 1
#define SRC_MEM  2
#define SRC_TOK  3

typedef struct Type Type;
typedef struct Member Member;
typedef struct Param Param;
typedef struct Sym Sym;
typedef struct Ident Ident;
typedef struct Macro Macro;
typedef struct Tok Tok;
typedef struct Src Src;
typedef struct Tag Tag;
typedef struct Case Case;
typedef struct Label Label;

struct Tok {
    int kind;
    int val;
    char *str;
    int len;
    Ident *id;
    int uns;
};

struct Type {
    int kind;
    int uns;
    int size;
    int align;
    Type *base;
    int len;          /* array length, -1 if unknown */
    Member *mem;      /* struct/union members */
    int isunion;
    int complete;
    Param *params;
    int nparams;
    int variadic;
    int proto;        /* has a parameter list */
    Type *ptrto;      /* cached pointer-to type */
};

struct Member {
    Ident *name;
    Type *type;
    int off;
    Member *next;
};

struct Param {
    Ident *name;
    Type *type;
    Param *next;
};

struct Sym {
    Ident *name;
    int kind;
    Type *type;
    int addr;         /* global: segment offset; local: frame offset; enum: value */
    int seg;          /* SEG_DATA / SEG_BSS for globals */
    int sys;          /* builtin number, 0 if none */
    int defined;
    int patch;        /* forward-reference chain through code operands */
    int depth;        /* scope depth */
    Sym *shadow;      /* outer binding of the same name */
    Sym *scnext;      /* next symbol in the same scope */
};

struct Tag {
    Ident *name;
    Type *type;
    int depth;
    Tag *shadow;
    Tag *scnext;
};

struct Ident {
    char *name;
    int len;
    int kw;
    Sym *sym;
    Tag *tag;
    Macro *macro;
    Ident *next;
};

struct Macro {
    int fn;           /* function-like */
    int nparams;
    Ident *params[16];
    Tok *body;
    int nbody;
    int busy;
};

struct Src {
    int kind;
    FILE *fp;
    char *buf;
    int pos;
    int len;
    int cap;
    int eof;
    char *name;
    int line;
    int fidx;
    int bol;
    Tok *toks;
    int ntok;
    int tpos;
    Macro *mac;
    int barrier;
    int owned;        /* toks was malloc'd and is freed on pop */
    int conddepth;
    Src *prev;
};

struct Case {
    int val;
    int pc;
    Case *next;
};

struct Label {
    Ident *name;
    int pc;           /* -1 until defined */
    int patch;
    Label *next;
};

/* ------------------------------------------------------------------ */
/* globals                                                             */
/* ------------------------------------------------------------------ */

char *arena_p;
int arena_left;
int arena_total;

Ident *idtab[512];

Src *src;
Src *src_free;        /* recycled token sources */
Sym *sym_free;        /* recycled block-scope symbols */
int stop_nl;          /* return TK_NL at end of line (directives) */
int in_if;            /* evaluating #if: 'defined' is an operator */
Tok tok;              /* current token */
Tok pushed[4];
int npushed;

/* conditional compilation */
int cond_active[32];
int cond_done[32];
int cond_else[32];
int conddepth;

char *fnames[64];
int nfiles;

/* output */
char *code;
int cpos;
int ccap;
char *strseg;
int spos;
int scap;
char *dataseg;
int dpos;
int dcap;
int bss;
int *relocs;          /* code position * 4 + segment */
int nrelocs;
int rcap;
int *lines;           /* pairs: pc, line | file << 24 */
int nlines;
int lcap;
int last_line;
int last_lfile;
char *ftab;           /* function table: addr(4) len(1) name */
int fpos;
int fcap;
int nfuncs;

/* constant folding history */
int hist_pos[8];
int nhist;
int nocode_depth;     /* >0 while code is going to be thrown away */

/* expression state */
int last_was_lea = -1; /* position of a LEA that a load may fuse with */
int lv;               /* address of an lvalue is on the stack */
Sym *pending_fn;      /* a function designator not yet materialised */

/* function state */
int depth;            /* scope depth: 0 is file scope */
Sym *scope_syms[64];
Tag *scope_tags[64];
int frame_size;
int frame_max;
Type *cur_ret;
Sym *cur_fn;
Label *labels;
int brk_chain;
int cont_chain;
int cont_target;      /* -1: use cont_chain */
int in_loop;
int in_switch;
Case *cases;
int default_pc;
int ret_tmp;          /* frame slot for struct return copies */

/* init chunk chain */
int init_tail;        /* code position of the operand of the last chain JMP */
int in_chunk;

/* builtin types */
Type *ty_void;
Type *ty_char;
Type *ty_uchar;
Type *ty_short;
Type *ty_ushort;
Type *ty_int;
Type *ty_uint;
Type *ty_float;
Type *ty_charp;

Ident *id_sys;
Ident *id_main;
Ident *id_defined;
Ident *id_line;

int errors;
int warnings;
int verbose;

/* ------------------------------------------------------------------ */
/* utilities                                                           */
/* ------------------------------------------------------------------ */

char *cur_name(void)
{
    Src *s = src;
    while (s && s->kind == SRC_TOK) s = s->prev;
    if (s) return s->name;
    return "?";
}

int cur_line(void)
{
    Src *s = src;
    while (s && s->kind == SRC_TOK) s = s->prev;
    if (s) return s->line;
    return 0;
}

void error(char *msg)
{
    printf("%s:%d: error: %s\n", cur_name(), cur_line(), msg);
    exit(1);
}

void error_s(char *msg, char *arg)
{
    printf("%s:%d: error: %s '%s'\n", cur_name(), cur_line(), msg, arg);
    exit(1);
}

void warn_s(char *msg, char *arg)
{
    printf("%s:%d: warning: %s '%s'\n", cur_name(), cur_line(), msg, arg);
    warnings++;
}

char *alloc(int n)
{
    char *p;
    int sz;
    n = (n + 3) & ~3;
    if (n > arena_left) {
        sz = 16384;
        if (n > sz) sz = n;
        arena_p = malloc(sz);
        if (!arena_p) error("out of memory");
        memset(arena_p, 0, sz);
        arena_left = sz;
        arena_total = arena_total + sz;
    }
    p = arena_p;
    arena_p = arena_p + n;
    arena_left = arena_left - n;
    return p;
}

char *grow(char *buf, int *cap, int need)
{
    char *nb;
    int nc = *cap;
    if (need <= nc) return buf;
    if (nc < 16) nc = 16;
    while (nc < need) nc = nc * 2;
    nb = malloc(nc);
    if (!nb) error("out of memory");
    if (buf) {
        memcpy(nb, buf, *cap);
        free(buf);
    }
    *cap = nc;
    return nb;
}

int *grow_ints(int *buf, int *cap, int need)
{
    int *nb;
    int nc = *cap;
    if (need <= nc) return buf;
    while (nc < need) nc = nc * 2;
    nb = (int *)malloc(nc * 4);
    if (!nb) error("out of memory");
    memcpy(nb, buf, *cap * 4);
    free(buf);
    *cap = nc;
    return nb;
}

char *str_dup(char *s, int n)
{
    char *p = alloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ------------------------------------------------------------------ */
/* identifiers                                                         */
/* ------------------------------------------------------------------ */

Ident *intern(char *s, int n)
{
    unsigned h = 0;
    int i;
    Ident *id;
    for (i = 0; i < n; i++) h = h * 31 + (s[i] & 255);
    h = h & 511;
    for (id = idtab[h]; id; id = id->next)
        if (id->len == n && memcmp(id->name, s, n) == 0) return id;
    id = (Ident *)alloc(sizeof(Ident));
    id->name = str_dup(s, n);
    id->len = n;
    id->next = idtab[h];
    idtab[h] = id;
    return id;
}

void keyword(char *s, int kw)
{
    intern(s, strlen(s))->kw = kw;
}

/* ------------------------------------------------------------------ */
/* sources                                                             */
/* ------------------------------------------------------------------ */

void push_src(Src *s)
{
    s->prev = src;
    s->conddepth = conddepth;
    src = s;
}

int add_fname(char *name)
{
    if (nfiles < 64) {
        fnames[nfiles] = name;
        nfiles++;
    }
    return nfiles - 1;
}

int open_file(char *path)
{
    FILE *fp = fopen(path, "rb");
    Src *s;
    if (!fp) return 0;
    s = (Src *)alloc(sizeof(Src));
    s->kind = SRC_FILE;
    s->fp = fp;
    s->cap = 1024;
    s->buf = alloc(s->cap);
    s->name = str_dup(path, strlen(path));
    s->line = 1;
    s->bol = 1;
    s->fidx = add_fname(s->name);
    push_src(s);
    return 1;
}

void open_mem(char *text, char *name)
{
    Src *s = (Src *)alloc(sizeof(Src));
    s->kind = SRC_MEM;
    s->buf = text;
    s->len = strlen(text);
    s->eof = 1;
    s->name = name;
    s->line = 1;
    s->bol = 1;
    s->fidx = add_fname(name);
    push_src(s);
}

void push_toks(Tok *t, int n, Macro *m, int barrier)
{
    Src *s = src_free;
    if (s) {
        src_free = s->prev;
        memset(s, 0, sizeof(Src));
    } else s = (Src *)alloc(sizeof(Src));
    s->kind = SRC_TOK;
    s->toks = t;
    s->ntok = n;
    s->mac = m;
    s->barrier = barrier;
    if (m) m->busy = 1;
    push_src(s);
}

void pop_src(void)
{
    Src *s = src;
    if (s->kind == SRC_FILE) {
        fclose(s->fp);
        if (conddepth != s->conddepth) error("unterminated #if");
    }
    if (s->mac) s->mac->busy = 0;
    src = s->prev;
    if (s->kind == SRC_TOK) {
        if (s->owned) free(s->toks);
        s->prev = src_free;
        src_free = s;
    }
}

void refill(Src *s)
{
    int keep = s->len - s->pos;
    int n;
    if (keep > 0 && s->pos > 0) memmove(s->buf, s->buf + s->pos, keep);
    if (keep < 0) keep = 0;
    s->len = keep;
    s->pos = 0;
    n = fread(s->buf + keep, 1, s->cap - keep, s->fp);
    if (n <= 0) s->eof = 1;
    else s->len = keep + n;
}

/* character k positions ahead in the current character source */
int sc(int k)
{
    Src *s = src;
    if (s->pos + k >= s->len && s->kind == SRC_FILE && !s->eof) refill(s);
    if (s->pos + k >= s->len) return -1;
    return s->buf[s->pos + k] & 255;
}

void adv(void)
{
    if (src->buf[src->pos] == '\n') {
        src->line++;
        src->bol = 1;
    }
    src->pos++;
}

/* ------------------------------------------------------------------ */
/* the character-level lexer                                           */
/* ------------------------------------------------------------------ */

void do_directive(void);
void skip_block(void);
int cond_skipping(void);

int is_id1(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

int is_dig(int c)
{
    return c >= '0' && c <= '9';
}

int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int lex_escape(void)
{
    int c = sc(0);
    int v;
    int n;
    adv();
    if (c == 'n') return 10;
    if (c == 't') return 9;
    if (c == 'r') return 13;
    if (c == '0' && !is_dig(sc(0))) return 0;
    if (c == 'a') return 7;
    if (c == 'b') return 8;
    if (c == 'f') return 12;
    if (c == 'v') return 11;
    if (c == 'e') return 27;
    if (c == 'x') {
        v = 0;
        while (hexval(sc(0)) >= 0) { v = v * 16 + hexval(sc(0)); adv(); }
        return v & 255;
    }
    if (c >= '0' && c <= '7') {
        v = c - '0';
        n = 1;
        while (n < 3 && sc(0) >= '0' && sc(0) <= '7') { v = v * 8 + sc(0) - '0'; adv(); n++; }
        return v & 255;
    }
    return c;
}

char numbuf[64];

void lex_number(void)
{
    unsigned v = 0;
    int c;
    int isf = 0;
    int n = 0;
    float f;
    tok.uns = 0;
    if (sc(0) == '0' && (sc(1) == 'x' || sc(1) == 'X')) {
        adv(); adv();
        while (hexval(sc(0)) >= 0) { v = v * 16 + hexval(sc(0)); adv(); }
    } else {
        /* collect the spelling, then decide integer or float */
        while (1) {
            c = sc(0);
            if (is_dig(c) || c == '.') {
                if (c == '.') isf = 1;
            } else if ((c == 'e' || c == 'E') && (is_dig(sc(1)) || ((sc(1) == '-' || sc(1) == '+') && is_dig(sc(2))))) {
                isf = 1;
                if (n < 62) { numbuf[n] = c; n++; }
                adv();
                c = sc(0);
            } else break;
            if (n < 62) { numbuf[n] = c; n++; }
            adv();
        }
        numbuf[n] = 0;
        if (sc(0) == 'f' || sc(0) == 'F') { isf = 1; adv(); }
        if (isf) {
            f = atof(numbuf);
            memcpy(&tok.val, &f, 4);
            tok.kind = TK_FNUM;
            return;
        }
        if (numbuf[0] == '0') {
            for (n = 1; numbuf[n]; n++) v = v * 8 + numbuf[n] - '0';
        } else {
            for (n = 0; numbuf[n]; n++) v = v * 10 + numbuf[n] - '0';
        }
    }
    while (sc(0) == 'u' || sc(0) == 'U' || sc(0) == 'l' || sc(0) == 'L') {
        if (sc(0) == 'u' || sc(0) == 'U') tok.uns = 1;
        adv();
    }
    tok.kind = TK_NUM;
    tok.val = v;
}

char *sbuf;
int scap2;

void lex_string(void)
{
    int n = 0;
    int c;
    adv();
    while (1) {
        c = sc(0);
        if (c == '"') { adv(); break; }
        if (c < 0 || c == '\n') error("unterminated string");
        if (c == '\\') { adv(); c = lex_escape(); }
        else adv();
        if (n + 2 > scap2) sbuf = grow(sbuf, &scap2, n + 2);
        sbuf[n] = c;
        n++;
    }
    tok.kind = TK_STR;
    tok.str = str_dup(sbuf, n);
    tok.len = n;
}

int two(int a, int b)
{
    return sc(0) == a && sc(1) == b;
}

/* one token from a character source; 0 on end of that source */
int lex_chars(void)
{
    int c;
    int start;
    int d;
    while (1) {
        c = sc(0);
        if (c < 0) return 0;
        if (c == '\n') {
            adv();
            if (stop_nl) { tok.kind = TK_NL; return 1; }
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == 12 || c == 11) { adv(); continue; }
        if (c == '\\' && (sc(1) == '\n' || sc(1) == '\r')) {
            adv();
            if (sc(0) == '\r') adv();
            adv();
            src->bol = 0;
            continue;
        }
        if (two('/', '/') || two('#', '#')) {
            while (sc(0) >= 0 && sc(0) != '\n') adv();
            continue;
        }
        if (two('/', '*')) {
            adv(); adv();
            while (sc(0) >= 0 && !two('*', '/')) adv();
            if (sc(0) < 0) error("unterminated comment");
            adv(); adv();
            continue;
        }
        if (c == '#' && src->bol && !stop_nl) {
            adv();
            src->bol = 0;
            do_directive();
            if (cond_skipping()) skip_block();
            continue;
        }
        src->bol = 0;
        if (is_id1(c)) {
            start = src->pos;
            /* identifiers never straddle a refill: make sure it is buffered */
            d = 0;
            while (is_id1(sc(d)) || is_dig(sc(d))) d++;
            tok.id = intern(src->buf + src->pos, d);
            while (d > 0) { adv(); d--; }
            tok.kind = TK_ID;
            return 1;
        }
        if (is_dig(c) || (c == '.' && is_dig(sc(1)))) { lex_number(); return 1; }
        if (c == '"') { lex_string(); return 1; }
        if (c == '\'') {
            adv();
            c = sc(0);
            if (c == '\\') { adv(); c = lex_escape(); }
            else adv();
            if (sc(0) != '\'') error("bad character constant");
            adv();
            tok.kind = TK_NUM;
            tok.val = c;
            if (c > 127) tok.val = c - 256;
            tok.uns = 0;
            return 1;
        }
        adv();
        d = sc(0);
        tok.kind = c;
        if (c == '=' && d == '=') { adv(); tok.kind = TK_EQ; }
        else if (c == '!' && d == '=') { adv(); tok.kind = TK_NE; }
        else if (c == '<' && d == '=') { adv(); tok.kind = TK_LE; }
        else if (c == '>' && d == '=') { adv(); tok.kind = TK_GE; }
        else if (c == '<' && d == '<') {
            adv(); tok.kind = TK_SHL;
            if (sc(0) == '=') { adv(); tok.kind = TK_SHLEQ; }
        }
        else if (c == '>' && d == '>') {
            adv(); tok.kind = TK_SHR;
            if (sc(0) == '=') { adv(); tok.kind = TK_SHREQ; }
        }
        else if (c == '&' && d == '&') { adv(); tok.kind = TK_ANDAND; }
        else if (c == '|' && d == '|') { adv(); tok.kind = TK_OROR; }
        else if (c == '+' && d == '+') { adv(); tok.kind = TK_INC; }
        else if (c == '-' && d == '-') { adv(); tok.kind = TK_DEC; }
        else if (c == '-' && d == '>') { adv(); tok.kind = TK_ARROW; }
        else if (c == '+' && d == '=') { adv(); tok.kind = TK_ADDEQ; }
        else if (c == '-' && d == '=') { adv(); tok.kind = TK_SUBEQ; }
        else if (c == '*' && d == '=') { adv(); tok.kind = TK_MULEQ; }
        else if (c == '/' && d == '=') { adv(); tok.kind = TK_DIVEQ; }
        else if (c == '%' && d == '=') { adv(); tok.kind = TK_MODEQ; }
        else if (c == '&' && d == '=') { adv(); tok.kind = TK_ANDEQ; }
        else if (c == '|' && d == '=') { adv(); tok.kind = TK_OREQ; }
        else if (c == '^' && d == '=') { adv(); tok.kind = TK_XOREQ; }
        else if (c == '.' && d == '.' && sc(1) == '.') { adv(); adv(); tok.kind = TK_ELLIPSIS; }
        return 1;
    }
    return 0;
}

/* next token without macro expansion */
void lex_raw(void)
{
    if (npushed > 0) {
        npushed--;
        tok = pushed[npushed];
        return;
    }
    while (1) {
        if (!src) { tok.kind = TK_EOF; return; }
        if (src->kind == SRC_TOK) {
            if (src->tpos < src->ntok) {
                tok = src->toks[src->tpos];
                src->tpos++;
                return;
            }
            if (src->barrier) { tok.kind = TK_EOA; return; }
            pop_src();
            continue;
        }
        if (lex_chars()) return;
        if (stop_nl) { tok.kind = TK_NL; return; }
        if (!src->prev) { tok.kind = TK_EOF; return; }
        pop_src();
    }
}

void unread(Tok *t)
{
    if (npushed >= 4) error("internal: token pushback");
    pushed[npushed] = *t;
    npushed++;
}

/* ------------------------------------------------------------------ */
/* macros                                                              */
/* ------------------------------------------------------------------ */

/* token buffers that are thrown away again: malloc'd, not from the arena */
Tok *mbuf_grow(Tok *t, int *cap, int need)
{
    Tok *nt;
    int nc = *cap;
    if (need <= nc) return t;
    if (nc < 8) nc = 8;
    while (nc < need) nc = nc * 2;
    nt = (Tok *)malloc(nc * sizeof(Tok));
    if (!nt) error("out of memory");
    if (t) {
        memcpy(nt, t, *cap * sizeof(Tok));
        free(t);
    }
    *cap = nc;
    return nt;
}

Tok *tbuf_grow(Tok *t, int *cap, int need)
{
    Tok *nt;
    int nc = *cap;
    if (need <= nc) return t;
    if (nc < 8) nc = 8;
    while (nc < need) nc = nc * 2;
    nt = (Tok *)alloc(nc * sizeof(Tok));
    if (t) memcpy(nt, t, *cap * sizeof(Tok));
    *cap = nc;
    return nt;
}

void next(void);

/* fully macro-expand a list of tokens */
Tok *expand_list(Tok *in, int n, int *outn)
{
    Tok *out = 0;
    int cap = 0;
    int cnt = 0;
    Tok save = tok;
    int savepushed = npushed;
    Tok savep[4];
    memcpy(savep, pushed, sizeof(savep));
    npushed = 0;
    push_toks(in, n, 0, 1);
    while (1) {
        next();
        if (tok.kind == TK_EOA) break;
        out = mbuf_grow(out, &cap, cnt + 1);
        out[cnt] = tok;
        cnt++;
    }
    pop_src();
    memcpy(pushed, savep, sizeof(savep));
    npushed = savepushed;
    tok = save;
    *outn = cnt;
    return out;
}

/* returns 1 if the macro was expanded (its tokens are now the source) */
int expand_macro(Ident *id)
{
    Macro *m = id->macro;
    Tok *args[16];
    int nargs[16];
    int acap[16];
    int na = 0;
    int lvl = 0;
    int i;
    int j;
    int k;
    Tok *out = 0;
    int cap = 0;
    int cnt = 0;
    Tok idtok = tok;
    Tok t;
    Tok *ex;
    int nex;
    if (m->busy) return 0;
    if (!m->fn) {
        if (m->nbody) push_toks(m->body, m->nbody, m, 0);
        else push_toks(0, 0, m, 0);
        return 1;
    }
    /* function-like: only if '(' follows */
    lex_raw();
    while (tok.kind == TK_NL && !stop_nl) lex_raw();
    if (tok.kind != '(') {
        t = tok;
        unread(&t);
        tok = idtok;
        return 0;
    }
    for (i = 0; i < 16; i++) { args[i] = 0; nargs[i] = 0; acap[i] = 0; }
    na = 0;
    lex_raw();
    if (tok.kind != ')' || m->nparams > 0) {
        na = 1;
        while (1) {
            if (tok.kind == TK_EOF) error("unterminated macro call");
            if (tok.kind == TK_NL) { lex_raw(); continue; }
            if (lvl == 0 && tok.kind == ')') break;
            if (lvl == 0 && tok.kind == ',') {
                if (na >= 16) error("too many macro arguments");
                na++;
                lex_raw();
                continue;
            }
            if (tok.kind == '(') lvl++;
            if (tok.kind == ')') lvl--;
            args[na - 1] = mbuf_grow(args[na - 1], &acap[na - 1], nargs[na - 1] + 1);
            args[na - 1][nargs[na - 1]] = tok;
            nargs[na - 1]++;
            lex_raw();
        }
    }
    if (na != m->nparams) error_s("wrong number of arguments to macro", id->name);
    /* pre-expand the arguments, then substitute */
    for (i = 0; i < na; i++) {
        ex = expand_list(args[i], nargs[i], &nex);
        free(args[i]);
        args[i] = ex;
        nargs[i] = nex;
    }
    for (i = 0; i < m->nbody; i++) {
        t = m->body[i];
        k = -1;
        if (t.kind == TK_ID)
            for (j = 0; j < m->nparams; j++)
                if (m->params[j] == t.id) k = j;
        if (k >= 0) {
            for (j = 0; j < nargs[k]; j++) {
                out = mbuf_grow(out, &cap, cnt + 1);
                out[cnt] = args[k][j];
                cnt++;
            }
        } else {
            out = mbuf_grow(out, &cap, cnt + 1);
            out[cnt] = t;
            cnt++;
        }
    }
    for (i = 0; i < na; i++) free(args[i]);
    push_toks(out, cnt, m, 0);
    src->owned = 1;
    return 1;
}

/* next token, macro expanded */
void next(void)
{
    Ident *id;
    int paren;
    while (1) {
        lex_raw();
        if (tok.kind != TK_ID) return;
        id = tok.id;
        if (in_if && id == id_defined) {
            lex_raw();
            paren = 0;
            if (tok.kind == '(') { paren = 1; lex_raw(); }
            if (tok.kind != TK_ID) error("defined needs a name");
            tok.kind = TK_NUM;
            tok.val = 0;
            if (tok.id->macro) tok.val = 1;
            tok.uns = 0;
            if (paren) {
                Tok t = tok;
                lex_raw();
                if (tok.kind != ')') error("expected ) after defined");
                tok = t;
            }
            return;
        }
        if (id == id_line) {
            tok.kind = TK_NUM;
            tok.val = cur_line();
            tok.uns = 0;
            return;
        }
        if (!id->macro || !expand_macro(id)) {
            if (id->kw) tok.kind = id->kw;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* directives                                                          */
/* ------------------------------------------------------------------ */

void skip_line(void)
{
    while (sc(0) >= 0 && sc(0) != '\n') adv();
}

Tok *ifl;
int nifl;
int ifp;

int if_expr(void);

int if_primary(void)
{
    int v;
    Tok t;
    if (ifp >= nifl) error("bad #if expression");
    t = ifl[ifp];
    ifp++;
    if (t.kind == TK_NUM) return t.val;
    if (t.kind == TK_ID) return 0;
    if (t.kind == '(') {
        v = if_expr();
        if (ifp < nifl && ifl[ifp].kind == ')') ifp++;
        else error("missing ) in #if");
        return v;
    }
    if (t.kind == '!') return !if_primary();
    if (t.kind == '-') return -if_primary();
    if (t.kind == '+') return if_primary();
    if (t.kind == '~') return ~if_primary();
    error("bad token in #if");
    return 0;
}

int if_prec(int k)
{
    if (k == '*' || k == '/' || k == '%') return 10;
    if (k == '+' || k == '-') return 9;
    if (k == TK_SHL || k == TK_SHR) return 8;
    if (k == '<' || k == '>' || k == TK_LE || k == TK_GE) return 7;
    if (k == TK_EQ || k == TK_NE) return 6;
    if (k == '&') return 5;
    if (k == '^') return 4;
    if (k == '|') return 3;
    if (k == TK_ANDAND) return 2;
    if (k == TK_OROR) return 1;
    return 0;
}

int if_bin(int minp)
{
    int l = if_primary();
    int r;
    int k;
    int p;
    while (ifp < nifl) {
        k = ifl[ifp].kind;
        p = if_prec(k);
        if (p == 0 || p < minp) break;
        ifp++;
        r = if_bin(p + 1);
        if (k == '*') l = l * r;
        else if (k == '/') { if (r) l = l / r; else l = 0; }
        else if (k == '%') { if (r) l = l % r; else l = 0; }
        else if (k == '+') l = l + r;
        else if (k == '-') l = l - r;
        else if (k == TK_SHL) l = l << r;
        else if (k == TK_SHR) l = l >> r;
        else if (k == '<') l = l < r;
        else if (k == '>') l = l > r;
        else if (k == TK_LE) l = l <= r;
        else if (k == TK_GE) l = l >= r;
        else if (k == TK_EQ) l = l == r;
        else if (k == TK_NE) l = l != r;
        else if (k == '&') l = l & r;
        else if (k == '^') l = l ^ r;
        else if (k == '|') l = l | r;
        else if (k == TK_ANDAND) l = l && r;
        else if (k == TK_OROR) l = l || r;
    }
    return l;
}

int if_expr(void)
{
    int c = if_bin(1);
    int a;
    int b;
    if (ifp < nifl && ifl[ifp].kind == '?') {
        ifp++;
        a = if_expr();
        if (ifp < nifl && ifl[ifp].kind == ':') ifp++;
        else error("missing : in #if");
        b = if_expr();
        if (c) return a;
        return b;
    }
    return c;
}

int eval_if_line(void)
{
    int cap = 0;
    int v;
    ifl = 0;
    nifl = 0;
    in_if = 1;
    while (1) {
        next();
        if (tok.kind == TK_NL || tok.kind == TK_EOF) break;
        ifl = mbuf_grow(ifl, &cap, nifl + 1);
        ifl[nifl] = tok;
        nifl++;
    }
    in_if = 0;
    ifp = 0;
    if (nifl == 0) error("empty #if");
    v = if_expr();
    free(ifl);
    ifl = 0;
    return v;
}

int cond_skipping(void)
{
    int i;
    for (i = 0; i < conddepth; i++)
        if (!cond_active[i]) return 1;
    return 0;
}

void include_file(char *name, int sys)
{
    char path[160];
    char *dir = cur_name();
    int n = 0;
    int i;
    if (name[0] == '/') {
        if (open_file(name)) return;
    } else {
        /* the directory of the including file first */
        for (i = 0; dir[i]; i++)
            if (dir[i] == '/') n = i + 1;
        if (n + strlen(name) < 150 && !sys) {
            memcpy(path, dir, n);
            strcpy(path + n, name);
            if (open_file(path)) return;
        }
        if (strlen(name) < 140) {
            strcpy(path, "/include/");
            strcat(path, name);
            if (open_file(path)) return;
            strcpy(path, "/calcide/include/");
            strcat(path, name);
            if (open_file(path)) return;
        }
        if (!sys && n == 0 && open_file(name)) return;
    }
    /* <stdio.h> and friends are built in; <picocalc.h> adds key and colour names */
    if (strcmp(name, "picocalc.h") == 0 || strcmp(name, "conio.h") == 0 || strcmp(name, "calcide.h") == 0) {
        open_mem(cc_picocalc_h, "<picocalc.h>");
        return;
    }
    if (sys) return;
    error_s("cannot open include file", name);
}

void define_macro(void)
{
    Ident *name;
    Macro *m;
    int cap = 0;
    lex_raw();
    if (tok.kind != TK_ID) error("#define needs a name");
    name = tok.id;
    m = (Macro *)alloc(sizeof(Macro));
    if (sc(0) == '(') {
        adv();
        m->fn = 1;
        lex_raw();
        if (tok.kind != ')') {
            while (1) {
                if (tok.kind != TK_ID) error("bad macro parameter");
                if (m->nparams >= 16) error("too many macro parameters");
                m->params[m->nparams] = tok.id;
                m->nparams++;
                lex_raw();
                if (tok.kind == ')') break;
                if (tok.kind != ',') error("expected , in macro parameters");
                lex_raw();
            }
        }
    }
    {
        Tok *tmp = 0;
        while (1) {
            lex_raw();
            if (tok.kind == TK_NL || tok.kind == TK_EOF) break;
            tmp = mbuf_grow(tmp, &cap, m->nbody + 1);
            tmp[m->nbody] = tok;
            m->nbody++;
        }
        if (m->nbody) {
            m->body = (Tok *)alloc(m->nbody * sizeof(Tok));
            memcpy(m->body, tmp, m->nbody * sizeof(Tok));
            free(tmp);
        }
    }
    name->macro = m;
}

void do_directive(void)
{
    Ident *d;
    int v;
    char *fname;
    int sys;
    int n;
    char nbuf[128];
    stop_nl = 1;
    lex_raw();
    if (tok.kind == TK_NL) { stop_nl = 0; return; }
    if (tok.kind != TK_ID) error("bad directive");
    d = tok.id;
    if (strcmp(d->name, "ifdef") == 0 || strcmp(d->name, "ifndef") == 0) {
        lex_raw();
        if (tok.kind != TK_ID) error("#ifdef needs a name");
        v = tok.id->macro != 0;
        if (d->name[2] == 'n') v = !v;
        if (conddepth >= 32) error("#if nesting too deep");
        cond_active[conddepth] = v;
        cond_done[conddepth] = v;
        cond_else[conddepth] = 0;
        conddepth++;
        skip_line();
    } else if (strcmp(d->name, "if") == 0) {
        v = 0;
        if (!cond_skipping()) v = eval_if_line() != 0;
        else skip_line();
        if (conddepth >= 32) error("#if nesting too deep");
        cond_active[conddepth] = v;
        cond_done[conddepth] = v;
        cond_else[conddepth] = 0;
        conddepth++;
        stop_nl = 0;
        return;
    } else if (strcmp(d->name, "elif") == 0) {
        if (conddepth == 0) error("#elif without #if");
        if (cond_else[conddepth - 1]) error("#elif after #else");
        if (cond_done[conddepth - 1]) {
            cond_active[conddepth - 1] = 0;
            skip_line();
        } else {
            conddepth--;
            v = 0;
            if (!cond_skipping()) v = eval_if_line() != 0;
            else skip_line();
            conddepth++;
            cond_active[conddepth - 1] = v;
            cond_done[conddepth - 1] = v;
        }
    } else if (strcmp(d->name, "else") == 0) {
        if (conddepth == 0) error("#else without #if");
        if (cond_else[conddepth - 1]) error("#else after #else");
        cond_else[conddepth - 1] = 1;
        cond_active[conddepth - 1] = !cond_done[conddepth - 1];
        cond_done[conddepth - 1] = 1;
        skip_line();
    } else if (strcmp(d->name, "endif") == 0) {
        if (conddepth == 0) error("#endif without #if");
        conddepth--;
        skip_line();
    } else if (cond_skipping()) {
        skip_line();
    } else if (strcmp(d->name, "define") == 0) {
        define_macro();
        stop_nl = 0;
        return;
    } else if (strcmp(d->name, "undef") == 0) {
        lex_raw();
        if (tok.kind == TK_ID) tok.id->macro = 0;
        skip_line();
    } else if (strcmp(d->name, "include") == 0) {
        while (sc(0) == ' ' || sc(0) == '\t') adv();
        sys = sc(0) == '<';
        if (sc(0) != '<' && sc(0) != '"') error("#include expects \"file\" or <file>");
        adv();
        n = 0;
        while (sc(0) >= 0 && sc(0) != '"' && sc(0) != '>' && sc(0) != '\n') {
            if (n < 127) { nbuf[n] = sc(0); n++; }
            adv();
        }
        nbuf[n] = 0;
        skip_line();
        if (sc(0) == '\n') adv();
        fname = str_dup(nbuf, n);
        stop_nl = 0;
        include_file(fname, sys);
        return;
    } else if (strcmp(d->name, "error") == 0) {
        error("#error");
    } else if (strcmp(d->name, "pragma") == 0 || strcmp(d->name, "line") == 0
            || strcmp(d->name, "warning") == 0) {
        skip_line();
    } else {
        error_s("unknown directive", d->name);
    }
    stop_nl = 0;
}

/* skip lines until a directive turns the condition back on */
void skip_block(void)
{
    int c;
    while (cond_skipping()) {
        c = sc(0);
        if (c < 0) error("unterminated #if");
        if (c == '\n') { adv(); continue; }
        if (c == ' ' || c == '\t' || c == '\r') { adv(); continue; }
        if (two('/', '*')) {
            adv(); adv();
            while (sc(0) >= 0 && !two('*', '/')) adv();
            adv(); adv();
            continue;
        }
        if (c == '#' && src->bol && sc(1) != '#') {
            adv();
            src->bol = 0;
            do_directive();
            continue;
        }
        src->bol = 0;
        skip_line();
    }
}

/* ------------------------------------------------------------------ */
/* token helpers for the parser                                        */
/* ------------------------------------------------------------------ */

Tok peek_tok;

void peek(void)
{
    Tok cur = tok;
    next();
    peek_tok = tok;
    unread(&peek_tok);
    tok = cur;
}

void expect(int k, char *what)
{
    if (tok.kind != k) error_s("expected", what);
    next();
}

int accept(int k)
{
    if (tok.kind == k) { next(); return 1; }
    return 0;
}

/* ------------------------------------------------------------------ */
/* types                                                               */
/* ------------------------------------------------------------------ */

Type *new_type(int kind, int size, int align)
{
    Type *t = (Type *)alloc(sizeof(Type));
    t->kind = kind;
    t->size = size;
    t->align = align;
    t->complete = 1;
    t->len = -1;
    return t;
}

Type *ptr_to(Type *b)
{
    Type *t;
    if (b->ptrto) return b->ptrto;
    t = new_type(T_PTR, 4, 4);
    t->base = b;
    b->ptrto = t;
    return t;
}

Type *array_of(Type *b, int n)
{
    Type *t = new_type(T_ARRAY, 0, b->align);
    t->base = b;
    t->len = n;
    if (n >= 0) t->size = b->size * n;
    else t->complete = 0;
    return t;
}

int is_int(Type *t)
{
    return t->kind == T_CHAR || t->kind == T_SHORT || t->kind == T_INT;
}

int is_arith(Type *t)
{
    return is_int(t) || t->kind == T_FLOAT;
}

int is_ptr(Type *t)
{
    return t->kind == T_PTR;
}

int is_scalar(Type *t)
{
    return is_arith(t) || t->kind == T_PTR;
}

int elem_size(Type *t)
{
    int s = t->base->size;
    if (t->base->kind == T_VOID || t->base->kind == T_FUNC) s = 1;
    if (s <= 0) error("pointer to incomplete type");
    return s;
}

/* ------------------------------------------------------------------ */
/* code emission                                                       */
/* ------------------------------------------------------------------ */

void need_code(int n)
{
    if (cpos + n > ccap) code = grow(code, &ccap, cpos + n);
}

void hist_clear(void)
{
    nhist = 0;
}

void hist_push(int pos)
{
    int i;
    if (nhist == 8) {
        for (i = 0; i < 7; i++) hist_pos[i] = hist_pos[i + 1];
        nhist = 7;
    }
    hist_pos[nhist] = pos;
    nhist++;
}

void out1(int v)
{
    need_code(1);
    code[cpos] = v;
    cpos++;
}

void out2(int v)
{
    out1(v & 255);
    out1((v >> 8) & 255);
}

void out4(int v)
{
    out1(v & 255);
    out1((v >> 8) & 255);
    out1((v >> 16) & 255);
    out1((v >> 24) & 255);
}

int get4(int pos)
{
    return (code[pos] & 255) | ((code[pos + 1] & 255) << 8)
         | ((code[pos + 2] & 255) << 16) | ((code[pos + 3] & 255) << 24);
}

void put4(int pos, int v)
{
    code[pos] = v & 255;
    code[pos + 1] = (v >> 8) & 255;
    code[pos + 2] = (v >> 16) & 255;
    code[pos + 3] = (v >> 24) & 255;
}

/* every non-IMM instruction breaks the folding history */
void op(int o)
{
    hist_clear();
    last_was_lea = -1;
    out1(o);
}

void op4(int o, int v)
{
    hist_clear();
    out1(o);
    out4(v);
}

/* a foldable constant */
void emit_imm(int v)
{
    int at = cpos;
    if (v >= -128 && v <= 127) { out1(OP_IMM8); out1(v & 255); }
    else if (v >= -32768 && v <= 32767) { out1(OP_IMM16); out2(v); }
    else { out1(OP_IMM); out4(v); }
    hist_push(at);
}

int imm_value(int pos)
{
    int o = code[pos] & 255;
    int v;
    if (o == OP_IMM8) {
        v = code[pos + 1] & 255;
        if (v > 127) v = v - 256;
        return v;
    }
    if (o == OP_IMM16) {
        v = (code[pos + 1] & 255) | ((code[pos + 2] & 255) << 8);
        if (v > 32767) v = v - 65536;
        return v;
    }
    return get4(pos + 1);
}

/* is the code from 'start' exactly one foldable constant? */
int is_const_since(int start)
{
    return nhist >= 1 && hist_pos[nhist - 1] == start;
}

/* an address that must be relocated: never folded */
void emit_addr(int seg, int off)
{
    hist_clear();
    last_was_lea = -1;
    relocs = grow_ints(relocs, &rcap, nrelocs + 1);
    out1(OP_IMM);
    relocs[nrelocs] = cpos * 4 + seg;
    nrelocs++;
    out4(off);
}

float bits_f(int b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}

int f_bits(float f)
{
    int b;
    memcpy(&b, &f, 4);
    return b;
}

/* fold a unary operation on the last constant */
int fold1(int o)
{
    unsigned ua;
    int p;
    int a;
    int r;
    if (nhist < 1) return 0;
    p = hist_pos[nhist - 1];
    if (p + 1 > cpos) return 0;
    a = imm_value(p);
    if (o == OP_NEG) r = 0u - (unsigned)a;
    else if (o == OP_NOT) r = ~a;
    else if (o == OP_LNOT) r = !a;
    else if (o == OP_FNEG) r = a ^ 0x80000000;
    else if (o == OP_ITOF) r = f_bits((float)a);
    else if (o == OP_UTOF) r = f_bits((float)(unsigned)a);
    else if (o == OP_FTOI) r = (int)bits_f(a);
    else if (o == OP_FTOU) r = (int)(unsigned)bits_f(a);
    else if (o == OP_SEXT8) { r = a & 255; if (r > 127) r = r - 256; }
    else if (o == OP_ZEXT8) r = a & 255;
    else if (o == OP_SEXT16) { r = a & 65535; if (r > 32767) r = r - 65536; }
    else if (o == OP_ZEXT16) r = a & 65535;
    else return 0;
    nhist--;
    cpos = p;
    emit_imm(r);
    return 1;
}

int fold2(int o)
{
    int p1;
    int p2;
    int a;
    int b;
    int r;
    unsigned ua;
    unsigned ub;
    if (nhist < 2) return 0;
    p1 = hist_pos[nhist - 2];
    p2 = hist_pos[nhist - 1];
    a = imm_value(p1);
    b = imm_value(p2);
    ua = a;
    ub = b;
    if (o == OP_ADD) r = ua + ub;
    else if (o == OP_SUB) r = ua - ub;
    else if (o == OP_MUL) r = ua * ub;
    else if (o == OP_DIV) { if (b == 0 || (b == -1 && a == INT_MIN)) return 0; r = a / b; }
    else if (o == OP_MOD) { if (b == 0 || b == -1) return 0; r = a % b; }
    else if (o == OP_DIVU) { if (b == 0) return 0; r = ua / ub; }
    else if (o == OP_MODU) { if (b == 0) return 0; r = ua % ub; }
    else if (o == OP_AND) r = a & b;
    else if (o == OP_OR) r = a | b;
    else if (o == OP_XOR) r = a ^ b;
    else if (o == OP_SHL) r = ua << (b & 31);
    else if (o == OP_SHR) r = ua >> (b & 31);
    else if (o == OP_SAR) r = a >> (b & 31);
    else if (o == OP_EQ) r = a == b;
    else if (o == OP_NE) r = a != b;
    else if (o == OP_LT) r = a < b;
    else if (o == OP_LE) r = a <= b;
    else if (o == OP_GT) r = a > b;
    else if (o == OP_GE) r = a >= b;
    else if (o == OP_LTU) r = ua < ub;
    else if (o == OP_LEU) r = ua <= ub;
    else if (o == OP_GTU) r = ua > ub;
    else if (o == OP_GEU) r = ua >= ub;
    else if (o == OP_FADD) r = f_bits(bits_f(a) + bits_f(b));
    else if (o == OP_FSUB) r = f_bits(bits_f(a) - bits_f(b));
    else if (o == OP_FMUL) r = f_bits(bits_f(a) * bits_f(b));
    else if (o == OP_FDIV) { if (b == 0 || b == 0x80000000) return 0; r = f_bits(bits_f(a) / bits_f(b)); }
    else if (o == OP_ITOF2) r = 0;
    else return 0;
    if (o == OP_ITOF2) {
        /* convert the lower of the two constants in place */
        a = f_bits((float)a);
        nhist = nhist - 2;
        cpos = p1;
        emit_imm(a);
        emit_imm(b);
        return 1;
    }
    nhist = nhist - 2;
    cpos = p1;
    emit_imm(r);
    return 1;
}

int is_fold1(int o)
{
    return o == OP_NEG || o == OP_NOT || o == OP_LNOT || o == OP_FNEG
        || (o >= OP_ITOF && o <= OP_FTOU) || (o >= OP_SEXT8 && o <= OP_ZEXT16);
}

/* emit an operator, folding it if its operands are constants */
void emit_op(int o)
{
    if (is_fold1(o)) {
        if (fold1(o)) return;
    } else if (o == OP_ITOF2 || o == OP_UTOF2) {
        if (o == OP_ITOF2 && fold2(o)) return;
    } else if (o >= OP_ADD && o <= OP_FDIV && o != OP_NEG && o != OP_NOT && o != OP_LNOT && o != OP_FNEG) {
        if (fold2(o)) return;
    }
    op(o);
}

int here(void)
{
    hist_clear();     /* a jump may land here */
    return cpos;
}

/* emit a jump-like instruction whose operand is a patch chain link */
int emit_jump(int o, int chain)
{
    int at;
    op(o);
    at = cpos;
    out4(chain);
    return at;
}

void patch_chain(int chain, int target)
{
    int nx;
    while (chain) {
        nx = get4(chain);
        put4(chain, target);
        chain = nx;
    }
}

void emit_lea(int off)
{
    if (off >= -32768 && off <= 32767) { op(OP_LEA); out2(off); }
    else op4(OP_LEA32, off);
}

void emit_load(Type *t)
{
    if (t->kind == T_CHAR) { if (t->uns) op(OP_LDBU); else op(OP_LDB); }
    else if (t->kind == T_SHORT) { if (t->uns) op(OP_LDHU); else op(OP_LDH); }
    else if (t->kind == T_INT || t->kind == T_FLOAT || t->kind == T_PTR) {
        /* fuse LEA + LDW */
        if (last_was_lea >= 0 && last_was_lea == cpos - 3 && (code[cpos - 3] & 255) == OP_LEA && nhist == 0) {
            code[cpos - 3] = OP_LLW;
            last_was_lea = -1;
            return;
        }
        op(OP_LDW);
    }
    else if (t->kind == T_VOID) error("void value used");
    else error("cannot load this type");
}

void emit_store(Type *t)
{
    if (t->kind == T_CHAR) op(OP_STB);
    else if (t->kind == T_SHORT) op(OP_STH);
    else if (t->kind == T_INT || t->kind == T_FLOAT || t->kind == T_PTR) op(OP_STW);
    else error("cannot assign this type");
}

void emit_line(void)
{
    int ln = cur_line();
    Src *s = src;
    int fi;
    while (s && s->kind == SRC_TOK) s = s->prev;
    fi = 0;
    if (s) fi = s->fidx;
    if (ln == last_line && fi == last_lfile) return;
    if (nocode_depth) return;
    last_line = ln;
    last_lfile = fi;
    lines = grow_ints(lines, &lcap, nlines * 2 + 2);
    lines[nlines * 2] = cpos;
    lines[nlines * 2 + 1] = (ln & 0xFFFFFF) | (fi << 24);
    nlines++;
}

/* reference to a function: a direct call, or its address */
void emit_fn_ref(int o, Sym *s)
{
    if (s->sys) error_s("cannot take the address of builtin", s->name->name);
    if (nocode_depth && !s->defined) error("function reference in a constant expression");
    hist_clear();
    out1(o);
    if (s->defined) out4(s->addr);
    else {
        int at = cpos;
        out4(s->patch);
        s->patch = at;
    }
}

/* ------------------------------------------------------------------ */
/* scopes and symbols                                                  */
/* ------------------------------------------------------------------ */

void enter_scope(void)
{
    depth++;
    if (depth >= 64) error("blocks nested too deeply");
    scope_syms[depth] = 0;
    scope_tags[depth] = 0;
}

void leave_scope(void)
{
    Sym *s = scope_syms[depth];
    Tag *t = scope_tags[depth];
    while (s) {
        Sym *nx = s->scnext;
        s->name->sym = s->shadow;
        /* globals declared from a block were put in scope 0, so everything
         * here is a local, a block typedef or an enum constant */
        s->scnext = sym_free;
        sym_free = s;
        s = nx;
    }
    while (t) {
        t->name->tag = t->shadow;
        t = t->scnext;
    }
    depth--;
}

Sym *add_sym(Ident *name, int kind, Type *type)
{
    Sym *s;
    if (depth > 0 && sym_free) {
        s = sym_free;
        sym_free = s->scnext;
        memset(s, 0, sizeof(Sym));
    } else s = (Sym *)alloc(sizeof(Sym));
    s->name = name;
    s->kind = kind;
    s->type = type;
    s->depth = depth;
    s->shadow = name->sym;
    name->sym = s;
    s->scnext = scope_syms[depth];
    scope_syms[depth] = s;
    return s;
}

/* a file-scope symbol declared from inside a block */
Sym *add_global_sym(Ident *name, int kind, Type *type)
{
    Sym *s = (Sym *)alloc(sizeof(Sym));
    Sym *p = name->sym;
    Sym *prev = 0;
    s->name = name;
    s->kind = kind;
    s->type = type;
    s->depth = 0;
    while (p && p->depth > 0) { prev = p; p = p->shadow; }
    s->shadow = p;
    if (prev) prev->shadow = s;
    else name->sym = s;
    s->scnext = scope_syms[0];
    scope_syms[0] = s;
    return s;
}

Tag *add_tag(Ident *name, Type *type)
{
    Tag *t = (Tag *)alloc(sizeof(Tag));
    t->name = name;
    t->type = type;
    t->depth = depth;
    t->shadow = name->tag;
    name->tag = t;
    t->scnext = scope_tags[depth];
    scope_tags[depth] = t;
    return t;
}

int alloc_local(Type *t)
{
    int a = t->align;
    int off;
    if (a < 1) a = 1;
    if (t->size <= 0) error("local variable of incomplete type");
    frame_size = (frame_size + a - 1) & ~(a - 1);
    off = frame_size;
    frame_size = frame_size + t->size;
    frame_size = (frame_size + 3) & ~3;
    if (frame_size > frame_max) frame_max = frame_size;
    return off;
}

int alloc_global(Type *t, int initialized, int *seg)
{
    int a = t->align;
    int off;
    if (a < 1) a = 1;
    if (initialized) {
        dpos = (dpos + a - 1) & ~(a - 1);
        off = dpos;
        if (t->size > 0) {
            dpos = dpos + t->size;
            if (dpos > dcap) dataseg = grow(dataseg, &dcap, dpos);
            memset(dataseg + off, 0, t->size);
        }
        *seg = SEG_DATA;
        return off;
    }
    if (t->size <= 0) error("global of incomplete type");
    bss = (bss + a - 1) & ~(a - 1);
    off = bss;
    bss = bss + t->size;
    *seg = SEG_BSS;
    return off;
}

/* ------------------------------------------------------------------ */
/* declarations: specifiers                                            */
/* ------------------------------------------------------------------ */

int is_typename(void)
{
    int k = tok.kind;
    if (k >= KW_INT && k <= KW_REGISTER) return 1;
    if (k == KW_INLINE || k == KW_BOOL) return 1;
    if (k == TK_ID && tok.id->sym && tok.id->sym->kind == S_TYPEDEF) return 1;
    return 0;
}

Type *declarator(Type *base, Ident **name);
Type *declspec(void);
Type *type_name(void);
int const_expr(void);
Type *assign_expr(void);
Type *expr(void);
Type *rv(Type *t);
void convert(Type *from, Type *to);

int st_typedef;
int st_static;
int st_extern;

Type *struct_decl(int isunion)
{
    Ident *tagname = 0;
    Tag *tg;
    Type *t;
    Type *base;
    Type *mt;
    Ident *mname;
    Member *last = 0;
    Member *m;
    int off = 0;
    int maxsz = 0;
    int a;
    int sv1;
    int sv2;
    int sv3;
    if (tok.kind == TK_ID) {
        tagname = tok.id;
        next();
    }
    if (tok.kind != '{') {
        if (!tagname) error("expected struct name");
        tg = tagname->tag;
        if (tg) return tg->type;
        t = new_type(T_STRUCT, 0, 1);
        t->complete = 0;
        t->isunion = isunion;
        add_tag(tagname, t);
        return t;
    }
    t = 0;
    if (tagname) {
        tg = tagname->tag;
        if (tg && tg->depth == depth && tg->type->kind == T_STRUCT) {
            if (tg->type->complete) error_s("redefinition of struct", tagname->name);
            t = tg->type;
        }
    }
    if (!t) {
        t = new_type(T_STRUCT, 0, 1);
        if (tagname) add_tag(tagname, t);
    }
    t->isunion = isunion;
    t->complete = 0;
    next();
    while (tok.kind != '}') {
        sv1 = st_typedef;
        sv2 = st_static;
        sv3 = st_extern;
        base = declspec();
        st_typedef = sv1;
        st_static = sv2;
        st_extern = sv3;
        while (1) {
            mname = 0;
            mt = declarator(base, &mname);
            if (accept(':')) const_expr();   /* bit-fields: width ignored */
            if (mt->size <= 0 && !(mt->kind == T_ARRAY && mt->len < 0)) error("member of incomplete type");
            a = mt->align;
            if (a < 1) a = 1;
            m = (Member *)alloc(sizeof(Member));
            m->name = mname;
            m->type = mt;
            if (isunion) {
                m->off = 0;
                if (mt->size > maxsz) maxsz = mt->size;
            } else {
                off = (off + a - 1) & ~(a - 1);
                m->off = off;
                off = off + mt->size;
            }
            if (a > t->align) t->align = a;
            if (last) last->next = m;
            else t->mem = m;
            last = m;
            if (!accept(',')) break;
        }
        expect(';', ";");
    }
    next();
    if (isunion) off = maxsz;
    t->size = (off + t->align - 1) & ~(t->align - 1);
    t->complete = 1;
    return t;
}

Type *enum_decl(void)
{
    Ident *name;
    int v = 0;
    Sym *s;
    if (tok.kind == TK_ID) {
        next();
        if (tok.kind != '{') return ty_int;
    }
    expect('{', "{");
    while (tok.kind != '}') {
        if (tok.kind != TK_ID) error("expected enumerator");
        name = tok.id;
        next();
        if (accept('=')) v = const_expr();
        s = add_sym(name, S_ENUM, ty_int);
        s->addr = v;
        v++;
        if (!accept(',')) break;
    }
    expect('}', "}");
    return ty_int;
}

Type *declspec(void)
{
    int nint = 0;
    int nchar = 0;
    int nshort = 0;
    int nlong = 0;
    int uns = 0;
    int sgn = 0;
    int nfloat = 0;
    int nvoid = 0;
    Type *t = 0;
    int k;
    st_typedef = 0;
    st_static = 0;
    st_extern = 0;
    while (1) {
        k = tok.kind;
        if (k == KW_TYPEDEF) st_typedef = 1;
        else if (k == KW_STATIC) st_static = 1;
        else if (k == KW_EXTERN) st_extern = 1;
        else if (k == KW_CONST || k == KW_VOLATILE || k == KW_AUTO || k == KW_REGISTER || k == KW_INLINE) { }
        else if (k == KW_INT) nint++;
        else if (k == KW_CHAR) nchar++;
        else if (k == KW_SHORT) nshort++;
        else if (k == KW_LONG) nlong++;
        else if (k == KW_UNSIGNED) uns = 1;
        else if (k == KW_SIGNED) sgn = 1;
        else if (k == KW_FLOAT || k == KW_DOUBLE) nfloat++;
        else if (k == KW_VOID) nvoid++;
        else if (k == KW_BOOL) nint++;
        else if (k == KW_STRUCT || k == KW_UNION) {
            next();
            t = struct_decl(k == KW_UNION);
            continue;
        } else if (k == KW_ENUM) {
            next();
            t = enum_decl();
            continue;
        } else if (k == TK_ID && tok.id->sym && tok.id->sym->kind == S_TYPEDEF
                   && !t && !nint && !nchar && !nshort && !nlong && !uns && !sgn && !nfloat && !nvoid) {
            t = tok.id->sym->type;
        } else break;
        next();
    }
    if (t) return t;
    if (nvoid) return ty_void;
    if (nfloat) return ty_float;
    if (nchar) { if (uns) return ty_uchar; return ty_char; }
    if (nshort) { if (uns) return ty_ushort; return ty_short; }
    if (uns) return ty_uint;
    return ty_int;
}

/* ------------------------------------------------------------------ */
/* declarations: declarators                                           */
/* ------------------------------------------------------------------ */

Type *func_param_list(Type *t);

Type *func_params(Type *ret)
{
    Type *t = new_type(T_FUNC, 1, 1);
    Param *last = 0;
    Param *p;
    Type *pt;
    Ident *pn;
    Type *b;
    int sv1 = st_typedef;
    int sv2 = st_static;
    int sv3 = st_extern;
    t->base = ret;
    t->proto = 1;
    if (tok.kind == ')') {
        next();
        t->proto = 0;     /* f(): unknown parameters */
        return t;
    }
    t = func_param_list(t);
    st_typedef = sv1;
    st_static = sv2;
    st_extern = sv3;
    return t;
}

Type *func_param_list(Type *t)
{
    Param *last = 0;
    Param *p;
    Type *pt;
    Ident *pn;
    Type *b;
    if (tok.kind == KW_VOID) {
        peek();
        if (peek_tok.kind == ')') {
            next();
            next();
            return t;
        }
    }
    while (1) {
        if (tok.kind == TK_ELLIPSIS) {
            next();
            t->variadic = 1;
            break;
        }
        b = declspec();
        pn = 0;
        pt = declarator(b, &pn);
        if (pt->kind == T_ARRAY) pt = ptr_to(pt->base);
        if (pt->kind == T_FUNC) pt = ptr_to(pt);
        p = (Param *)alloc(sizeof(Param));
        p->name = pn;
        p->type = pt;
        if (last) last->next = p;
        else t->params = p;
        last = p;
        t->nparams++;
        if (!accept(',')) break;
    }
    expect(')', ")");
    return t;
}

Type *type_suffix(Type *t)
{
    int n;
    Type *inner;
    if (accept('[')) {
        n = -1;
        if (tok.kind != ']') {
            n = const_expr();
            if (n < 0) error("negative array size");
        }
        expect(']', "]");
        inner = type_suffix(t);
        if (inner->size <= 0 && inner->kind != T_VOID) {
            if (!(inner->kind == T_ARRAY && inner->len < 0 && 0)) {
                if (inner->size <= 0) error("array of incomplete type");
            }
        }
        return array_of(inner, n);
    }
    if (accept('(')) {
        return func_params(t);
    }
    return t;
}

Type *declarator(Type *base, Ident **name)
{
    Type *t = base;
    Type *hole;
    Type *inner;
    Type *full;
    while (tok.kind == '*') {
        next();
        t = ptr_to(t);
        while (tok.kind == KW_CONST || tok.kind == KW_VOLATILE) next();
    }
    if (tok.kind == '(') {
        peek();
        /* a parenthesised declarator, not a parameter list */
        if (peek_tok.kind == '*' || peek_tok.kind == '(' || (peek_tok.kind == TK_ID
            && !(peek_tok.id->sym && peek_tok.id->sym->kind == S_TYPEDEF))) {
            next();
            hole = new_type(T_HOLE, 0, 1);
            inner = declarator(hole, name);
            expect(')', ")");
            full = type_suffix(t);
            *hole = *full;
            return inner;
        }
    }
    if (tok.kind == TK_ID) {
        *name = tok.id;
        next();
    }
    return type_suffix(t);
}

Type *type_name(void)
{
    Ident *n = 0;
    Type *b = declspec();
    return declarator(b, &n);
}

/* ------------------------------------------------------------------ */
/* expressions                                                         */
/* ------------------------------------------------------------------ */

/* materialise an rvalue: load an lvalue, decay arrays and functions */
Type *rv(Type *t)
{
    if (pending_fn) {
        emit_fn_ref(OP_IMM, pending_fn);
        pending_fn = 0;
        lv = 0;
        return ptr_to(t);
    }
    if (t->kind == T_ARRAY) { lv = 0; return ptr_to(t->base); }
    if (t->kind == T_FUNC) { lv = 0; return ptr_to(t); }
    if (t->kind == T_STRUCT) { lv = 0; return t; }
    if (lv) {
        emit_load(t);
        lv = 0;
    }
    return t;
}

/* implicit conversion of the value on top of the stack */
void convert(Type *from, Type *to)
{
    if (to->kind == T_VOID) return;
    if (to->kind == T_FLOAT && is_int(from)) {
        if (from->uns && from->kind == T_INT) emit_op(OP_UTOF);
        else emit_op(OP_ITOF);
    } else if (is_int(to) && from->kind == T_FLOAT) {
        if (to->uns) emit_op(OP_FTOU);
        else emit_op(OP_FTOI);
    } else if (to->kind == T_STRUCT || from->kind == T_STRUCT) {
        if (to != from) error("incompatible struct types");
    } else if (to->kind == T_FLOAT && from->kind == T_PTR) {
        error("cannot convert pointer to float");
    } else if (to->kind == T_PTR && from->kind == T_FLOAT) {
        error("cannot convert float to pointer");
    }
}

/* explicit cast */
void cast_to(Type *from, Type *to)
{
    convert(from, to);
    if (to->kind == T_CHAR && from->kind != T_CHAR) {
        if (to->uns) emit_op(OP_ZEXT8);
        else emit_op(OP_SEXT8);
    } else if (to->kind == T_SHORT && from->kind != T_SHORT && from->kind != T_CHAR) {
        if (to->uns) emit_op(OP_ZEXT16);
        else emit_op(OP_SEXT16);
    } else if (to->kind == T_CHAR && from->kind == T_CHAR && to->uns != from->uns) {
        if (to->uns) emit_op(OP_ZEXT8);
        else emit_op(OP_SEXT8);
    }
}

Type *usual(Type *l, Type *r)
{
    if (l->kind == T_FLOAT || r->kind == T_FLOAT) return ty_float;
    if ((l->kind == T_INT && l->uns) || (r->kind == T_INT && r->uns)) return ty_uint;
    return ty_int;
}

/* make both operands (l below, r on top) arithmetic of type 'to' */
void promote2(Type *l, Type *r, Type *to)
{
    if (to->kind == T_FLOAT) {
        if (l->kind != T_FLOAT) {
            if (l->uns && l->kind == T_INT) op(OP_UTOF2);
            else emit_op(OP_ITOF2);
        }
        if (r->kind != T_FLOAT) {
            if (r->uns && r->kind == T_INT) emit_op(OP_UTOF);
            else emit_op(OP_ITOF);
        }
    }
}

/* binary operator k with left type l (below) and right type r (on top) */
Type *binop_code(int k, Type *l, Type *r)
{
    Type *t;
    int sz;
    if (l->kind == T_STRUCT || r->kind == T_STRUCT) error("invalid operands (struct)");
    if (k == '+' || k == '-') {
        if (is_ptr(l) && is_int(r)) {
            sz = elem_size(l);
            if (sz != 1) { emit_imm(sz); emit_op(OP_MUL); }
            if (k == '+') emit_op(OP_ADD);
            else emit_op(OP_SUB);
            return l;
        }
        if (is_int(l) && is_ptr(r) && k == '+') {
            sz = elem_size(r);
            op(OP_SWAP);
            if (sz != 1) { emit_imm(sz); emit_op(OP_MUL); }
            emit_op(OP_ADD);
            return r;
        }
        if (is_ptr(l) && is_ptr(r) && k == '-') {
            sz = elem_size(l);
            emit_op(OP_SUB);
            if (sz != 1) { emit_imm(sz); emit_op(OP_DIV); }
            return ty_int;
        }
    }
    if (k == TK_EQ || k == TK_NE || k == '<' || k == '>' || k == TK_LE || k == TK_GE) {
        if (is_ptr(l) || is_ptr(r)) {
            if (k == TK_EQ) emit_op(OP_EQ);
            else if (k == TK_NE) emit_op(OP_NE);
            else if (k == '<') emit_op(OP_LTU);
            else if (k == '>') emit_op(OP_GTU);
            else if (k == TK_LE) emit_op(OP_LEU);
            else emit_op(OP_GEU);
            return ty_int;
        }
        t = usual(l, r);
        promote2(l, r, t);
        if (t->kind == T_FLOAT) {
            if (k == TK_EQ) emit_op(OP_FEQ);
            else if (k == TK_NE) emit_op(OP_FNE);
            else if (k == '<') emit_op(OP_FLT);
            else if (k == '>') emit_op(OP_FGT);
            else if (k == TK_LE) emit_op(OP_FLE);
            else emit_op(OP_FGE);
        } else if (t->uns) {
            if (k == TK_EQ) emit_op(OP_EQ);
            else if (k == TK_NE) emit_op(OP_NE);
            else if (k == '<') emit_op(OP_LTU);
            else if (k == '>') emit_op(OP_GTU);
            else if (k == TK_LE) emit_op(OP_LEU);
            else emit_op(OP_GEU);
        } else {
            if (k == TK_EQ) emit_op(OP_EQ);
            else if (k == TK_NE) emit_op(OP_NE);
            else if (k == '<') emit_op(OP_LT);
            else if (k == '>') emit_op(OP_GT);
            else if (k == TK_LE) emit_op(OP_LE);
            else emit_op(OP_GE);
        }
        return ty_int;
    }
    if (!is_arith(l) || !is_arith(r)) error("invalid operands to binary operator");
    t = usual(l, r);
    if (k == '%' || k == '&' || k == '|' || k == '^' || k == TK_SHL || k == TK_SHR) {
        if (t->kind == T_FLOAT) error("integer operator on float");
        if (k == '%') { if (t->uns) emit_op(OP_MODU); else emit_op(OP_MOD); }
        else if (k == '&') emit_op(OP_AND);
        else if (k == '|') emit_op(OP_OR);
        else if (k == '^') emit_op(OP_XOR);
        else if (k == TK_SHL) emit_op(OP_SHL);
        else {
            if (l->uns && l->kind == T_INT) emit_op(OP_SHR);
            else emit_op(OP_SAR);
            if (l->uns && l->kind == T_INT) return ty_uint;
            return ty_int;
        }
        return t;
    }
    promote2(l, r, t);
    if (t->kind == T_FLOAT) {
        if (k == '+') emit_op(OP_FADD);
        else if (k == '-') emit_op(OP_FSUB);
        else if (k == '*') emit_op(OP_FMUL);
        else emit_op(OP_FDIV);
        return t;
    }
    if (k == '+') emit_op(OP_ADD);
    else if (k == '-') emit_op(OP_SUB);
    else if (k == '*') emit_op(OP_MUL);
    else { if (t->uns) emit_op(OP_DIVU); else emit_op(OP_DIV); }
    return t;
}

/* pointer to the char data of a new string literal */
int add_string(char *s, int n)
{
    int off;
    if (spos + n + 1 > scap) strseg = grow(strseg, &scap, spos + n + 1);
    off = spos;
    memcpy(strseg + spos, s, n);
    strseg[spos + n] = 0;
    spos = spos + n + 1;
    return off;
}

/* adjacent string literals concatenate */
char *catbuf;
int catcap;

void string_lit(char **s, int *n)
{
    char *buf = tok.str;
    int len = tok.len;
    next();
    if (tok.kind != TK_STR) {
        *s = buf;
        *n = len;
        return;
    }
    /* several literals: join them in a scratch buffer */
    catbuf = grow(catbuf, &catcap, len + 256);
    memcpy(catbuf, buf, len);
    while (tok.kind == TK_STR) {
        if (len + tok.len + 1 > catcap) catbuf = grow(catbuf, &catcap, len + tok.len + 1);
        memcpy(catbuf + len, tok.str, tok.len);
        len = len + tok.len;
        next();
    }
    *s = catbuf;
    *n = len;
}

Type *call(Type *ft, Sym *direct)
{
    Param *p;
    Type *at;
    int n = 0;
    int tmp;
    Type *ret;
    if (ft->kind == T_PTR && ft->base->kind == T_FUNC) ft = ft->base;
    if (ft->kind != T_FUNC) error("called object is not a function");
    p = ft->params;
    next();   /* past '(' */
    if (tok.kind != ')') {
        while (1) {
            at = assign_expr();
            at = rv(at);
            if (p) {
                convert(at, p->type);
                p = p->next;
            } else if (ft->proto && !ft->variadic) {
                error("too many arguments");
            }
            n++;
            if (!accept(',')) break;
        }
    }
    expect(')', ")");
    if (p) error("too few arguments");
    if (n > 255) error("too many arguments");
    if (direct && direct->sys) {
        hist_clear();
        out1(OP_SYS);
        out2(direct->sys);
        out1(n);
    } else if (direct) {
        emit_fn_ref(OP_CALL, direct);
    } else {
        op(OP_CALLI);
        out1(n);
    }
    ret = ft->base;
    lv = 0;
    if (ret->kind == T_STRUCT) {
        /* copy out of the callee's dead frame straight away */
        tmp = alloc_local(ret);
        emit_lea(tmp);
        op(OP_SWAP);
        op4(OP_MEMCPY, ret->size);
    }
    return ret;
}

Type *primary(void)
{
    Type *t;
    Sym *s;
    Ident *id;
    char *str;
    int n;
    int off;
    lv = 0;
    if (tok.kind == TK_NUM) {
        emit_imm(tok.val);
        if (tok.uns) t = ty_uint;
        else t = ty_int;
        next();
        return t;
    }
    if (tok.kind == TK_FNUM) {
        emit_imm(tok.val);
        next();
        return ty_float;
    }
    if (tok.kind == TK_STR) {
        string_lit(&str, &n);
        off = add_string(str, n);
        emit_addr(SEG_STR, off);
        return ty_charp;
    }
    if (tok.kind == '(') {
        next();
        t = expr();
        expect(')', ")");
        return t;
    }
    if (tok.kind == TK_ID) {
        id = tok.id;
        s = id->sym;
        next();
        if (!s) {
            if (tok.kind == '(') {
                /* implicit declaration: int f() */
                warn_s("implicit declaration of function", id->name);
                t = new_type(T_FUNC, 1, 1);
                t->base = ty_int;
                s = add_global_sym(id, S_FUNC, t);
            } else error_s("undeclared identifier", id->name);
        }
        if (s->kind == S_ENUM) {
            emit_imm(s->addr);
            return ty_int;
        }
        if (s->kind == S_TYPEDEF) error_s("unexpected type name", id->name);
        if (s->kind == S_FUNC) {
            if (tok.kind == '(') return call(s->type, s);
            pending_fn = s;
            return s->type;
        }
        if (s->kind == S_LOCAL) {
            emit_lea(s->addr);
            last_was_lea = cpos - 3;
            if (s->addr < -32768 || s->addr > 32767) last_was_lea = -1;
        } else {
            emit_addr(s->seg, s->addr);
        }
        lv = 1;
        return s->type;
    }
    if (tok.kind == TK_EOF) error("unexpected end of file");
    error("expected expression");
    return 0;
}

Member *find_member(Type *t, Ident *name)
{
    Member *m;
    if (t->kind != T_STRUCT) error("member access on a non-struct");
    if (!t->complete) error("use of incomplete struct");
    for (m = t->mem; m; m = m->next)
        if (m->name == name) return m;
    error_s("no such member", name->name);
    return 0;
}

int inc_mode(Type *t)
{
    if (t->kind == T_CHAR) { if (t->uns) return INC_BU; return INC_B; }
    if (t->kind == T_SHORT) { if (t->uns) return INC_HU; return INC_H; }
    if (t->kind == T_FLOAT) return INC_F;
    if (t->kind == T_INT || t->kind == T_PTR) return INC_W;
    error("invalid operand to ++/--");
    return 0;
}

void emit_inc(Type *t, int delta, int post)
{
    int m = inc_mode(t);
    if (t->kind == T_PTR) delta = delta * elem_size(t);
    if (post) m = m | INC_POST;
    op(OP_INC);
    out1(m);
    out4(delta);
}

Type *postfix(void)
{
    Type *t = primary();
    Type *it;
    Member *m;
    int sz;
    while (1) {
        if (tok.kind == '[') {
            next();
            t = rv(t);
            if (!is_ptr(t)) error("subscripted value is not an array or pointer");
            it = expr();
            it = rv(it);
            if (!is_int(it)) error("array subscript is not an integer");
            expect(']', "]");
            sz = elem_size(t);
            if (sz != 1) { emit_imm(sz); emit_op(OP_MUL); }
            emit_op(OP_ADD);
            t = t->base;
            lv = 1;
        } else if (tok.kind == '(') {
            if (pending_fn) {
                Sym *s = pending_fn;
                pending_fn = 0;
                t = call(s->type, s);
            } else {
                t = rv(t);
                t = call(t, 0);
            }
        } else if (tok.kind == '.') {
            next();
            if (tok.kind != TK_ID) error("expected member name");
            m = find_member(t, tok.id);
            next();
            if (m->off) { emit_imm(m->off); emit_op(OP_ADD); }
            t = m->type;
            lv = 1;
        } else if (tok.kind == TK_ARROW) {
            next();
            t = rv(t);
            if (!is_ptr(t)) error("-> on a non-pointer");
            if (tok.kind != TK_ID) error("expected member name");
            m = find_member(t->base, tok.id);
            next();
            if (m->off) { emit_imm(m->off); emit_op(OP_ADD); }
            t = m->type;
            lv = 1;
        } else if (tok.kind == TK_INC || tok.kind == TK_DEC) {
            if (!lv) error("lvalue required for ++/--");
            if (tok.kind == TK_INC) emit_inc(t, 1, 1);
            else emit_inc(t, -1, 1);
            next();
            lv = 0;
        } else break;
    }
    return t;
}

Type *unary(void);

Type *sizeof_expr(void)
{
    int save_c = cpos;
    int save_r = nrelocs;
    int save_s = spos;
    Type *t;
    nocode_depth++;
    t = unary();
    if (pending_fn) pending_fn = 0;
    nocode_depth--;
    cpos = save_c;
    nrelocs = save_r;
    spos = save_s;
    hist_clear();
    lv = 0;
    return t;
}

Type *unary(void)
{
    Type *t;
    int k = tok.kind;
    if (k == '-' || k == '+' || k == '!' || k == '~') {
        next();
        t = unary();
        t = rv(t);
        if (k == '-') {
            if (t->kind == T_FLOAT) emit_op(OP_FNEG);
            else if (is_int(t)) { emit_op(OP_NEG); t = usual(t, ty_int); }
            else error("invalid operand to unary -");
        } else if (k == '+') {
            if (!is_arith(t)) error("invalid operand to unary +");
        } else if (k == '!') {
            if (t->kind == T_FLOAT) { emit_imm(0); emit_op(OP_FEQ); }
            else if (is_scalar(t)) emit_op(OP_LNOT);
            else error("invalid operand to !");
            t = ty_int;
        } else {
            if (!is_int(t)) error("invalid operand to ~");
            emit_op(OP_NOT);
            t = usual(t, ty_int);
        }
        return t;
    }
    if (k == '*') {
        next();
        t = unary();
        t = rv(t);
        if (!is_ptr(t)) error("dereference of a non-pointer");
        t = t->base;
        if (t->kind == T_VOID) error("dereference of void *");
        lv = 1;
        return t;
    }
    if (k == '&') {
        next();
        t = unary();
        if (pending_fn) {
            emit_fn_ref(OP_IMM, pending_fn);
            pending_fn = 0;
            lv = 0;
            return ptr_to(t);
        }
        if (!lv && t->kind != T_ARRAY && t->kind != T_FUNC && t->kind != T_STRUCT) error("cannot take the address of an rvalue");
        lv = 0;
        return ptr_to(t);
    }
    if (k == TK_INC || k == TK_DEC) {
        next();
        t = unary();
        if (!lv) error("lvalue required for ++/--");
        if (k == TK_INC) emit_inc(t, 1, 0);
        else emit_inc(t, -1, 0);
        lv = 0;
        return t;
    }
    if (k == KW_SIZEOF) {
        next();
        if (tok.kind == '(') {
            peek();
            if (peek_tok.kind >= KW_INT && peek_tok.kind <= KW_REGISTER
                || peek_tok.kind == KW_BOOL
                || (peek_tok.kind == TK_ID && peek_tok.id->sym && peek_tok.id->sym->kind == S_TYPEDEF)) {
                next();
                t = type_name();
                expect(')', ")");
                if (t->size <= 0 && t->kind != T_VOID) error("sizeof incomplete type");
                emit_imm(t->size);
                lv = 0;
                return ty_uint;
            }
        }
        t = sizeof_expr();
        if (t->kind == T_FUNC) emit_imm(1);
        else emit_imm(t->size);
        return ty_uint;
    }
    if (k == '(') {
        peek();
        if ((peek_tok.kind >= KW_INT && peek_tok.kind <= KW_REGISTER) || peek_tok.kind == KW_BOOL
            || (peek_tok.kind == TK_ID && peek_tok.id->sym && peek_tok.id->sym->kind == S_TYPEDEF)) {
            Type *to;
            next();
            to = type_name();
            expect(')', ")");
            t = unary();
            t = rv(t);
            if (to->kind == T_STRUCT) error("cast to struct");
            cast_to(t, to);
            lv = 0;
            return to;
        }
    }
    return postfix();
}

int prec(int k)
{
    if (k == '*' || k == '/' || k == '%') return 10;
    if (k == '+' || k == '-') return 9;
    if (k == TK_SHL || k == TK_SHR) return 8;
    if (k == '<' || k == '>' || k == TK_LE || k == TK_GE) return 7;
    if (k == TK_EQ || k == TK_NE) return 6;
    if (k == '&') return 5;
    if (k == '^') return 4;
    if (k == '|') return 3;
    if (k == TK_ANDAND) return 2;
    if (k == TK_OROR) return 1;
    return 0;
}

/* make the top of the stack a 0/1 truth value test-ready */
void to_bool(Type *t)
{
    if (t->kind == T_FLOAT) { emit_imm(0); emit_op(OP_FNE); }
    else if (!is_scalar(t)) error("scalar required");
}

Type *binary(int minp)
{
    Type *l = unary();
    Type *r;
    int k;
    int p;
    int jf;
    int je;
    while (1) {
        k = tok.kind;
        p = prec(k);
        if (p == 0 || p < minp) break;
        next();
        l = rv(l);
        if (k == TK_ANDAND || k == TK_OROR) {
            to_bool(l);
            if (k == TK_ANDAND) jf = emit_jump(OP_JZ, 0);
            else jf = emit_jump(OP_JNZ, 0);
            r = binary(p + 1);
            r = rv(r);
            to_bool(r);
            if (k == TK_ANDAND) jf = emit_jump(OP_JZ, jf);
            else jf = emit_jump(OP_JNZ, jf);
            if (k == TK_ANDAND) emit_imm(1);
            else emit_imm(0);
            je = emit_jump(OP_JMP, 0);
            patch_chain(jf, here());
            if (k == TK_ANDAND) emit_imm(0);
            else emit_imm(1);
            patch_chain(je, here());
            l = ty_int;
            continue;
        }
        r = binary(p + 1);
        r = rv(r);
        l = binop_code(k, l, r);
    }
    return l;
}

Type *cond_expr(void)
{
    Type *t = binary(1);
    Type *t2;
    int jf;
    int je;
    if (tok.kind != '?') return t;
    next();
    t = rv(t);
    to_bool(t);
    jf = emit_jump(OP_JZ, 0);
    t = expr();
    t = rv(t);
    je = emit_jump(OP_JMP, 0);
    expect(':', ":");
    patch_chain(jf, here());
    t2 = cond_expr();
    t2 = rv(t2);
    if (t->kind == T_FLOAT && is_int(t2)) emit_op(OP_ITOF);
    else if (is_int(t) && t2->kind == T_FLOAT) emit_op(OP_FTOI);
    patch_chain(je, here());
    if (is_ptr(t2) && !is_ptr(t)) t = t2;
    lv = 0;
    return t;
}

int assign_op(int k)
{
    if (k == TK_ADDEQ) return '+';
    if (k == TK_SUBEQ) return '-';
    if (k == TK_MULEQ) return '*';
    if (k == TK_DIVEQ) return '/';
    if (k == TK_MODEQ) return '%';
    if (k == TK_ANDEQ) return '&';
    if (k == TK_OREQ) return '|';
    if (k == TK_XOREQ) return '^';
    if (k == TK_SHLEQ) return TK_SHL;
    if (k == TK_SHREQ) return TK_SHR;
    return 0;
}

Type *assign_expr(void)
{
    Type *t = cond_expr();
    Type *r;
    int k = tok.kind;
    int bop;
    if (k == '=') {
        if (!lv && t->kind != T_STRUCT) error("lvalue required for assignment");
        if (t->kind == T_ARRAY) error("cannot assign to an array");
        next();
        r = assign_expr();
        r = rv(r);
        if (t->kind == T_STRUCT) {
            if (r != t) error("incompatible struct assignment");
            op4(OP_MEMCPY, t->size);
            lv = 0;
            return t;
        }
        convert(r, t);
        emit_store(t);
        lv = 0;
        return t;
    }
    bop = assign_op(k);
    if (bop) {
        if (!lv) error("lvalue required for assignment");
        next();
        op(OP_DUP);
        emit_load(t);
        lv = 0;
        r = assign_expr();
        r = rv(r);
        r = binop_code(bop, t, r);
        convert(r, t);
        emit_store(t);
        lv = 0;
        return t;
    }
    return t;
}

Type *expr(void)
{
    Type *t = assign_expr();
    while (tok.kind == ',') {
        next();
        rv(t);
        op(OP_POP);
        t = assign_expr();
    }
    return t;
}

/* an integer constant expression */
int const_expr(void)
{
    int start = cpos;
    int v;
    Type *t;
    nocode_depth++;
    hist_clear();
    t = cond_expr();
    t = rv(t);
    nocode_depth--;
    if (!is_const_since(start) || !is_int(t)) error("constant expression required");
    v = imm_value(start);
    cpos = start;
    hist_clear();
    return v;
}

/* ------------------------------------------------------------------ */
/* statements                                                          */
/* ------------------------------------------------------------------ */

void statement(void);
void local_decl(void);

void expr_stmt(void)
{
    Type *t = expr();
    rv(t);
    op(OP_POP);
    expect(';', ";");
}

void cond_jump(int *chain)
{
    Type *t = expr();
    t = rv(t);
    to_bool(t);
    *chain = emit_jump(OP_JZ, 0);
}

Label *find_label(Ident *name)
{
    Label *l;
    for (l = labels; l; l = l->next)
        if (l->name == name) return l;
    l = (Label *)alloc(sizeof(Label));
    l->name = name;
    l->pc = -1;
    l->next = labels;
    labels = l;
    return l;
}

void block(void)
{
    enter_scope();
    {
        int save = frame_size;
        while (tok.kind != '}') {
            if (tok.kind == TK_EOF) error("unexpected end of file in block");
            if (is_typename()) {
                int st = 0;
                if (tok.kind == TK_ID) {
                    peek();
                    /* 'name:' is a label, 'type name' a declaration */
                    if (peek_tok.kind == ':') st = 1;
                }
                if (st) statement();
                else local_decl();
            } else statement();
        }
        frame_size = save;
    }
    leave_scope();
    next();
}

void statement(void)
{
    int k = tok.kind;
    int jf;
    int je;
    int top;
    int sv_brk;
    int sv_cont;
    int sv_ct;
    int sv_sw;
    Case *sv_cases;
    int sv_def;
    Type *t;
    int tmp;
    Case *c;
    Label *l;
    emit_line();
    if (k == '{') {
        next();
        block();
        return;
    }
    if (k == ';') { next(); return; }
    if (k == KW_IF) {
        next();
        expect('(', "(");
        cond_jump(&jf);
        expect(')', ")");
        statement();
        if (tok.kind == KW_ELSE) {
            next();
            je = emit_jump(OP_JMP, 0);
            patch_chain(jf, here());
            statement();
            patch_chain(je, here());
        } else {
            patch_chain(jf, here());
        }
        return;
    }
    if (k == KW_WHILE) {
        next();
        sv_brk = brk_chain;
        sv_cont = cont_chain;
        sv_ct = cont_target;
        top = here();
        expect('(', "(");
        cond_jump(&jf);
        expect(')', ")");
        brk_chain = jf;
        cont_target = top;
        cont_chain = 0;
        in_loop++;
        statement();
        in_loop--;
        jf = emit_jump(OP_JMP, top);
        patch_chain(brk_chain, here());
        brk_chain = sv_brk;
        cont_chain = sv_cont;
        cont_target = sv_ct;
        return;
    }
    if (k == KW_DO) {
        next();
        sv_brk = brk_chain;
        sv_cont = cont_chain;
        sv_ct = cont_target;
        brk_chain = 0;
        cont_chain = 0;
        cont_target = -1;
        top = here();
        in_loop++;
        statement();
        in_loop--;
        patch_chain(cont_chain, here());
        if (tok.kind != KW_WHILE) error("expected while after do");
        next();
        expect('(', "(");
        t = expr();
        t = rv(t);
        to_bool(t);
        emit_jump(OP_JNZ, top);
        expect(')', ")");
        expect(';', ";");
        patch_chain(brk_chain, here());
        brk_chain = sv_brk;
        cont_chain = sv_cont;
        cont_target = sv_ct;
        return;
    }
    if (k == KW_FOR) {
        int lcond;
        int linc;
        int jbody;
        int save = frame_size;
        next();
        expect('(', "(");
        enter_scope();
        if (tok.kind != ';') {
            if (is_typename()) local_decl();
            else expr_stmt();
        } else next();
        sv_brk = brk_chain;
        sv_cont = cont_chain;
        sv_ct = cont_target;
        brk_chain = 0;
        lcond = here();
        if (tok.kind != ';') {
            cond_jump(&jf);
            brk_chain = jf;
        }
        expect(';', ";");
        jbody = emit_jump(OP_JMP, 0);
        linc = here();
        if (tok.kind != ')') {
            t = expr();
            rv(t);
            op(OP_POP);
        }
        emit_jump(OP_JMP, lcond);
        expect(')', ")");
        patch_chain(jbody, here());
        cont_target = linc;
        cont_chain = 0;
        in_loop++;
        statement();
        in_loop--;
        emit_jump(OP_JMP, linc);
        patch_chain(brk_chain, here());
        brk_chain = sv_brk;
        cont_chain = sv_cont;
        cont_target = sv_ct;
        leave_scope();
        frame_size = save;
        return;
    }
    if (k == KW_SWITCH) {
        next();
        expect('(', "(");
        t = expr();
        t = rv(t);
        if (!is_int(t)) error("switch on a non-integer");
        expect(')', ")");
        tmp = alloc_local(ty_int);
        emit_lea(tmp);
        op(OP_SWAP);
        op(OP_STW);
        op(OP_POP);
        je = emit_jump(OP_JMP, 0);
        sv_brk = brk_chain;
        sv_sw = in_switch;
        sv_cases = cases;
        sv_def = default_pc;
        brk_chain = 0;
        cases = 0;
        default_pc = -1;
        in_switch = 1;
        statement();
        brk_chain = emit_jump(OP_JMP, brk_chain);
        patch_chain(je, here());
        for (c = cases; c; c = c->next) {
            op(OP_LLW);
            out2(tmp);
            emit_imm(c->val);
            op(OP_EQ);
            emit_jump(OP_JNZ, c->pc);
        }
        if (default_pc >= 0) emit_jump(OP_JMP, default_pc);
        patch_chain(brk_chain, here());
        brk_chain = sv_brk;
        in_switch = sv_sw;
        cases = sv_cases;
        default_pc = sv_def;
        return;
    }
    if (k == KW_CASE) {
        next();
        if (!in_switch) error("case outside switch");
        c = (Case *)alloc(sizeof(Case));
        c->val = const_expr();
        expect(':', ":");
        c->pc = here();
        c->next = cases;
        cases = c;
        statement();
        return;
    }
    if (k == KW_DEFAULT) {
        next();
        if (!in_switch) error("default outside switch");
        expect(':', ":");
        default_pc = here();
        statement();
        return;
    }
    if (k == KW_BREAK) {
        next();
        if (!in_loop && !in_switch) error("break outside loop or switch");
        brk_chain = emit_jump(OP_JMP, brk_chain);
        expect(';', ";");
        return;
    }
    if (k == KW_CONTINUE) {
        next();
        if (!in_loop) error("continue outside loop");
        if (cont_target >= 0) emit_jump(OP_JMP, cont_target);
        else cont_chain = emit_jump(OP_JMP, cont_chain);
        expect(';', ";");
        return;
    }
    if (k == KW_RETURN) {
        next();
        if (tok.kind == ';') {
            next();
            op(OP_RET);
            return;
        }
        t = expr();
        t = rv(t);
        if (cur_ret->kind == T_VOID) error("return with a value in a void function");
        convert(t, cur_ret);
        op(OP_RETV);
        expect(';', ";");
        return;
    }
    if (k == KW_GOTO) {
        next();
        if (tok.kind != TK_ID) error("expected label");
        l = find_label(tok.id);
        next();
        if (l->pc >= 0) emit_jump(OP_JMP, l->pc);
        else l->patch = emit_jump(OP_JMP, l->patch);
        expect(';', ";");
        return;
    }
    if (k == TK_ID) {
        peek();
        if (peek_tok.kind == ':') {
            l = find_label(tok.id);
            if (l->pc >= 0) error_s("duplicate label", tok.id->name);
            next();
            next();
            l->pc = here();
            patch_chain(l->patch, l->pc);
            l->patch = 0;
            if (tok.kind == '}') return;
            statement();
            return;
        }
    }
    expr_stmt();
}

/* ------------------------------------------------------------------ */
/* initializers                                                        */
/* ------------------------------------------------------------------ */

/* local initializer: store into the frame at 'off' */
void local_init(Type *t, int off)
{
    Type *r;
    Member *m;
    int i;
    char *s;
    int n;
    if (t->kind == T_ARRAY && t->base->kind == T_CHAR && tok.kind == TK_STR) {
        string_lit(&s, &n);
        if (t->len < 0) error("internal: unsized local array");
        emit_lea(off);
        op4(OP_MEMSET, t->size);
        i = n + 1;
        if (i > t->len) i = t->len;
        emit_lea(off);
        emit_addr(SEG_STR, add_string(s, n));
        op4(OP_MEMCPY, i);
        op(OP_POP);
        return;
    }
    if (t->kind == T_ARRAY || t->kind == T_STRUCT) {
        if (tok.kind != '{') {
            if (t->kind == T_STRUCT) {
                emit_lea(off);
                r = assign_expr();
                r = rv(r);
                if (r != t) error("incompatible struct initializer");
                op4(OP_MEMCPY, t->size);
                op(OP_POP);
                return;
            }
            error("array initializer must be a list");
        }
        next();
        emit_lea(off);
        op4(OP_MEMSET, t->size);
        if (t->kind == T_ARRAY) {
            i = 0;
            while (tok.kind != '}') {
                if (t->len >= 0 && i >= t->len) error("too many initializers");
                local_init(t->base, off + i * t->base->size);
                i++;
                if (!accept(',')) break;
            }
        } else {
            m = t->mem;
            while (tok.kind != '}') {
                if (!m) error("too many initializers");
                local_init(m->type, off + m->off);
                m = m->next;
                if (t->isunion) m = 0;
                if (!accept(',')) break;
            }
        }
        expect('}', "}");
        return;
    }
    if (tok.kind == '{') {
        next();
        local_init(t, off);
        accept(',');
        expect('}', "}");
        return;
    }
    emit_lea(off);
    r = assign_expr();
    r = rv(r);
    convert(r, t);
    emit_store(t);
    op(OP_POP);
}

void chunk_begin(int at)
{
    /* link the previous init chunk to this one */
    put4(init_tail, at);
    in_chunk = 1;
}

void chunk_end(void)
{
    init_tail = emit_jump(OP_JMP, 0);
    in_chunk = 0;
}

void data_put(int off, int v, int size)
{
    if (off + size > dpos) error("internal: data write out of range");
    dataseg[off] = v & 255;
    if (size > 1) dataseg[off + 1] = (v >> 8) & 255;
    if (size > 2) {
        dataseg[off + 2] = (v >> 16) & 255;
        dataseg[off + 3] = (v >> 24) & 255;
    }
}

int static_init;   /* 1: constants only (static locals) */

/* one scalar global initializer at segment offset off */
void global_scalar(Type *t, int seg, int off)
{
    int start = cpos;
    int vstart;
    Type *r;
    int v;
    emit_addr(seg, off);
    vstart = cpos;
    r = assign_expr();
    r = rv(r);
    convert(r, t);
    if (is_const_since(vstart) && seg == SEG_DATA) {
        v = imm_value(vstart);
        cpos = start;
        nrelocs--;
        hist_clear();
        data_put(off, v, t->size);
        return;
    }
    if (static_init) error("initializer is not a constant");
    chunk_begin(start);
    emit_store(t);
    op(OP_POP);
    chunk_end();
}

void global_init(Type *t, int seg, int off)
{
    Member *m;
    int i;
    char *s;
    int n;
    if (t->kind == T_ARRAY && t->base->kind == T_CHAR && tok.kind == TK_STR) {
        string_lit(&s, &n);
        if (seg == SEG_DATA) {
            for (i = 0; i < n && i < t->len; i++) dataseg[off + i] = s[i];
            return;
        }
        if (static_init) error("initializer is not a constant");
        i = n + 1;
        if (i > t->len) i = t->len;
        chunk_begin(cpos);
        emit_addr(seg, off);
        emit_addr(SEG_STR, add_string(s, n));
        op4(OP_MEMCPY, i);
        op(OP_POP);
        chunk_end();
        return;
    }
    if (t->kind == T_ARRAY || t->kind == T_STRUCT) {
        if (tok.kind != '{') error("aggregate initializer must be a list");
        next();
        if (t->kind == T_ARRAY) {
            i = 0;
            while (tok.kind != '}') {
                if (i >= t->len) error("too many initializers");
                global_init(t->base, seg, off + i * t->base->size);
                i++;
                if (!accept(',')) break;
            }
        } else {
            m = t->mem;
            while (tok.kind != '}') {
                if (!m) error("too many initializers");
                global_init(m->type, seg, off + m->off);
                m = m->next;
                if (t->isunion) m = 0;
                if (!accept(',')) break;
            }
        }
        expect('}', "}");
        return;
    }
    if (tok.kind == '{') {
        next();
        global_init(t, seg, off);
        accept(',');
        expect('}', "}");
        return;
    }
    global_scalar(t, seg, off);
}

/* For 'T a[] = ...' the length comes from the initializer. The tokens are
 * read ahead into a buffer to count the elements, then replayed. */
int unsized_len(Type *t)
{
    Tok *buf = 0;
    int cap = 0;
    int n = 0;
    int lvl = 0;
    int count = 0;
    int i;
    Tok cur;
    if (tok.kind == TK_STR && t->base->kind == T_CHAR) {
        /* concatenated string literal: peek its full length */
        int len = 0;
        while (tok.kind == TK_STR) {
            buf = mbuf_grow(buf, &cap, n + 1);
            buf[n] = tok;
            n++;
            len = len + tok.len;
            next();
        }
        buf = mbuf_grow(buf, &cap, n + 1);
        buf[n] = tok;
        n++;
        push_toks(buf, n, 0, 0);
        src->owned = 1;
        next();
        return len + 1;
    }
    if (tok.kind != '{') error("array initializer must be a list");
    while (1) {
        buf = mbuf_grow(buf, &cap, n + 1);
        buf[n] = tok;
        n++;
        if (tok.kind == '{' || tok.kind == '(' || tok.kind == '[') {
            lvl++;
            if (lvl == 1) count = 0;
        } else if (tok.kind == '}' || tok.kind == ')' || tok.kind == ']') {
            lvl--;
            if (lvl == 0) {
                if (n >= 2 && buf[n - 2].kind != ',' && buf[n - 2].kind != '{') count++;
                break;
            }
        } else if (tok.kind == ',' && lvl == 1) {
            count++;
        } else if (tok.kind == TK_EOF) error("unterminated initializer");
        next();
    }
    next();
    buf = mbuf_grow(buf, &cap, n + 1);
    buf[n] = tok;
    n++;
    push_toks(buf, n, 0, 0);
    src->owned = 1;
    next();
    return count;
}

/* ------------------------------------------------------------------ */
/* declarations                                                        */
/* ------------------------------------------------------------------ */

void local_decl(void)
{
    Type *base = declspec();
    Type *t;
    Ident *name;
    Sym *s;
    int seg;
    int sv;
    if (accept(';')) return;
    while (1) {
        name = 0;
        t = declarator(base, &name);
        if (!name) error("expected variable name");
        if (st_typedef) {
            s = add_sym(name, S_TYPEDEF, t);
        } else if (t->kind == T_FUNC) {
            s = name->sym;
            while (s && s->depth > 0) s = s->shadow;
            if (!s) add_global_sym(name, S_FUNC, t);
        } else if (st_static || st_extern) {
            if (st_extern) {
                Sym *g = name->sym;
                while (g && g->depth > 0) g = g->shadow;
                if (!g) error_s("extern variable not declared at file scope", name->name);
                s = add_sym(name, S_GLOBAL, g->type);
                s->seg = g->seg;
                s->addr = g->addr;
            } else {
                if (t->kind == T_ARRAY && t->len < 0 && tok.kind == '=') {
                    next();
                    t = array_of(t->base, unsized_len(t));
                    unread(&tok);
                    tok.kind = '=';
                }
                s = add_sym(name, S_GLOBAL, t);
                s->addr = alloc_global(t, tok.kind == '=', &seg);
                s->seg = seg;
                if (accept('=')) {
                    sv = static_init;
                    static_init = 1;
                    global_init(t, seg, s->addr);
                    static_init = sv;
                }
            }
        } else {
            if (t->kind == T_ARRAY && t->len < 0) {
                if (tok.kind != '=') error("array size missing");
                next();
                t = array_of(t->base, unsized_len(t));
                unread(&tok);
                tok.kind = '=';
            }
            if (t->kind == T_VOID) error("variable of type void");
            s = add_sym(name, S_LOCAL, t);
            s->addr = alloc_local(t);
            if (accept('=')) local_init(t, s->addr);
        }
        if (!accept(',')) break;
    }
    expect(';', ";");
}

void func_body(Sym *fs, Type *ft)
{
    Param *p;
    Sym *s;
    int enter_at;
    int off;
    int loc;
    Label *l;
    int namelen;
    if (fs->defined) error_s("redefinition of function", fs->name->name);
    fs->addr = here();
    fs->defined = 1;
    patch_chain(fs->patch, fs->addr);
    fs->patch = 0;
    /* function table entry for error messages */
    namelen = fs->name->len;
    if (namelen > 255) namelen = 255;
    if (fpos + namelen + 5 > fcap) ftab = grow(ftab, &fcap, fpos + namelen + 5);
    ftab[fpos] = fs->addr & 255;
    ftab[fpos + 1] = (fs->addr >> 8) & 255;
    ftab[fpos + 2] = (fs->addr >> 16) & 255;
    ftab[fpos + 3] = (fs->addr >> 24) & 255;
    ftab[fpos + 4] = namelen;
    memcpy(ftab + fpos + 5, fs->name->name, namelen);
    fpos = fpos + namelen + 5;
    nfuncs++;

    cur_fn = fs;
    cur_ret = ft->base;
    labels = 0;
    frame_size = 0;
    frame_max = 0;
    enter_scope();
    /* parameters occupy the first cells of the frame */
    for (p = ft->params; p; p = p->next) {
        off = frame_size;
        frame_size = frame_size + 4;
        if (p->name) {
            s = add_sym(p->name, S_LOCAL, p->type);
            s->addr = off;
        }
    }
    if (frame_size > frame_max) frame_max = frame_size;
    hist_clear();
    out1(OP_ENTER);
    out1(ft->nparams);
    enter_at = cpos;
    out4(0);
    /* struct parameters arrive as pointers: copy into the frame */
    for (p = ft->params; p; p = p->next) {
        if (p->type->kind == T_STRUCT && p->name) {
            s = p->name->sym;
            loc = alloc_local(p->type);
            emit_lea(loc);
            op(OP_LLW);
            out2(s->addr);
            op4(OP_MEMCPY, p->type->size);
            op(OP_POP);
            s->addr = loc;
        }
    }
    expect('{', "{");
    block();
    op(OP_RET);
    put4(enter_at, frame_max);
    for (l = labels; l; l = l->next)
        if (l->pc < 0) error_s("undefined label", l->name->name);
    leave_scope();
    cur_fn = 0;
}

void top_decl(void)
{
    Type *base;
    Type *t;
    Ident *name;
    Sym *s;
    int seg;
    if (tok.kind == ';') { next(); return; }
    base = declspec();
    if (accept(';')) return;
    while (1) {
        name = 0;
        t = declarator(base, &name);
        if (!name) error("expected a name");
        if (st_typedef) {
            add_sym(name, S_TYPEDEF, t);
        } else if (t->kind == T_FUNC) {
            s = name->sym;
            if (s && s->kind != S_FUNC) error_s("redeclared as a function", name->name);
            if (!s) s = add_sym(name, S_FUNC, t);
            else if (s->sys && tok.kind == '{') {
                /* a program may define its own version of a builtin */
                s->sys = 0;
                s->defined = 0;
                s->type = t;
            } else if (t->proto) s->type = t;
            if (tok.kind == TK_ID && tok.id == id_sys) {
                next();
                expect('(', "(");
                s->sys = const_expr();
                s->defined = 1;
                expect(')', ")");
            }
            if (tok.kind == '{') {
                if (s->sys) error("builtin function with a body");
                func_body(s, t);
                return;
            }
        } else {
            if (t->kind == T_ARRAY && t->len < 0 && tok.kind == '=') {
                next();
                t = array_of(t->base, unsized_len(t));
                unread(&tok);
                tok.kind = '=';
            }
            s = name->sym;
            if (s && s->depth == 0 && s->kind == S_GLOBAL) {
                /* a redeclaration (extern, tentative definition) */
                if (tok.kind == '=' && s->defined) error_s("redefinition of", name->name);
                if (t->kind == T_ARRAY && s->type->kind == T_ARRAY && s->type->len < 0 && t->len >= 0) {
                    s->type = t;
                    s->addr = alloc_global(t, 0, &seg);
                    s->seg = seg;
                }
            } else {
                if (t->kind == T_VOID) error("variable of type void");
                s = add_sym(name, S_GLOBAL, t);
                if (st_extern && t->kind == T_ARRAY && t->len < 0) {
                    s->seg = SEG_BSS;
                    s->addr = 0;
                } else {
                    s->addr = alloc_global(t, tok.kind == '=', &seg);
                    s->seg = seg;
                }
            }
            if (accept('=')) {
                global_init(t, s->seg, s->addr);
                s->defined = 1;
            }
        }
        if (!accept(',')) break;
    }
    expect(';', ";");
}

/* ------------------------------------------------------------------ */
/* driver                                                              */
/* ------------------------------------------------------------------ */

void init_types(void)
{
    ty_void = new_type(T_VOID, 0, 1);
    ty_char = new_type(T_CHAR, 1, 1);
    ty_uchar = new_type(T_CHAR, 1, 1);
    ty_uchar->uns = 1;
    ty_short = new_type(T_SHORT, 2, 2);
    ty_ushort = new_type(T_SHORT, 2, 2);
    ty_ushort->uns = 1;
    ty_int = new_type(T_INT, 4, 4);
    ty_uint = new_type(T_INT, 4, 4);
    ty_uint->uns = 1;
    ty_float = new_type(T_FLOAT, 4, 4);
    ty_charp = ptr_to(ty_char);
}

void init_keywords(void)
{
    keyword("int", KW_INT);
    keyword("char", KW_CHAR);
    keyword("short", KW_SHORT);
    keyword("long", KW_LONG);
    keyword("signed", KW_SIGNED);
    keyword("unsigned", KW_UNSIGNED);
    keyword("void", KW_VOID);
    keyword("float", KW_FLOAT);
    keyword("double", KW_DOUBLE);
    keyword("struct", KW_STRUCT);
    keyword("union", KW_UNION);
    keyword("enum", KW_ENUM);
    keyword("typedef", KW_TYPEDEF);
    keyword("static", KW_STATIC);
    keyword("extern", KW_EXTERN);
    keyword("const", KW_CONST);
    keyword("volatile", KW_VOLATILE);
    keyword("auto", KW_AUTO);
    keyword("register", KW_REGISTER);
    keyword("if", KW_IF);
    keyword("else", KW_ELSE);
    keyword("while", KW_WHILE);
    keyword("do", KW_DO);
    keyword("for", KW_FOR);
    keyword("switch", KW_SWITCH);
    keyword("case", KW_CASE);
    keyword("default", KW_DEFAULT);
    keyword("break", KW_BREAK);
    keyword("continue", KW_CONTINUE);
    keyword("return", KW_RETURN);
    keyword("goto", KW_GOTO);
    keyword("sizeof", KW_SIZEOF);
    keyword("inline", KW_INLINE);
    keyword("_Bool", KW_BOOL);
    id_sys = intern("__sys", 5);
    id_main = intern("main", 4);
    id_defined = intern("defined", 7);
    id_line = intern("__LINE__", 8);
}

void w4(FILE *f, int v)
{
    char b[4];
    b[0] = v & 255;
    b[1] = (v >> 8) & 255;
    b[2] = (v >> 16) & 255;
    b[3] = (v >> 24) & 255;
    fwrite(b, 1, 4, f);
}

int write_output(char *path, int entry)
{
    FILE *f = fopen(path, "wb");
    int i;
    int n;
    int lsize;
    if (!f) {
        printf("cc: cannot write %s\n", path);
        return 0;
    }
    lsize = 0;
    for (i = 0; i < nfiles; i++) lsize = lsize + strlen(fnames[i]) + 1;
    w4(f, CBC_MAGIC);
    w4(f, CBC_VERSION);
    w4(f, cpos);
    w4(f, spos + dpos);        /* initialised data image */
    w4(f, spos + dpos + bss);  /* whole data segment */
    w4(f, entry);
    w4(f, nlines);
    w4(f, nfuncs);
    w4(f, fpos);
    w4(f, nfiles);
    w4(f, lsize);
    w4(f, 0);
    fwrite(code, 1, cpos, f);
    if (spos) fwrite(strseg, 1, spos, f);
    if (dpos) fwrite(dataseg, 1, dpos, f);
    for (i = 0; i < nlines; i++) {
        w4(f, lines[i * 2]);
        w4(f, lines[i * 2 + 1]);
    }
    if (fpos) fwrite(ftab, 1, fpos, f);
    for (i = 0; i < nfiles; i++) {
        n = strlen(fnames[i]);
        fwrite(fnames[i], 1, n + 1, f);
    }
    fclose(f);
    return 1;
}

void usage(void)
{
    printf("usage: cc file.c [-o out.cbc] [-v]\n");
    exit(2);
}

char outname[160];

/* Natively the compiler runs inside the firmware more than once, so every
 * global has to start from zero again. Under the VM this is a no-op. */
void reset_globals(void)
{
    arena_p = 0; arena_left = 0; arena_total = 0;
    memset(idtab, 0, sizeof(idtab));
    src = 0; src_free = 0; sym_free = 0; stop_nl = 0; in_if = 0; npushed = 0;
    memset(&tok, 0, sizeof(tok));
    conddepth = 0; nfiles = 0;
    code = 0; cpos = 0; ccap = 0;
    strseg = 0; spos = 0; scap = 0;
    dataseg = 0; dpos = 0; dcap = 0; bss = 0;
    relocs = 0; nrelocs = 0; rcap = 0;
    lines = 0; nlines = 0; lcap = 0; last_line = 0; last_lfile = 0;
    ftab = 0; fpos = 0; fcap = 0; nfuncs = 0;
    nhist = 0; nocode_depth = 0; last_was_lea = -1;
    lv = 0; pending_fn = 0; depth = 0;
    memset(scope_syms, 0, sizeof(scope_syms));
    memset(scope_tags, 0, sizeof(scope_tags));
    frame_size = 0; frame_max = 0; cur_ret = 0; cur_fn = 0; labels = 0;
    brk_chain = 0; cont_chain = 0; cont_target = -1; in_loop = 0; in_switch = 0;
    cases = 0; default_pc = 0; ret_tmp = 0; init_tail = 0; in_chunk = 0;
    errors = 0; warnings = 0; verbose = 0;
    sbuf = 0; scap2 = 0; catbuf = 0; catcap = 0; ifl = 0; nifl = 0; ifp = 0;
    st_typedef = 0; st_static = 0; st_extern = 0; static_init = 0;
    outname[0] = 0;
}

int main(int argc, char **argv)
{
    char *in = 0;
    char *out = 0;
    int i;
    int n;
    int entry;
    Sym *ms;
    int base_str;
    int base_data;
    int base_bss;
    int seg;
    int pos;
    reset_globals();
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { i++; out = argv[i]; }
        else if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (argv[i][0] == '-') usage();
        else in = argv[i];
    }
    if (!in) usage();
    if (!out) {
        n = strlen(in);
        if (n > 150) n = 150;
        memcpy(outname, in, n);
        outname[n] = 0;
        if (n > 2 && outname[n - 2] == '.' && (outname[n - 1] == 'c' || outname[n - 1] == 'C')) outname[n - 2] = 0;
        strcat(outname, ".cbc");
        out = outname;
    }

    ccap = 4096;
    code = malloc(ccap);
    scap = 1024;
    strseg = malloc(scap);
    dcap = 1024;
    dataseg = malloc(dcap);
    rcap = 256;
    relocs = (int *)malloc(rcap * 4);
    lcap = 512;
    lines = (int *)malloc(lcap * 4);
    fcap = 512;
    ftab = malloc(fcap);
    scap2 = 256;
    sbuf = malloc(scap2);
    if (!code || !strseg || !dataseg || !relocs || !lines || !ftab || !sbuf) error("out of memory");
    last_line = -1;
    cont_target = -1;

    init_keywords();
    init_types();

    /* entry: jump into the chain of global initialisers, which ends in main */
    hist_clear();
    out1(OP_JMP);
    init_tail = cpos;
    out4(0);

    if (!open_file(in)) {
        printf("cc: cannot open %s\n", in);
        exit(1);
    }
    open_mem(cc_prelude, "<builtin>");
    next();
    while (tok.kind != TK_EOF) top_decl();

    ms = id_main->sym;
    if (!ms || ms->kind != S_FUNC || !ms->defined) error("no main function");
    entry = here();
    put4(init_tail, entry);
    hist_clear();
    out1(OP_CALL);
    out4(ms->addr);
    op(OP_HALT);

    /* every function that was called must have been defined */
    for (i = 0; i < 512; i++) {
        Ident *id;
        for (id = idtab[i]; id; id = id->next) {
            Sym *s = id->sym;
            while (s) {
                if (s->kind == S_FUNC && !s->defined && s->patch) {
                    printf("cc: error: undefined function '%s'\n", id->name);
                    exit(1);
                }
                s = s->shadow;
            }
        }
    }

    /* relocate: strings, then initialised data, then bss */
    base_str = CBC_DATA_BASE;
    base_data = (base_str + spos + 3) & ~3;
    /* pad the string area so data stays aligned */
    while (CBC_DATA_BASE + spos < base_data) {
        if (spos + 1 > scap) strseg = grow(strseg, &scap, spos + 1);
        strseg[spos] = 0;
        spos++;
    }
    base_bss = (base_data + dpos + 3) & ~3;
    while (base_data + dpos < base_bss) {
        if (dpos + 1 > dcap) dataseg = grow(dataseg, &dcap, dpos + 1);
        dataseg[dpos] = 0;
        dpos++;
    }
    for (i = 0; i < nrelocs; i++) {
        pos = relocs[i] >> 2;
        seg = relocs[i] & 3;
        if (seg == SEG_STR) put4(pos, get4(pos) + base_str);
        else if (seg == SEG_DATA) put4(pos, get4(pos) + base_data);
        else put4(pos, get4(pos) + base_bss);
    }
    bss = (bss + 3) & ~3;

    /* execution starts at 0: the jump into the initialiser chain */
    if (!write_output(out, 0)) exit(1);
    if (verbose) {
        printf("cc: %s -> %s\n", in, out);
        printf("    code %d, data %d, bss %d, %d functions, %d KB compiler memory\n",
               cpos, spos + dpos, bss, nfuncs, arena_total / 1024);
    } else {
        printf("%s: %d bytes code, %d data, %d bss\n", out, cpos, spos + dpos, bss);
    }
    return 0;
}
