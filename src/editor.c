/* editor.c - the CalcIDE full-screen text editor.
 *
 * The text lives in one gap buffer, so typing is O(1) and the only
 * allocation is the buffer itself, which is released on exit. Screen:
 *
 *   row 0      key hints
 *   rows 1-30  text, with an optional line-number gutter
 *   row 31     status: file name, modified flag, line and column, free KB
 *
 * Keys
 *   arrows, Home/End, PgUp/PgDn      move          Ctrl+G   go to line
 *   Ctrl+F  find   F3 / Ctrl+N  find next          Ctrl+S   save
 *   Ctrl+X  cut line   Ctrl+C  copy line   Ctrl+V  paste line(s)
 *   Ctrl+D  duplicate line   Ctrl+K  delete line   Ctrl+Z  undo last cut
 *   F9 / Ctrl+B  compile        F5 / Ctrl+R  compile and run
 *   Ctrl+O  open another file   F1  help   Esc / Ctrl+Q  quit
 */
#include "editor.h"
#include "console.h"
#include "config.h"
#include "vfs.h"
#include "hal.h"
#include "shell.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>

#define TOP_ROW   1
#define TEXT_ROWS 30
#define STAT_ROW  31

typedef struct {
    char *buf;
    int cap, gs, ge;          /* gap [gs, ge) */
    int pos;                  /* cursor, logical offset */
    int top;                  /* offset of the first visible line */
    int top_line;             /* its line number (0-based) */
    int left;                 /* horizontal scroll, in visual columns */
    int want_col;             /* remembered column for up/down */
    int modified;
    char path[VFS_PATH];
    char msg[64];
    char find[40];
    int match_pos, match_len;
} Ed;

/* Several files can be open at once, one per tab (max_files in the
 * settings). E is always the tab being edited. */
#define MAX_TABS 8
static Ed tabs[MAX_TABS];
static int ntabs, cur;
static int open_tab(const char *full);
#define E (tabs[cur])

static char *clip;           /* shared by all tabs: copy between files */
static int clip_len;
static int ed_quit;

/* ------------------------------------------------------------------ */
/* gap buffer                                                          */
/* ------------------------------------------------------------------ */

static int len(void) { return E.cap - (E.ge - E.gs); }
static int ch(int i) { return (unsigned char)E.buf[i < E.gs ? i : i + (E.ge - E.gs)]; }

static void move_gap(int p)
{
    if (p < E.gs) {
        int n = E.gs - p;
        memmove(E.buf + E.ge - n, E.buf + p, (size_t)n);
        E.gs -= n;
        E.ge -= n;
    } else if (p > E.gs) {
        int n = p - E.gs;
        memmove(E.buf + E.gs, E.buf + E.ge, (size_t)n);
        E.gs += n;
        E.ge += n;
    }
}

static int reserve(int n)
{
    if (E.ge - E.gs >= n) return 1;
    {
        int grow = n + 4096 + E.cap / 4;
        char *nb = (char *)realloc(E.buf, (size_t)(E.cap + grow));
        int tail = E.cap - E.ge;
        if (!nb) { snprintf(E.msg, sizeof E.msg, "Out of memory!"); return 0; }
        memmove(nb + E.ge + grow, nb + E.ge, (size_t)tail);
        E.buf = nb;
        E.ge += grow;
        E.cap += grow;
    }
    return 1;
}

static int ins(int p, const char *s, int n)
{
    if (n <= 0) return 1;
    if (!reserve(n)) return 0;
    move_gap(p);
    memcpy(E.buf + E.gs, s, (size_t)n);
    E.gs += n;
    E.modified = 1;
    return 1;
}

static void del(int p, int n)
{
    if (p < 0) { n += p; p = 0; }
    if (p + n > len()) n = len() - p;
    if (n <= 0) return;
    move_gap(p);
    E.ge += n;
    E.modified = 1;
}

/* ------------------------------------------------------------------ */
/* lines                                                               */
/* ------------------------------------------------------------------ */

static int line_start(int p) { while (p > 0 && ch(p - 1) != '\n') p--; return p; }
static int line_end(int p) { int n = len(); while (p < n && ch(p) != '\n') p++; return p; }
static int next_line(int p) { int e = line_end(p); return e < len() ? e + 1 : -1; }

static int prev_line(int p)
{
    int s = line_start(p);
    if (s == 0) return -1;
    return line_start(s - 1);
}

static int line_of(int p)
{
    int i, n = 0, from = 0;
    if (p >= E.top) { from = E.top; n = E.top_line; }
    for (i = from; i < p; i++) if (ch(i) == '\n') n++;
    return n;
}

