/* tracker.c - a step editor for .song files.
 *
 * Four channel columns, one row per sixteenth note. A cell holds a note,
 * a drum, an "off" (rest), or nothing - which holds whatever came before.
 * Saving writes an ordinary .song file that `play` understands; loading
 * reads any .song whose notes fit on the sixteenth-note grid.
 */
#include "tracker.h"
#include "sound.h"
#include "console.h"
#include "config.h"
#include "hal.h"
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define NCH 4
#define MAXROWS 1024
#define TOP 2                       /* first grid row on screen */

enum { C_EMPTY, C_NOTE, C_DRUM, C_OFF };
typedef struct { uint8_t kind, val; } Cell;

static const char *drum_name[9] = {"", "K", "SN", "HH", "OH", "CP", "TH", "TL", "CY"};
static const char *ins_name[5] = {"ISQ", "IPLS", "ITRI", "ISAW", "ISIN"};
static const char *ins_short[5] = {"SQ", "PLS", "TRI", "SAW", "SIN"};
static const char *note_name[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static Cell *grid;
static int rows, tempo, ins[NCH], vol[NCH];
static int cx, cy, top, oct, adv, modified, quit;
static char path[VFS_PATH], msg[64];

#define AT(r, c) grid[(r) * NCH + (c)]

static int used_rows(void)
{
    int r, c, last = -1;
    for (r = 0; r < rows; r++)
        for (c = 0; c < NCH; c++)
            if (AT(r, c).kind != C_EMPTY) last = r;
    return last + 1;
}

static int grow(int want)
{
    Cell *g;
    if (want <= rows) return 1;
    if (want > MAXROWS) return 0;
    want = (want + 15) / 16 * 16;
    g = (Cell *)realloc(grid, (size_t)want * NCH * sizeof(Cell));
    if (!g) return 0;
    memset(g + rows * NCH, 0, (size_t)(want - rows) * NCH * sizeof(Cell));
    grid = g;
    rows = want;
    return 1;
}

/* ------------------------------------------------------------------ */
/* song text                                                           */
/* ------------------------------------------------------------------ */

typedef struct { char *s; size_t n, cap; int ok; } Buf;

static void put(Buf *b, const char *t)
{
    size_t l = strlen(t);
    if (!b->ok) return;
    if (b->n + l + 1 > b->cap) {
        size_t nc = (b->cap + l + 1) * 2;
        char *ns = (char *)realloc(b->s, nc);
        if (!ns) { b->ok = 0; return; }
        b->s = ns;
        b->cap = nc;
    }
    memcpy(b->s + b->n, t, l + 1);
    b->n += l;
}

static void len_text(int n, char *out)
{
    switch (n) {
    case 1: strcpy(out, "/16"); break;
    case 2: strcpy(out, "/8"); break;
    case 3: strcpy(out, "/8."); break;
    case 4: strcpy(out, "/4"); break;
    case 6: strcpy(out, "/4."); break;
    case 8: strcpy(out, "/2"); break;
    case 12: strcpy(out, "/2."); break;
    case 16: strcpy(out, "/1"); break;
    default: snprintf(out, 16, "/16*%d", n); break;
    }
}

static void cell_text(Cell c, char *out)
{
    if (c.kind == C_NOTE) sprintf(out, "%s%d", note_name[c.val % 12], c.val / 12);
    else if (c.kind == C_DRUM) strcpy(out, drum_name[c.val]);
    else strcpy(out, "R");
}

/* the song from row `from` to the end. file=1: a .song file with one line
 * per bar; file=0: one string with channels joined by '|' (for play). */
static char *gen(int from, int file)
{
    Buf b = {0, 0, 0, 1};
    int end = (used_rows() + 15) / 16 * 16, c, r, first = 1;   /* to the end of the bar */
    char t[200];
    put(&b, "");
    if (file) {
        sprintf(t, "# %s - made with the CalcIDE tracker\nT%d\n", vfs_basename(path), tempo);
        put(&b, t);
    }
    for (c = 0; c < NCH; c++) {
        int any = 0;
        for (r = 0; r < end; r++) if (AT(r, c).kind != C_EMPTY) any = 1;
        if (!any) continue;
        if (file) sprintf(t, "%d: %s V%d", c + 1, ins_name[ins[c]], vol[c]);
        else sprintf(t, "%sT%d %s V%d", first ? "" : " | ", tempo, ins_name[ins[c]], vol[c]);
        put(&b, t);
        first = 0;
        r = from;
        {
            int bar = from / 16;
            while (r < end) {
                Cell start = AT(r, c);
                int n = 1;
                char lt[16], nt[8];
                if (file && r / 16 > bar) { bar = r / 16; sprintf(t, "\n%d:", c + 1); put(&b, t); }
                while (r + n < end && AT(r + n, c).kind == C_EMPTY) n++;
                if (start.kind == C_EMPTY) {
                    /* starting part-way through a held note: find it */
                    int k = r - 1;
                    while (k >= 0 && AT(k, c).kind == C_EMPTY) k--;
                    start.kind = C_OFF;
                    if (k >= 0 && AT(k, c).kind == C_NOTE) start = AT(k, c);
                }
                cell_text(start, nt);
                len_text(n, lt);
                sprintf(t, " %s%s", nt, lt);
                put(&b, t);
                r += n;
            }
        }
        if (file) put(&b, "\n");
    }
    if (!b.ok) { free(b.s); return 0; }
    return b.s;
}

/* ------------------------------------------------------------------ */
/* loading a .song                                                     */
/* ------------------------------------------------------------------ */

static int parse_channel(const char *text, int c, char *why)
{
    char *s = song_expand(text), *p;
    int deflen = 4, defoct = 4, row = 0, lost = 0;
    if (!s) { strcpy(why, play_error()[0] ? play_error() : "out of memory"); return -1; }
    for (p = strtok(s, " \t,\r\n"); p; p = strtok(0, " \t,\r\n")) {
        char tok[24], *q;
        Cell cell = {C_EMPTY, 0};
        int len, mul = 1, steps, i;
        snprintf(tok, sizeof tok, "%s", p);
        for (q = tok; *q; q++) *q = (char)toupper((unsigned char)*q);
        q = tok;
        if (q[0] == 'I') {
            for (i = 0; i < 5; i++) if (!strcmp(q, ins_name[i])) ins[c] = i;
            continue;
        }
        if ((q[0] == 'A' && q[1] == 'T') || (q[0] == 'R' && q[1] == 'L') || (q[0] == 'V' && q[1] == 'B') || (q[0] == 'T' && q[1] == 'P')) { lost = 1; continue; }
        if (strchr("TLOV", q[0]) && isdigit((unsigned char)q[1])) {
            int v = atoi(q + 1);
            if (q[0] == 'T') { if (!tempo) tempo = v; else if (v != tempo) lost = 1; }
            else if (q[0] == 'L') deflen = v;
            else if (q[0] == 'O') defoct = v;
            else vol[c] = v;
            continue;
        }
        for (i = 8; i >= 1; i--) {
            size_t l = strlen(drum_name[i]);
            if (!strncmp(q, drum_name[i], l) && (!q[l] || q[l] == '/' || q[l] == '.' || q[l] == '*')) { cell.kind = C_DRUM; cell.val = (uint8_t)i; q += l; break; }
        }
        if (cell.kind == C_EMPTY) {
            if (*q == 'R') { cell.kind = C_OFF; q++; }
            else if (*q >= 'A' && *q <= 'G') {
                static const int base[7] = {9, 11, 0, 2, 4, 5, 7};
                int semi = base[*q++ - 'A'], o = defoct;
                if (*q == '#') { semi++; q++; }
                else if (*q == 'B' && q[1] != 0 && (isdigit((unsigned char)q[1]) || strchr("/.*~", q[1]))) { semi--; q++; }
                else if (*q == 'B' && !q[1]) { semi--; q++; }
                if (isdigit((unsigned char)*q)) o = *q++ - '0';
                if (*q == '~') { lost = 1; q++; while (*q && *q != '/' && *q != '*' && *q != '.') q++; }
                semi += o * 12;
                if (semi < 0) semi = 0;
                if (semi > 107) semi = 107;
                cell.kind = C_NOTE;
                cell.val = (uint8_t)semi;
            } else { sprintf(why, "can't read '%.12s'", p); free(s); return -1; }
        }
        len = deflen;
        if (*q == '/') { len = atoi(q + 1); q++; while (isdigit((unsigned char)*q)) q++; }
        if (*q == '*') { mul = atoi(q + 1); q++; while (isdigit((unsigned char)*q)) q++; }
        if (len <= 0 || 16 % len) { sprintf(why, "'%.12s' is shorter than 1/16", p); free(s); return -1; }
        steps = 16 / len * mul;
        if (*q == '.') { if (steps % 2) { sprintf(why, "'%.12s' is off the 1/16 grid", p); free(s); return -1; } steps += steps / 2; }
        if (!grow(row + steps)) { strcpy(why, "song too long for the tracker"); free(s); return -1; }
        AT(row, c) = cell;
        row += steps;
    }
    free(s);
    return lost;
}

static int load(char *why)
{
    char *txt = song_load_text(path), *p;
    int c = 0, lost = 0;
    if (!txt) { strcpy(why, play_error()); return 0; }
    for (p = txt; c < NCH; c++) {
        char *bar = strchr(p, '|');
        int r;
        if (bar) *bar = 0;
        r = parse_channel(p, c, why);
        if (r < 0) { free(txt); return 0; }
        lost |= r;
        if (!bar) break;
        p = bar + 1;
    }
    free(txt);
    if (lost) strcpy(why, "loaded; slides/effects/tempo changes were dropped");
    return 1;
}

static int save(void)
{
    char *s = gen(0, 1);
    vfile *f;
    int ok;
    if (!s) { strcpy(msg, "out of memory"); return 0; }
    f = vfs_open(path, "w");
    if (!f) { free(s); snprintf(msg, sizeof msg, "can't write %s", vfs_basename(path)); return 0; }
    ok = vfs_write(f, s, (int)strlen(s)) == (int)strlen(s);
    vfs_close(f);
    free(s);
    if (ok) { modified = 0; snprintf(msg, sizeof msg, "saved %s", vfs_basename(path)); }
    else strcpy(msg, "write failed");
    return ok;
}

/* ------------------------------------------------------------------ */
/* screen                                                              */
/* ------------------------------------------------------------------ */

static int vis_rows(void) { return con_rows() - TOP - 1; }

static void draw(void)
{
    int cols = con_cols(), bg = g_conf.ed_bg, sa = ATTR(g_conf.ed_status_fg, g_conf.ed_status_bg);
    int c, y;
    char t[80];
    con_fill_rect(0, 0, cols, 1, ' ', sa);
    snprintf(t, sizeof t, " TRACKER %s%s  T%d  O%d  +%d  row %d", vfs_basename(path), modified ? "*" : "",
             tempo, oct, adv, cy + 1);
    con_text_at(0, 0, t, sa, cols);
    con_fill_rect(0, 1, cols, 1, ' ', ATTR(g_conf.ed_lnum, bg));
    for (c = 0; c < NCH; c++) {
        snprintf(t, sizeof t, "%d %s%d", c + 1, ins_short[ins[c]], vol[c]);
        con_text_at(4 + c * 7, 1, t, ATTR(c == cx ? 14 : g_conf.ed_lnum, bg), 7);
    }
    for (y = 0; y < vis_rows(); y++) {
        int r = top + y;
        con_fill_rect(0, TOP + y, cols, 1, ' ', ATTR(g_conf.ed_fg, bg));
        if (r >= rows) continue;
        snprintf(t, sizeof t, "%3d", r + 1);
        con_text_at(0, TOP + y, t, ATTR(r % 16 == 0 ? 14 : r % 4 == 0 ? g_conf.ed_fg : g_conf.ed_lnum, bg), 3);
        for (c = 0; c < NCH; c++) {
            Cell ce = AT(r, c);
            int a, fg;
            if (ce.kind == C_NOTE) { cell_text(ce, t); fg = g_conf.ed_kw; }
            else if (ce.kind == C_DRUM) { strcpy(t, drum_name[ce.val]); fg = g_conf.ed_str; }
            else if (ce.kind == C_OFF) { strcpy(t, "---"); fg = g_conf.ed_com; }
            else { strcpy(t, r % 4 == 0 ? " . " : "   "); fg = g_conf.ed_lnum; }
            a = ATTR(fg, bg);
            if (r == cy && c == cx) a = ATTR(g_conf.ed_status_fg, g_conf.ed_status_bg);
            else if (r == cy) a = ATTR(fg, g_conf.ed_sel);
            con_fill_rect(4 + c * 7, TOP + y, 5, 1, ' ', a);
            con_text_at(5 + c * 7, TOP + y, t, a, 4);
        }
    }
    con_fill_rect(0, con_rows() - 1, cols, 1, ' ', sa);
    con_text_at(0, con_rows() - 1, msg[0] ? msg : " a-g note  kshoptly drum  r off  F1 help", sa, cols);
    msg[0] = 0;
    con_place_cursor(5 + cx * 7, TOP + cy - top);
    con_flush();
}

static void help(void)
{
    static const char *lines[] = {
        "TRACKER KEYS (one row = 1/16 note)",
        "a b c d e f g  note at the octave shown",
        "#  sharp     + -  octave up / down",
        "k s h o p t l y  kick snare hat open",
        "               clap tomH tomL crash",
        "r  note off (rest)   Space/Del clear",
        "   (an empty cell holds the last note)",
        "arrows, PgUp/PgDn, Home/End  move",
        "Tab  next channel  n  step +1/2/4/8",
        "i  instrument   [ ]  volume",
        "{ }  tempo      Ins / Ctrl+K row in/out",
        "Ctrl+D  copy this bar to the next",
        "Enter  play from this bar",
        "F5 / Ctrl+R  play all  (break stops)",
        "Ctrl+S save   Ctrl+W save + make .wav",
        "Esc / Ctrl+Q  quit",
        0
    };
    int i, a = ATTR(g_conf.ed_fg, g_conf.ed_bg);
    con_fill_rect(0, 0, con_cols(), con_rows(), ' ', a);
    for (i = 0; lines[i]; i++) con_text_at(1, 1 + i, lines[i], i == 0 ? ATTR(14, g_conf.ed_bg) : a, con_cols() - 1);
    con_place_cursor(-1, -1);
    con_getkey();
}

static int ask(const char *q)
{
    int k;
    snprintf(msg, sizeof msg, "%s", q);
    draw();
    for (;;) {
        k = con_getkey();
        if (k == 'y' || k == 'Y') return 1;
        if (k == 'n' || k == 'N') return 0;
        if (k == KEY_ESC) return -1;
    }
}

/* ------------------------------------------------------------------ */
/* playing                                                             */
/* ------------------------------------------------------------------ */

static void play_from(int row)
{
    char *s = gen(row, 0);
    int r;
    if (!s || !s[0]) { free(s); strcpy(msg, "nothing to play"); return; }
    snprintf(msg, sizeof msg, "playing from row %d (break stops)", row + 1);
    draw();
    r = play_notes(s);
    free(s);
    if (r < 0) snprintf(msg, sizeof msg, "%s", play_error());
    else if (r == 1) strcpy(msg, "stopped");
}

static void preview(Cell c, int ch)
{
    char t[40], nt[8];
    if (g_conf.mute) return;
    cell_text(c, nt);
    snprintf(t, sizeof t, "T%d %s V%d %s/16", tempo, ins_name[ins[ch]], vol[ch], nt);
    play_notes(t);
}

static void make_wav(void)
{
    char out[VFS_PATH], *dot, *s;
    uint32_t bytes = 0;
    int r;
    if (!save()) return;
    snprintf(out, sizeof out, "%s", path);
    dot = strrchr(out, '.');
    if (dot && !strchr(dot, '/')) *dot = 0;
    strncat(out, ".wav", sizeof out - strlen(out) - 1);
    s = gen(0, 0);
    if (!s) { strcpy(msg, "out of memory"); return; }
    snprintf(msg, sizeof msg, "rendering %s...", vfs_basename(out));
    draw();
    r = play_to_wav(s, out, &bytes);
    free(s);
    if (r < 0) snprintf(msg, sizeof msg, "%s", play_error());
    else snprintf(msg, sizeof msg, "wrote %s (%lu KB)", vfs_basename(out), (unsigned long)(bytes / 1024));
}

/* ------------------------------------------------------------------ */

static void put_cell(Cell c)
{
    if (!grow(cy + 1)) { strcpy(msg, "too many rows"); return; }
    AT(cy, cx) = c;
    modified = 1;
    if (c.kind == C_NOTE || c.kind == C_DRUM) preview(c, cx);
    if (grow(cy + adv + 1)) cy += adv;
}

static void key(int k)
{
    static const int base[7] = {9, 11, 0, 2, 4, 5, 7};
    Cell *cur = cy < rows ? &AT(cy, cx) : 0;
    int r, c;
    if (k >= 'a' && k <= 'g') { Cell n = {C_NOTE, (uint8_t)(oct * 12 + base[k - 'a'])}; put_cell(n); return; }
    switch (k) {
    case 'k': case 's': case 'h': case 'o': case 'p': case 't': case 'l': case 'y': {
        const char *m = " kshoptly";
        Cell n = {C_DRUM, (uint8_t)(strchr(m, k) - m)};
        put_cell(n);
        return;
    }
    case 'r': { Cell n = {C_OFF, 0}; put_cell(n); return; }
    case ' ': case KEY_DEL: case 8: case 127:
        if (cur) { cur->kind = C_EMPTY; modified = 1; }
        if (k == ' ' && grow(cy + adv + 1)) cy += adv;
        return;
    case '#':
        if (cur && cur->kind == C_NOTE && cur->val < 107) { cur->val++; modified = 1; preview(*cur, cx); }
        return;
    case '+': case '=':
        if (cur && cur->kind == C_NOTE && cur->val + 12 <= 107) { cur->val += 12; modified = 1; preview(*cur, cx); }
        if (oct < 8) oct++;
        return;
    case '-':
        if (cur && cur->kind == C_NOTE && cur->val >= 12) { cur->val -= 12; modified = 1; preview(*cur, cx); }
        if (oct > 0) oct--;
        return;
    case 'n': adv = adv >= 8 ? 1 : adv * 2; return;
    case 'i': ins[cx] = (ins[cx] + 1) % 5; modified = 1; return;
    case '[': if (vol[cx] > 0) vol[cx]--; modified = 1; return;
    case ']': if (vol[cx] < 9) vol[cx]++; modified = 1; return;
    case '{': if (tempo > 40) tempo -= 5; modified = 1; return;
    case '}': if (tempo < 300) tempo += 5; modified = 1; return;
    case KEY_UP: if (cy > 0) cy--; return;
    case KEY_DOWN: if (grow(cy + 2)) cy++; return;
    case KEY_LEFT: cx = (cx + NCH - 1) % NCH; return;
    case KEY_RIGHT: case 9: cx = (cx + 1) % NCH; return;
    case KEY_PGUP: cy = cy >= 16 ? cy - 16 : 0; return;
    case KEY_PGDN: if (grow(cy + 17)) cy += 16; return;
    case KEY_HOME: cy = 0; return;
    case KEY_END: cy = used_rows() ? used_rows() - 1 : 0; return;
    case KEY_INS:
        if (!grow(used_rows() + 1)) return;
        for (r = rows - 1; r > cy; r--) AT(r, cx) = AT(r - 1, cx);
        AT(cy, cx).kind = C_EMPTY;
        modified = 1;
        return;
    case KEY_CTRL('k'):
        for (r = cy; r + 1 < rows; r++) AT(r, cx) = AT(r + 1, cx);
        AT(rows - 1, cx).kind = C_EMPTY;
        modified = 1;
        return;
    case KEY_CTRL('d'): {
        int b = cy / 16 * 16;
        if (!grow(b + 32)) { strcpy(msg, "too many rows"); return; }
        for (r = 0; r < 16; r++) for (c = 0; c < NCH; c++) AT(b + 16 + r, c) = AT(b + r, c);
        cy = b + 16 + cy % 16;
        modified = 1;
        snprintf(msg, sizeof msg, "bar %d copied to bar %d", b / 16 + 1, b / 16 + 2);
        return;
    }
    case 10: case 13: play_from(cy / 16 * 16); return;
    case KEY_F5: case KEY_CTRL('r'): play_from(0); return;
    case KEY_CTRL('s'): save(); return;
    case KEY_CTRL('w'): make_wav(); return;
    case KEY_F1: help(); return;
    case KEY_ESC: case KEY_CTRL('q'):
        if (modified) {
            int a = ask(" Save changes? (y/n, Esc=cancel) ");
            if (a < 0) return;
            if (a == 1 && !save()) return;
        }
        quit = 1;
        return;
    }
}

int tracker_open(const char *file)
{
    char why[64] = "";
    int c;
    grid = 0;
    rows = 0;
    tempo = 0;
    for (c = 0; c < NCH; c++) { ins[c] = c == 1 ? 2 : c == 0 ? 1 : 0; vol[c] = 7; }
    ins[3] = 0;
    cx = cy = top = 0;
    oct = 4;
    adv = 1;
    modified = quit = 0;
    msg[0] = 0;
    if (!vfs_resolve(file && *file ? file : "untitled.song", path)) { con_puts("tracker: bad path\n"); return 1; }
    {
        int isdir = 0;
        if (vfs_stat(path, &isdir, 0)) {
            if (isdir) { con_printf("tracker: %s is a directory\n", file); return 1; }
            for (c = 0; c < NCH; c++) { ins[c] = 0; vol[c] = 7; }
            if (!load(why)) { con_printf("tracker: %s\n", why); free(grid); grid = 0; return 1; }
            snprintf(msg, sizeof msg, "%s", why);
        } else snprintf(msg, sizeof msg, "new song - F1 for help");
    }
    if (!tempo) tempo = 120;
    if (!grow(64)) { con_puts("tracker: out of memory\n"); return 1; }
    while (!quit) {
        if (cy < top) top = cy;
        if (cy >= top + vis_rows()) top = cy - vis_rows() + 1;
        draw();
        key(con_getkey());
    }
    free(grid);
    grid = 0;
    con_default_color();
    con_clear();
    return 0;
}
