/* console.c - a 32-row text console with 16 colours and an ANSI parser.
 *
 * The grid is kept in RAM (64 x 32 cells of char + attribute = 4 KB) and
 * rows are only sent to the panel when they change, at con_flush(). Bursts
 * of output therefore cost one redraw, not one per character.
 *
 * Supported escape sequences (enough for most programs that target a VT100
 * or an MS-DOS ANSI.SYS): ESC[n;mH ESC[nA/B/C/D ESC[J ESC[2J ESC[K ESC[s/u
 * ESC[?25l/h and SGR ESC[...m with 0, 1, 2, 5, 7, 22, 27, 30-37, 39,
 * 40-47, 49, 90-97, 100-107.
 */
#include "console.h"
#include "config.h"
#include <stdio.h>
#include <string.h>

/* CGA / MS-DOS palette, in ANSI order, as RGB565 */
const uint16_t con_palette[16] = {
    0x0000, 0xA800, 0x0540, 0xAAA0, 0x0015, 0xA815, 0x0555, 0xAD55,
    0x52AA, 0xFAAA, 0x57EA, 0xFFEA, 0x52BF, 0xFABF, 0x57FF, 0xFFFF
};

static cell_t grid[CON_ROWS][CON_MAXCOLS];
static uint8_t dirty[CON_ROWS];
static int cols = 40;
static int cx, cy;
static int attr = 0x07;
static int reverse;
static int bold;
static int cur_on = 1;
static int place_x = -1, place_y = -1;
static int save_x, save_y;
static int mirror = 1;
static int last_cur_row = -1;
static uint32_t blink_t;
static int blink_phase = 1;

/* ANSI parser */
static int esc_state;            /* 0 none, 1 ESC, 2 CSI */
static int esc_par[8];
static int esc_n;
static int esc_priv;

/* keyboard FIFO */
static int kq[128];
static int kq_head, kq_tail;

int con_scrolls;                 /* incremented on every scroll (line editor) */

static void mark(int r) { if (r >= 0 && r < CON_ROWS) dirty[r] = 1; }

void con_invalidate(void)
{
    int r;
    for (r = 0; r < CON_ROWS; r++) dirty[r] = 1;
}

static int cur_attr(void)
{
    int fg = attr & 15, bg = (attr >> 4) & 15;
    if (bold && fg < 8) fg += 8;
    if (reverse) { int t = fg; fg = bg; bg = t; }
    return ATTR(fg, bg);
}

static void clear_row(int r, int from)
{
    int c;
    cell_t v = (cell_t)(' ' | (cur_attr() << 8));
    for (c = from; c < CON_MAXCOLS; c++) grid[r][c] = v;
    mark(r);
}

void con_clear(void)
{
    int r;
    for (r = 0; r < CON_ROWS; r++) clear_row(r, 0);
    cx = cy = 0;
}

void con_init(void)
{
    attr = ATTR(g_conf.fg, g_conf.bg);
    cur_on = g_conf.cursor;
    con_set_width(g_conf.width);
    con_clear();
}

void con_set_width(int w)
{
    int r;
    cols = (w >= 64) ? 64 : 40;
    hal_set_font(cols == 64 ? 1 : 0);
    if (cx >= cols) cx = cols - 1;
    for (r = 0; r < CON_ROWS; r++) mark(r);
}

int con_cols(void) { return cols; }
int con_rows(void) { return CON_ROWS; }
int con_x(void) { return cx; }
int con_y(void) { return cy; }
int con_fg(void) { return attr & 15; }
int con_bg(void) { return (attr >> 4) & 15; }
int con_cursor_on(void) { return cur_on; }
int con_mirror(int on) { int old = mirror; mirror = on; return old; }

static void scroll_up(void)
{
    int r;
    memmove(grid[0], grid[1], sizeof(grid[0]) * (CON_ROWS - 1));
    clear_row(CON_ROWS - 1, 0);
    for (r = 0; r < CON_ROWS; r++) mark(r);
    con_scrolls++;
}

static void newline(void)
{
    cx = 0;
    if (cy == CON_ROWS - 1) scroll_up();
    else cy++;
}

void con_set_color(int fg, int bg)
{
    attr = ATTR(fg, bg);
    reverse = bold = 0;
}

void con_set_fg(int fg) { attr = (attr & 0xF0) | (fg & 15); }
void con_set_bg(int bg) { attr = (attr & 0x0F) | ((bg & 15) << 4); }

void con_default_color(void)
{
    attr = ATTR(g_conf.fg, g_conf.bg);
    reverse = bold = 0;
}

void con_cursor(int on) { cur_on = on ? 1 : 0; mark(cy); }