static int line_count(void)
{
    int i, n = 1, l = len();
    for (i = 0; i < l; i++) if (ch(i) == '\n') n++;
    return n;
}

static int pos_of_line(int ln)
{
    int p = 0, cur = 0;
    while (cur < ln) {
        int nx = next_line(p);
        if (nx < 0) break;
        p = nx;
        cur++;
    }
    return p;
}

/* visual column of position p within its line */
static int vcol(int p)
{
    int s = line_start(p), c = 0, ts = g_conf.tabsize > 0 ? g_conf.tabsize : 4;
    for (; s < p; s++) c = ch(s) == '\t' ? (c / ts + 1) * ts : c + 1;
    return c;
}

static int pos_at_vcol(int ls, int col)
{
    int p = ls, c = 0, e = line_end(ls), ts = g_conf.tabsize > 0 ? g_conf.tabsize : 4;
    while (p < e) {
        int w = ch(p) == '\t' ? (c / ts + 1) * ts - c : 1;
        if (c + w > col) break;
        c += w;
        p++;
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* drawing                                                             */
/* ------------------------------------------------------------------ */

static const char *kw_ctrl[] = {
    "if", "else", "while", "for", "do", "switch", "case", "default", "break",
    "continue", "return", "goto", "sizeof", 0
};
static const char *kw_type[] = {
    "int", "char", "short", "long", "unsigned", "signed", "void", "float",
    "double", "struct", "union", "enum", "typedef", "static", "const", "extern",
    "volatile", "register", "auto", "inline", "FILE", "size_t", "bool", "_Bool", 0
};

static int in_list(const char **l, const char *w, int n)
{
    int i;
    for (i = 0; l[i]; i++) if ((int)strlen(l[i]) == n && memcmp(l[i], w, (size_t)n) == 0) return 1;
    return 0;
}

static int gutter(void) { return g_conf.line_numbers ? (con_cols() == 64 ? 5 : 4) : 0; }
static int text_cols(void) { return con_cols() - gutter(); }

/* Colour one line. 'incom' carries a block comment across lines. */
static void color_line(int s, int e, uint8_t *attrs, int *incom)
{
    int i = s, n = e - s, k;
    int base = ATTR(g_conf.ed_fg, g_conf.ed_bg);
    int bg = g_conf.ed_bg;
    char word[24];
    for (k = 0; k < n; k++) attrs[k] = (uint8_t)base;
    if (!g_conf.syntax) return;
    {
        /* preprocessor line? */
        int j = s;
        while (j < e && (ch(j) == ' ' || ch(j) == '\t')) j++;
        if (!*incom && j < e && ch(j) == '#' && !(j + 1 < e && ch(j + 1) == '#')) {
            for (k = j - s; k < n; k++) attrs[k] = (uint8_t)ATTR(g_conf.ed_pre, bg);
            /* comments still win */
            for (i = j; i < e; i++) {
                if (ch(i) == '/' && i + 1 < e && ch(i + 1) == '/') { for (k = i - s; k < n; k++) attrs[k] = (uint8_t)ATTR(g_conf.ed_com, bg); break; }
                if (ch(i) == '/' && i + 1 < e && ch(i + 1) == '*') {
                    int z = i + 2;
                    while (z + 1 < e && !(ch(z) == '*' && ch(z + 1) == '/')) z++;
                    if (z + 1 >= e) { *incom = 1; z = e; } else z += 2;
                    for (k = i - s; k < z - s; k++) attrs[k] = (uint8_t)ATTR(g_conf.ed_com, bg);
                    i = z - 1;
                }
            }
            return;
        }
    }
    while (i < e) {
        int c = ch(i);
        if (*incom) {
            int st = i;
            while (i < e && !(ch(i) == '*' && i + 1 < e && ch(i + 1) == '/')) i++;
            if (i < e) { i += 2; *incom = 0; }
            for (k = st - s; k < i - s && k < n; k++) attrs[k] = (uint8_t)ATTR(g_conf.ed_com, bg);
            continue;
        }
        if ((c == '/' && i + 1 < e && ch(i + 1) == '/') || (c == '#' && i + 1 < e && ch(i + 1) == '#')) {
            for (k = i - s; k < n; k++) attrs[k] = (uint8_t)ATTR(g_conf.ed_com, bg);
            return;
        }
        if (c == '/' && i + 1 < e && ch(i + 1) == '*') { *incom = 1; continue; }
        if (c == '"' || c == '\'') {
            int st = i;
            i++;
            while (i < e && ch(i) != c) { if (ch(i) == '\\') i++; i++; }
            if (i < e) i++;
            for (k = st - s; k < i - s && k < n; k++) attrs[k] = (uint8_t)ATTR(g_conf.ed_str, bg);
            continue;
        }
        if (isdigit(c) || (c == '.' && i + 1 < e && isdigit(ch(i + 1)))) {
            int st = i;
            if (i > s && (isalnum(ch(i - 1)) || ch(i - 1) == '_')) { i++; continue; }
            while (i < e && (isalnum(ch(i)) || ch(i) == '.')) i++;
            for (k = st - s; k < i - s; k++) attrs[k] = (uint8_t)ATTR(g_conf.ed_num, bg);
            continue;
        }
        if (isalpha(c) || c == '_') {
            int st = i, wl = 0;
            while (i < e && (isalnum(ch(i)) || ch(i) == '_')) { if (wl < 23) word[wl++] = (char)ch(i); i++; }
            if (i - st <= 23) {
                int a = -1;
                if (in_list(kw_ctrl, word, wl)) a = ATTR(g_conf.ed_kw, bg);
                else if (in_list(kw_type, word, wl)) a = ATTR(g_conf.ed_type, bg);
                if (a >= 0) for (k = st - s; k < i - s; k++) attrs[k] = (uint8_t)a;
            }
            continue;
        }
        i++;
    }
}

static int comment_state_at(int p)
{
    /* is position p (a line start) inside a block comment? */
    int i = 0, incom = 0, instr = 0, linecom = 0;
    while (i < p) {
        int c = ch(i);
        if (c == '\n') { linecom = 0; instr = 0; i++; continue; }
        if (linecom) { i++; continue; }
        if (incom) { if (c == '*' && i + 1 < p && ch(i + 1) == '/') { incom = 0; i += 2; continue; } i++; continue; }
        if (instr) { if (c == '\\') i++; else if (c == instr) instr = 0; i++; continue; }
        if (c == '"' || c == '\'') { instr = c; i++; continue; }
        if (c == '/' && i + 1 < p && ch(i + 1) == '/') { linecom = 1; i += 2; continue; }
        if (c == '#' && i + 1 < p && ch(i + 1) == '#') { linecom = 1; i += 2; continue; }
        if (c == '/' && i + 1 < p && ch(i + 1) == '*') { incom = 1; i += 2; continue; }
        i++;
    }
    return incom;
}

static void scroll_to_cursor(void)
{
    int cl = line_of(E.pos), vc, tc;
    while (cl < E.top_line) { E.top = prev_line(E.top); E.top_line--; }
    while (cl >= E.top_line + TEXT_ROWS) { E.top = next_line(E.top); E.top_line++; }
    vc = vcol(E.pos);
    tc = text_cols();
    if (vc < E.left) E.left = vc - tc / 3 < 0 ? 0 : vc - tc / 3;
    if (vc >= E.left + tc) E.left = vc - tc + tc / 3;
}

static void draw_status(void)
{
    char s[80], name[40];
    int cols = con_cols(), a = ATTR(g_conf.ed_status_fg, g_conf.ed_status_bg);
    int ln = line_of(E.pos) + 1, col = vcol(E.pos) + 1;
    unsigned kb = (unsigned)(hal_ram_free() / 1024);
    const char *b = E.path[0] ? vfs_basename(E.path) : "untitled";
    snprintf(name, sizeof name, "%s%s", b, E.modified ? "*" : "");
    if (E.msg[0]) snprintf(s, sizeof s, " %s", E.msg);
    else if (cols >= 64) snprintf(s, sizeof s, " %-22.22s Ln %-5d Col %-4d %6uKB free", name, ln, col, kb);
    else snprintf(s, sizeof s, " %-14.14s L%-4d C%-3d %4uK", name, ln, col, kb);
    con_fill_rect(0, STAT_ROW, cols, 1, ' ', a);
    con_text_at(0, STAT_ROW, s, a, cols);
}

static void draw_hints(void)
{
    int cols = con_cols(), a = ATTR(g_conf.ed_status_fg, g_conf.ed_status_bg);
    const char *h = cols >= 64
        ? " ^S Save ^F Find ^G Goto ^X/^C/^V Line F9 Build F5 Run F1 Help ^Q Quit"
        : " ^S Save ^F Find F5 Run F1 Help ^Q Quit";
    con_fill_rect(0, 0, cols, 1, ' ', a);
    if (ntabs <= 1) {
        con_text_at(0, 0, h, a, cols);
        return;
    }
    {
        /* a tab strip: start far enough left that the current tab shows */
        char label[40];
        int i, x = 0, first = 0, w[MAX_TABS];
        for (i = 0; i < ntabs; i++) {
            snprintf(label, sizeof label, " %d:%s%s ", i + 1,
                     tabs[i].path[0] ? vfs_basename(tabs[i].path) : "new", tabs[i].modified ? "*" : "");
            w[i] = (int)strlen(label);
        }
        for (;;) {
            int need = 0;
            for (i = first; i <= cur; i++) need += w[i];
            if (need <= cols || first == cur) break;
            first++;
        }
        for (i = first; i < ntabs && x < cols; i++) {
            int ta = i == cur ? ATTR(g_conf.ed_fg, g_conf.ed_bg) : a;
            snprintf(label, sizeof label, " %d:%s%s ", i + 1,
                     tabs[i].path[0] ? vfs_basename(tabs[i].path) : "new", tabs[i].modified ? "*" : "");
            con_text_at(x, 0, label, ta, cols - x);
            x += w[i];
        }
    }
}

static void draw(void)
{
    static uint8_t attrs[1024];
    int row, p = E.top, incom, g = gutter(), tc = text_cols();
    int base = ATTR(g_conf.ed_fg, g_conf.ed_bg);
    int ts = g_conf.tabsize > 0 ? g_conf.tabsize : 4;
    int cur_row = -1, cur_x = 0;
    scroll_to_cursor();
    p = E.top;
    incom = g_conf.syntax ? comment_state_at(E.top) : 0;
    draw_hints();
    for (row = 0; row < TEXT_ROWS; row++) {
        int y = TOP_ROW + row;
        if (g) {
            char num[16];
            if (p >= 0) snprintf(num, sizeof num, "%*d ", g - 1, E.top_line + row + 1);
            else snprintf(num, sizeof num, "%*s ", g - 1, "~");
            con_text_at(0, y, num, ATTR(g_conf.ed_lnum, g_conf.ed_bg), g);
        }
        if (p < 0) {
            con_fill_rect(g, y, tc, 1, ' ', base);
            continue;
        }
        {
            int e = line_end(p), n = e - p, i, vc = 0, x;
            if (n > (int)sizeof attrs) n = (int)sizeof attrs;
            color_line(p, p + n, attrs, &incom);
            if (e > p + n) { /* very long line: keep the comment state sane */ }
            for (x = 0; x < tc; x++) con_put_at(g + x, y, ' ', base);
            for (i = 0; i <= n; i++) {
                int c = i < n ? ch(p + i) : ' ', a = i < n ? attrs[i] : base, w = 1, k;
                if (E.match_len && p + i >= E.match_pos && p + i < E.match_pos + E.match_len)
                    a = ATTR(g_conf.ed_fg, g_conf.ed_sel);
                if (p + i == E.pos) { cur_row = y; cur_x = vc - E.left; }
                if (i == n) break;
                if (c == '\t') { w = (vc / ts + 1) * ts - vc; c = ' '; }
                for (k = 0; k < w; k++) {
                    int sx = vc + k - E.left;
                    if (sx >= 0 && sx < tc) con_put_at(g + sx, y, c < 32 ? '?' : c, a);
                }
                vc += w;
            }
            if (E.left > 0) con_put_at(g, y, '<', ATTR(g_conf.ed_lnum, g_conf.ed_bg));
            if (vc - E.left > tc) con_put_at(g + tc - 1, y, '>', ATTR(g_conf.ed_lnum, g_conf.ed_bg));
            if (e > p + n) incom = 0;
        }
        p = next_line(p);
    }
    draw_status();
    if (cur_row >= 0) con_place_cursor(g + cur_x, cur_row);
    else con_place_cursor(-1, -1);
}

/* ------------------------------------------------------------------ */
/* prompts                                                             */
/* ------------------------------------------------------------------ */

/* a one-line prompt in the status row; returns length or -1 on Esc */
static int prompt(const char *q, char *out, int cap)
{
    int n = (int)strlen(out), k, cols = con_cols();
    int a = ATTR(g_conf.ed_status_fg, g_conf.ed_status_bg);
    for (;;) {
        char line[96];
        int ql;
        snprintf(line, sizeof line, " %s%s", q, out);
        ql = (int)strlen(line);
        con_fill_rect(0, STAT_ROW, cols, 1, ' ', a);
        con_text_at(0, STAT_ROW, ql > cols - 1 ? line + (ql - cols + 1) : line, a, cols);
        con_place_cursor(ql > cols - 1 ? cols - 1 : ql, STAT_ROW);
        k = con_getkey();
        if (k == KEY_ESC || k == KEY_CTRL('c') || k == KEY_BREAK) { con_place_cursor(-1, -1); return -1; }
        if (k == '\n' || k == '\r') break;
        if ((k == '\b' || k == 127) && n > 0) out[--n] = 0;
        else if (k >= 32 && k < 127 && n < cap - 1) { out[n++] = (char)k; out[n] = 0; }
    }
    return n;
}

static int confirm(const char *q)
{
    int a = ATTR(g_conf.ed_status_fg, g_conf.ed_status_bg), k;
    con_fill_rect(0, STAT_ROW, con_cols(), 1, ' ', a);
    con_text_at(0, STAT_ROW, q, a, con_cols());
    con_place_cursor((int)strlen(q) < con_cols() ? (int)strlen(q) : con_cols() - 1, STAT_ROW);
    k = con_getkey();
    if (k == 'y' || k == 'Y') return 1;
    if (k == 'n' || k == 'N') return 0;
    return -1;
}

/* ------------------------------------------------------------------ */
/* file I/O                                                            */
/* ------------------------------------------------------------------ */

static int load(const char *path)
{
    int n;
    char *txt;
    free(E.buf);
    E.buf = 0;
    E.cap = E.gs = E.ge = 0;
    E.pos = E.top = E.top_line = E.left = 0;
    E.modified = 0;
    E.match_len = 0;
    snprintf(E.path, sizeof E.path, "%s", path);
    txt = vfs_load(path, &n, 0);
    if (!txt) {
        n = 0;
        snprintf(E.msg, sizeof E.msg, "New file");
    } else {
        /* drop carriage returns from DOS line endings */
        int i, j = 0;
        for (i = 0; i < n; i++) if (txt[i] != '\r') txt[j++] = txt[i];
        n = j;
    }
    E.cap = n + 4096;
    E.buf = (char *)malloc((size_t)E.cap);
    if (!E.buf) { free(txt); E.cap = 0; return 0; }
    if (n) memcpy(E.buf + E.cap - n, txt, (size_t)n);   /* text after the gap */
    E.gs = 0;
    E.ge = E.cap - n;
    free(txt);
    return 1;
}

static int save(void)
{
    char *out;
    int i, n = len(), ok;
    if (!E.path[0]) {
        char name[VFS_PATH] = "";
        if (prompt("Save as: ", name, sizeof name) <= 0) return 0;
        if (!vfs_resolve(name, E.path)) return 0;
    }
    out = (char *)malloc((size_t)n + 1);
    if (!out) { snprintf(E.msg, sizeof E.msg, "Out of memory"); return 0; }
    for (i = 0; i < n; i++) out[i] = (char)ch(i);
    ok = vfs_save(E.path, out, n);
    free(out);
    if (ok) { E.modified = 0; snprintf(E.msg, sizeof E.msg, "Saved %d bytes", n); }
    else snprintf(E.msg, sizeof E.msg, "Save failed: %s", vfs_error());
    return ok;
}

/* ------------------------------------------------------------------ */
/* commands                                                            */
/* ------------------------------------------------------------------ */

static void find_next(int from)
{
    int n = len(), fl = (int)strlen(E.find), i, j, pass;
    if (!fl) return;
    for (pass = 0; pass < 2; pass++) {
        int start = pass ? 0 : from, end = pass ? from : n;
        for (i = start; i + fl <= n && i < end + fl; i++) {
            for (j = 0; j < fl; j++) if (tolower(ch(i + j)) != tolower((unsigned char)E.find[j])) break;
            if (j == fl) {
                E.pos = i;
                E.match_pos = i;
                E.match_len = fl;
                if (pass) snprintf(E.msg, sizeof E.msg, "Search wrapped");
                return;
            }
        }
    }
    E.match_len = 0;
    snprintf(E.msg, sizeof E.msg, "Not found: %s", E.find);
}

static void copy_line(int cut)
{
    int s = line_start(E.pos), e = line_end(E.pos), i;
    if (e < len()) e++;
    free(clip);
    clip = (char *)malloc((size_t)(e - s) + 1);
    if (!clip) { clip_len = 0; return; }
    for (i = s; i < e; i++) clip[i - s] = (char)ch(i);
    clip_len = e - s;
    if (cut) { del(s, e - s); E.pos = s; if (E.pos > len()) E.pos = len(); }
    snprintf(E.msg, sizeof E.msg, cut ? "Line cut" : "Line copied");
}

static void paste(void)
{
    int s = line_start(E.pos);
    if (!clip_len) return;
    if (clip[clip_len - 1] == '\n') {
        if (ins(s, clip, clip_len)) E.pos = s + clip_len;
    } else if (ins(E.pos, clip, clip_len)) E.pos += clip_len;
}

static void newline(void)
{
    char ind[64];
    int s = line_start(E.pos), n = 0;
    if (g_conf.autoindent) {
        while (s + n < E.pos && n < 62 && (ch(s + n) == ' ' || ch(s + n) == '\t')) { ind[n] = (char)ch(s + n); n++; }
        /* one level deeper after an opening brace */
        if (E.pos > 0 && ch(E.pos - 1) == '{') {
            int k;
            for (k = 0; k < g_conf.tabsize && n < 62; k++) ind[n++] = ' ';
        }
    }
    /* Enter between an auto-closed pair of braces opens the block */
    if (g_conf.auto_close && E.pos > 0 && E.pos < len() && ch(E.pos - 1) == '{' && ch(E.pos) == '}') {
        int k, outer = n - g_conf.tabsize;
        if (ins(E.pos, "\n", 1)) E.pos++;
        if (n && ins(E.pos, ind, n)) E.pos += n;
        if (ins(E.pos, "\n", 1) && outer > 0) ins(E.pos + 1, ind, outer);
        (void)k;
        return;
    }
    if (ins(E.pos, "\n", 1)) E.pos++;
    if (n && ins(E.pos, ind, n)) E.pos += n;
}

static int closer(int c)
{
    if (c == '(') return ')';
    if (c == '[') return ']';
    if (c == '{') return '}';
    if (c == '"' || c == '\'') return c;
    return 0;
}

static void build(int and_run)
{
    char out[VFS_PATH];
    int line = 0, rc;
    if (E.modified && !save()) return;
    if (!E.path[0]) return;
    con_place_cursor(-1, -1);
    con_default_color();
    con_clear();
    con_printf("Compiling %s ...\n", vfs_basename(E.path));
    rc = shell_compile(E.path, out, &line);
    if (rc == 0 && and_run) {
        con_printf("\n");
        shell_run_program(out, E.path);
    }
    con_default_color();
    con_printf("\n-- press any key to return to the editor --");
    con_cursor(1);
    con_getkey();
    con_clear();
    if (rc != 0 && line > 0) {
        /* the error may be in an included file: open it in a tab */
        if (shell_err_file[0] && strcmp(shell_err_file, E.path) != 0 && vfs_stat(shell_err_file, 0, 0))
            open_tab(shell_err_file);
        E.pos = pos_of_line(line - 1);
        snprintf(E.msg, sizeof E.msg, "Error at line %d", line);
    } else if (rc == 0) snprintf(E.msg, sizeof E.msg, and_run ? "Program finished" : "Build OK");
}

static void help(void)
{
    static const char *lines[] = {
        "CalcIDE editor keys",
        "",
        "Arrows/Home/End/PgUp/PgDn  move",
        "Ctrl+G   go to line",
        "Ctrl+F   find      F3/Ctrl+N next",
        "Ctrl+S   save      Ctrl+O open",
        "Ctrl+X   cut line  Ctrl+C copy line",
        "Ctrl+V   paste     Ctrl+D duplicate",
        "Ctrl+K   delete line",
        "Ctrl+O   open (new tab)",
        "F6/F7    previous/next tab",
        "Ctrl+W   close tab",
        "Tab      indent    Ctrl+T top/bottom",
        "F9/Ctrl+B compile",
        "F5/Ctrl+R compile and run",
        "Esc/Ctrl+Q quit",
        "",
        "Colours: /CalcIDE.conf (see 'set')",
        "",
        "Press any key",
        0
    };
    int i, a = ATTR(g_conf.ed_fg, g_conf.ed_bg);
    con_fill_rect(0, 0, con_cols(), CON_ROWS, ' ', a);
    for (i = 0; lines[i]; i++) con_text_at(2, 2 + i, lines[i], i == 0 ? ATTR(14, g_conf.ed_bg) : a, con_cols() - 2);
    con_place_cursor(-1, -1);
    con_getkey();
}

static int ask_save_and_quit(void)
{
    int r;
    char q[80];
    if (!E.modified) return 1;
    snprintf(q, sizeof q, " Save %s? (y/n, Esc=cancel) ", E.path[0] ? vfs_basename(E.path) : "changes");
    r = confirm(q);
    if (r < 0) return 0;
    if (r == 1 && !save()) return 0;
    return 1;
}

/* open a file in a tab of its own, or switch to it if it is open */
static int open_tab(const char *full)
{
    int i, max = g_conf.max_files;
    if (max < 1) max = 1;
    if (max > MAX_TABS) max = MAX_TABS;
    for (i = 0; i < ntabs; i++)
        if (full[0] && !strcmp(tabs[i].path, full)) { cur = i; return 1; }
    if (ntabs >= max) {
        snprintf(E.msg, sizeof E.msg, "%d files open (max_files = %d)", ntabs, max);
        return 0;
    }
    cur = ntabs++;
    memset(&E, 0, sizeof E);
    E.want_col = -1;
    if (!load(full)) {
        ntabs--;
        cur = ntabs > 0 ? ntabs - 1 : 0;
        if (ntabs) snprintf(E.msg, sizeof E.msg, "Out of memory");
        return 0;
    }
    return 1;
}

static void close_tab(void)
{
    int i;
    if (!ask_save_and_quit()) return;
    free(E.buf);
    for (i = cur; i < ntabs - 1; i++) tabs[i] = tabs[i + 1];
    ntabs--;
    memset(&tabs[ntabs], 0, sizeof(Ed));
    if (ntabs == 0) { ed_quit = 1; cur = 0; return; }
    if (cur >= ntabs) cur = ntabs - 1;
    con_clear();
}

static void quit_all(void)
{
    int i;
    for (i = 0; i < ntabs; i++) {
        cur = i;
        if (E.modified) {
            con_clear();
            if (!ask_save_and_quit()) return;   /* Esc: stay in the editor */
        }
    }
    for (i = 0; i < ntabs; i++) { free(tabs[i].buf); tabs[i].buf = 0; }
    ntabs = 0;
    cur = 0;
    ed_quit = 1;
}

static void key(int k)
{
    int ls, n;
    E.msg[0] = 0;
    if (k != KEY_F3 && k != KEY_CTRL('n')) E.match_len = 0;
    switch (k) {
    case KEY_LEFT: if (E.pos > 0) E.pos--; E.want_col = -1; break;
    case KEY_RIGHT: if (E.pos < len()) E.pos++; E.want_col = -1; break;
    case KEY_UP: case KEY_DOWN: case KEY_PGUP: case KEY_PGDN: {
        int steps = (k == KEY_PGUP || k == KEY_PGDN) ? TEXT_ROWS - 1 : 1, i;
        int up = (k == KEY_UP || k == KEY_PGUP);
        if (E.want_col < 0) E.want_col = vcol(E.pos);
        ls = line_start(E.pos);
        for (i = 0; i < steps; i++) {
            int nl = up ? prev_line(ls) : next_line(ls);
            if (nl < 0) break;
            ls = nl;
            if (k == KEY_PGUP && E.top_line > 0) { E.top = prev_line(E.top); E.top_line--; }
            if (k == KEY_PGDN && next_line(E.top) >= 0 && E.top_line + TEXT_ROWS < line_count()) { E.top = next_line(E.top); E.top_line++; }
        }
        E.pos = pos_at_vcol(ls, E.want_col);
        return;
    }
    case KEY_HOME: {
        /* first press: first non-blank; second: column 0 */
        int s = line_start(E.pos), f = s;
        while (f < line_end(s) && (ch(f) == ' ' || ch(f) == '\t')) f++;
        E.pos = (E.pos == f) ? s : f;
        break;
    }
    case KEY_END: E.pos = line_end(E.pos); break;
    case KEY_CTRL('t'): E.pos = (E.pos == 0) ? len() : 0; break;
    case KEY_CHOME: E.pos = 0; break;
    case KEY_CEND: E.pos = len(); break;
    case '\b': case 127:
        /* backspace inside an empty pair removes both halves */
        if (g_conf.auto_close && E.pos > 0 && E.pos < len() && closer(ch(E.pos - 1)) && closer(ch(E.pos - 1)) == ch(E.pos)) {
            del(E.pos - 1, 2);
            E.pos--;
            break;
        }
        if (E.pos > 0) {
            /* backspace over a soft tab */
            int s = line_start(E.pos), i, all = 1, ts = g_conf.tabsize;
            for (i = s; i < E.pos; i++) if (ch(i) != ' ') all = 0;
            if (all && E.pos > s && ts > 1 && (E.pos - s) % ts == 0) { del(E.pos - ts, ts); E.pos -= ts; }
            else { del(E.pos - 1, 1); E.pos--; }
        }
        break;
    case KEY_DEL: del(E.pos, 1); break;
    case '\n': case '\r': newline(); break;
    case '\t': {
        int sp = g_conf.tabsize - vcol(E.pos) % g_conf.tabsize;
        char t[8] = "        ";
        if (ins(E.pos, t, sp)) E.pos += sp;
        break;
    }
    case KEY_CTRL('s'): save(); break;
    case KEY_CTRL('f'): {
        if (prompt("Find: ", E.find, sizeof E.find) > 0) find_next(E.pos);
        break;
    }
    case KEY_F3: case KEY_CTRL('n'):
        if (!E.find[0] && prompt("Find: ", E.find, sizeof E.find) <= 0) break;
        find_next(E.match_len ? E.pos + 1 : E.pos);
        break;
    case KEY_CTRL('g'): {
        char num[12] = "";
        if (prompt("Go to line: ", num, sizeof num) > 0 && atoi(num) > 0) E.pos = pos_of_line(atoi(num) - 1);
        break;
    }
    case KEY_CTRL('x'): copy_line(1); break;
    case KEY_CTRL('c'): copy_line(0); break;
    case KEY_CTRL('v'): paste(); break;
    case KEY_CTRL('k'): {
        int s = line_start(E.pos), e = line_end(E.pos);
        if (e < len()) e++;
        del(s, e - s);
        E.pos = s > len() ? len() : s;
        break;
    }
    case KEY_CTRL('d'): {
        int s = line_start(E.pos), e = line_end(E.pos), i;
        char *tmp = (char *)malloc((size_t)(e - s) + 1);
        if (!tmp) break;
        tmp[0] = '\n';
        for (i = s; i < e; i++) tmp[i - s + 1] = (char)ch(i);
        ins(e, tmp, e - s + 1);
        free(tmp);
        break;
    }
    case KEY_CTRL('o'): {
        char name[VFS_PATH] = "", full[VFS_PATH];
        if (prompt("Open: ", name, sizeof name) > 0 && vfs_resolve(name, full)) {
            int was = cur;
            if (open_tab(full)) con_clear();
            else if (was < ntabs && cur != was) cur = was;
        }
        break;
    }
    case KEY_F6: if (ntabs > 1) { cur = (cur + ntabs - 1) % ntabs; con_clear(); } return;
    case KEY_F7: if (ntabs > 1) { cur = (cur + 1) % ntabs; con_clear(); } return;
    case KEY_CTRL('w'): close_tab(); return;
    case KEY_F9: case KEY_CTRL('b'): build(0); break;
    case KEY_F5: case KEY_CTRL('r'): build(1); break;
    case KEY_F1: help(); break;
    case KEY_ESC: case KEY_CTRL('q'):
        quit_all();
        return;
    default:
        if (k >= 32 && k < 127) {
            char c = (char)k;
            if (g_conf.auto_close) {
                int nx = E.pos < len() ? ch(E.pos) : 0;
                int pv = E.pos > 0 ? ch(E.pos - 1) : 0;
                /* typing a closer that is already there just steps over it */
                if ((k == ')' || k == ']' || k == '}' || k == '"' || k == '\'') && nx == k) { E.pos++; return; }
                if (k == '(' || k == '[' || k == '{'
                    || ((k == '"' || k == '\'') && !isalnum(pv) && pv != '\\' && pv != k)) {
                    char pair[2];
                    pair[0] = c;
                    pair[1] = (char)closer(k);
                    if (ins(E.pos, pair, 2)) E.pos++;
                    return;
                }
            }
            if (ins(E.pos, &c, 1)) E.pos++;
            /* a closing brace on an otherwise blank line moves out a level */
            if (k == '}') {
                int s = line_start(E.pos - 1), i, blank = 1;
                for (i = s; i < E.pos - 1; i++) if (ch(i) != ' ' && ch(i) != '\t') blank = 0;
                n = E.pos - 1 - s;
                if (blank && n >= g_conf.tabsize) { del(E.pos - 1 - g_conf.tabsize, g_conf.tabsize); E.pos -= g_conf.tabsize; }
            }
        }
        return;
    }
    if (k != KEY_UP && k != KEY_DOWN) E.want_col = -1;
}

int editor_open(int n, char **paths)
{
    char full[VFS_PATH] = "";
    int i;
    memset(tabs, 0, sizeof tabs);
    ntabs = 0;
    cur = 0;
    ed_quit = 0;
    for (i = 0; i < n || (n == 0 && i == 0); i++) {
        full[0] = 0;
        if (n > 0 && !vfs_resolve(paths[i], full)) { con_printf("edit: bad path %s\n", paths[i]); continue; }
        if (!open_tab(full)) {
            if (!ntabs) { con_printf("edit: out of memory\n"); return 1; }
            break;
        }
    }
    if (!ntabs) return 1;
    cur = 0;
    con_clear();
    while (!ed_quit) {
        draw();
        key(con_getkey());
    }
    for (i = 0; i < MAX_TABS; i++) { free(tabs[i].buf); tabs[i].buf = 0; }
    free(clip);
    clip = 0;
    clip_len = 0;
    con_place_cursor(-1, -1);
    con_default_color();
    con_clear();
    return 0;
}

int editor_run(const char *path)
{
    char *p = (char *)path;
    return editor_open(path && *path ? 1 : 0, &p);
}