void con_gotoxy(int x, int y)
{
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= cols) x = cols - 1;
    if (y >= CON_ROWS) y = CON_ROWS - 1;
    mark(cy);
    cx = x;
    cy = y;
    mark(cy);
}

void con_clear_eol(void) { clear_row(cy, cx); }

static void sgr(int p)
{
    static const int ansi2pal[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    if (p == 0) { con_default_color(); }
    else if (p == 1) bold = 1;
    else if (p == 2 || p == 22) bold = 0;
    else if (p == 7) reverse = 1;
    else if (p == 27) reverse = 0;
    else if (p >= 30 && p <= 37) con_set_fg(ansi2pal[p - 30]);
    else if (p == 39) con_set_fg(g_conf.fg);
    else if (p >= 40 && p <= 47) con_set_bg(ansi2pal[p - 40]);
    else if (p == 49) con_set_bg(g_conf.bg);
    else if (p >= 90 && p <= 97) con_set_fg(ansi2pal[p - 90] + 8);
    else if (p >= 100 && p <= 107) con_set_bg(ansi2pal[p - 100] + 8);
}

static void csi(int final)
{
    int p0 = esc_n > 0 ? esc_par[0] : 0;
    int p1 = esc_n > 1 ? esc_par[1] : 0;
    int i, r;
    switch (final) {
    case 'H': case 'f':
        con_gotoxy((p1 ? p1 : 1) - 1, (p0 ? p0 : 1) - 1);
        break;
    case 'A': con_gotoxy(cx, cy - (p0 ? p0 : 1)); break;
    case 'B': con_gotoxy(cx, cy + (p0 ? p0 : 1)); break;
    case 'C': con_gotoxy(cx + (p0 ? p0 : 1), cy); break;
    case 'D': con_gotoxy(cx - (p0 ? p0 : 1), cy); break;
    case 'G': con_gotoxy((p0 ? p0 : 1) - 1, cy); break;
    case 'd': con_gotoxy(cx, (p0 ? p0 : 1) - 1); break;
    case 'J':
        if (p0 == 2 || p0 == 3) { for (r = 0; r < CON_ROWS; r++) clear_row(r, 0); if (p0 == 2) { cx = cy = 0; } }
        else if (p0 == 0) { clear_row(cy, cx); for (r = cy + 1; r < CON_ROWS; r++) clear_row(r, 0); }
        else if (p0 == 1) { for (r = 0; r < cy; r++) clear_row(r, 0); for (i = 0; i <= cx && i < cols; i++) grid[cy][i] = (cell_t)(' ' | (cur_attr() << 8)); mark(cy); }
        break;
    case 'K':
        if (p0 == 0) clear_row(cy, cx);
        else if (p0 == 1) { for (i = 0; i <= cx && i < cols; i++) grid[cy][i] = (cell_t)(' ' | (cur_attr() << 8)); mark(cy); }
        else clear_row(cy, 0);
        break;
    case 'm':
        if (esc_n == 0) sgr(0);
        for (i = 0; i < esc_n; i++) sgr(esc_par[i]);
        break;
    case 's': save_x = cx; save_y = cy; break;
    case 'u': con_gotoxy(save_x, save_y); break;
    case 'h': if (esc_priv && p0 == 25) con_cursor(1); break;
    case 'l': if (esc_priv && p0 == 25) con_cursor(0); break;
    default: break;
    }
}

static void put_raw(int c)
{
    if (cx >= cols) newline();
    grid[cy][cx] = (cell_t)((c & 0xFF) | (cur_attr() << 8));
    mark(cy);
    cx++;
}

static void put_char(int c);

void con_putc(int c)
{
    if (mirror) {
        if (c == '\n') hal_serial_write("\r\n", 2);
        else { char ch = (char)c; hal_serial_write(&ch, 1); }
    }
    put_char(c);
}

static void put_char(int c)
{
    c &= 0xFF;
    if (esc_state == 1) {
        if (c == '[') { esc_state = 2; esc_n = 0; esc_priv = 0; memset(esc_par, 0, sizeof esc_par); return; }
        esc_state = 0;
        if (c == 'c') { con_default_color(); con_clear(); }
        return;
    }
    if (esc_state == 2) {
        if (c >= '0' && c <= '9') {
            if (esc_n == 0) esc_n = 1;
            esc_par[esc_n - 1] = esc_par[esc_n - 1] * 10 + (c - '0');
            return;
        }
        if (c == ';') { if (esc_n == 0) esc_n = 1; if (esc_n < 8) esc_n++; return; }
        if (c == '?') { esc_priv = 1; return; }
        esc_state = 0;
        csi(c);
        return;
    }
    switch (c) {
    case 27: esc_state = 1; return;
    case '\n': newline(); return;
    case '\r': cx = 0; return;
    case '\b': if (cx > 0) { cx--; } return;
    case '\t': do put_raw(' '); while (cx % 4 && cx < cols); return;
    case 7: con_bell(); return;
    case 12: con_clear(); return;
    default:
        if (c < 32) return;
        put_raw(c);
    }
}

void con_write(const char *s, int n)
{
    int i;
    if (mirror) {
        /* the serial side wants CR LF */
        int start = 0;
        for (i = 0; i < n; i++) {
            if (s[i] == '\n') {
                if (i > start) hal_serial_write(s + start, i - start);
                hal_serial_write("\r\n", 2);
                start = i + 1;
            }
        }
        if (n > start) hal_serial_write(s + start, n - start);
    }
    for (i = 0; i < n; i++) put_char((unsigned char)s[i]);
}

void con_puts(const char *s) { con_write(s, (int)strlen(s)); }

int con_vprintf(const char *fmt, va_list ap)
{
    char buf[256];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    if (n > (int)sizeof buf - 1) n = sizeof buf - 1;
    if (n > 0) con_write(buf, n);
    return n;
}

int con_printf(const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = con_vprintf(fmt, ap);
    va_end(ap);
    return n;
}

void con_put_at(int x, int y, int ch, int a)
{
    if (x < 0 || y < 0 || x >= cols || y >= CON_ROWS) return;
    cell_t v = (cell_t)((ch & 0xFF) | ((a & 0xFF) << 8));
    if (grid[y][x] != v) { grid[y][x] = v; mark(y); }
}

void con_fill_rect(int x, int y, int w, int h, int ch, int a)
{
    int i, j;
    for (j = y; j < y + h; j++)
        for (i = x; i < x + w; i++) con_put_at(i, j, ch, a);
}

void con_text_at(int x, int y, const char *s, int a, int maxw)
{
    int i;
    for (i = 0; s[i] && i < maxw; i++) con_put_at(x + i, y, (unsigned char)s[i], a);
}

cell_t con_get_at(int x, int y)
{
    if (x < 0 || y < 0 || x >= cols || y >= CON_ROWS) return 0;
    return grid[y][x];
}

void con_place_cursor(int x, int y)
{
    if (place_y >= 0) mark(place_y);
    place_x = x;
    place_y = y;
    if (y >= 0) mark(y);
}

void con_flush(void)
{
    int r, crow, ccol;
    if (place_y >= 0) { crow = place_y; ccol = place_x; }
    else { crow = cy; ccol = cx < cols ? cx : cols - 1; }
    if (!cur_on || !blink_phase) crow = -1;
    if (crow != last_cur_row) {
        mark(last_cur_row);
        mark(crow);
        last_cur_row = crow;
    }
    for (r = 0; r < CON_ROWS; r++) {
        if (!dirty[r]) continue;
        dirty[r] = 0;
        hal_draw_row(r, grid[r], cols, r == crow ? ccol : -1, g_conf.cursor_style);
    }
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

static void kq_push(int k)
{
    int n = (kq_tail + 1) % 128;
    if (n == kq_head) return;
    kq[kq_tail] = k;
    kq_tail = n;
}

void con_unget(int k) { kq_push(k); }

static int kq_pop(void)
{
    int k;
    if (kq_head == kq_tail) return -1;
    k = kq[kq_head];
    kq_head = (kq_head + 1) % 128;
    return k;
}

static void key_click(void)
{
    if (g_conf.click && !g_conf.mute) hal_tone(2000, 2);
}

int con_inkey(void)
{
    int k = kq_pop();
    if (k >= 0) return k;
    k = hal_key();
    if (k >= 0) key_click();
    return k;
}

int con_kbhit(void)
{
    int k;
    if (kq_head != kq_tail) return 1;
    k = hal_key();
    if (k < 0) return 0;
    key_click();
    kq_push(k);
    return 1;
}

int con_poll_break(void)
{
    return hal_poll_break(conf_is_break, kq_push);
}

void hal_dump_screen(void) __attribute__((weak));

int con_getkey(void)
{
    int k;
    blink_phase = 1;
    blink_t = hal_millis();
    con_flush();
    if (place_y >= 0 && hal_dump_screen) hal_dump_screen();
    for (;;) {
        k = con_inkey();
        if (k >= 0) {
            if (!blink_phase) { blink_phase = 1; con_flush(); }
            return k;
        }
        if (hal_millis() - blink_t >= 500) {
            blink_t = hal_millis();
            blink_phase = !blink_phase;
            con_flush();
        }
        hal_sleep_ms(5);
    }
}

void con_bell(void)
{
    if (!g_conf.mute) hal_tone(880, 60);
}
