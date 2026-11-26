/* lineedit.c - single-line editor used by the shell and by programs' stdin.
 *
 * Keys: Left/Right, Home/End (also Ctrl+A / Ctrl+E), Backspace, Del,
 * Up/Down for history, Tab for completion, Ctrl+U clears, Ctrl+K kills to
 * the end, Ctrl+C or Esc cancels. Long lines wrap onto following rows.
 */
#include "lineedit.h"
#include "console.h"
#include <string.h>
#include <stdlib.h>

LeComplete le_complete;
extern int con_scrolls;

void le_history_add(LeHistory *h, const char *line)
{
    int i;
    if (!h || !line[0]) return;
    if (h->max <= 0 || h->max > LE_HIST_MAX) h->max = LE_HIST_MAX;
    if (h->count && strcmp(h->lines[h->count - 1], line) == 0) return;
    if (h->count == h->max) {
        free(h->lines[0]);
        for (i = 1; i < h->count; i++) h->lines[i - 1] = h->lines[i];
        h->count--;
    }
    h->lines[h->count] = (char *)malloc(strlen(line) + 1);
    if (!h->lines[h->count]) return;
    strcpy(h->lines[h->count], line);
    h->count++;
}

void le_history_clear(LeHistory *h)
{
    int i;
    for (i = 0; i < h->count; i++) free(h->lines[i]);
    h->count = 0;
}

typedef struct {
    int sx, sy;          /* where the text starts */
    int shown;           /* characters drawn last time */
    int scroll_base;
} View;

static void redraw(View *v, const char *buf, int len, int pos)
{
    int cols = con_cols(), i, x, y;
    int sc0 = con_scrolls;
    con_gotoxy(v->sx, v->sy);
    for (i = 0; i < len; i++) con_putc((unsigned char)buf[i]);
    /* blank what is left of the previous, longer text */
    for (; i < v->shown; i++) con_putc(' ');
    if (con_scrolls != sc0) v->sy -= con_scrolls - sc0;
    if (v->sy < 0) v->sy = 0;
    v->shown = len;
    x = v->sx + pos;
    y = v->sy + x / cols;
    x = x % cols;
    if (y >= con_rows()) {
        /* the cursor sits just past the last cell: scroll it into view */
        con_gotoxy(cols - 1, con_rows() - 1);
        con_putc('\n');
        v->sy--;
        y = con_rows() - 1;
    }
    con_gotoxy(x, y);
}

int le_readline(char *buf, int max, const char *prompt, LeHistory *h)
{
    View v;
    int len = 0, pos = 0, k, hidx;
    char saved[256];
    int mir;
    if (max < 2) return -1;
    if (prompt && prompt[0]) con_puts(prompt);
    mir = con_mirror(0);          /* redraws are not echoed to serial */
    v.sx = con_x();
    v.sy = con_y();
    v.shown = 0;
    buf[0] = 0;
    hidx = h ? h->count : 0;
    saved[0] = 0;
    for (;;) {
        k = con_getkey();
        if (k == '\r' || k == '\n') {
            buf[len] = 0;
            pos = len;
            redraw(&v, buf, len, pos);
            /* the serial side only sees the line once it is complete */
            con_putc('\n');
            con_mirror(mir);
            if (mir) { hal_serial_write(buf, len); hal_serial_write("\r\n", 2); }
            con_flush();
            return len;
        }
        if (k == KEY_CTRL('c') || k == KEY_BREAK || (k == KEY_ESC && len == 0)) {
            con_mirror(mir);
            con_puts("^C\n");
            buf[0] = 0;
            return -1;
        }
        if (k == KEY_ESC || k == KEY_CTRL('u')) {
            len = pos = 0;
        } else if (k == KEY_LEFT) {
            if (pos > 0) pos--;
        } else if (k == KEY_RIGHT) {
            if (pos < len) pos++;
        } else if (k == KEY_HOME || k == KEY_CTRL('a')) {
            pos = 0;
        } else if (k == KEY_END || k == KEY_CTRL('e')) {
            pos = len;
        } else if (k == '\b' || k == 127) {
            if (pos > 0) {
                memmove(buf + pos - 1, buf + pos, len - pos);
                pos--;
                len--;
            }
        } else if (k == KEY_DEL) {
            if (pos < len) {
                memmove(buf + pos, buf + pos + 1, len - pos - 1);
                len--;
            }
        } else if (k == KEY_CTRL('k')) {
            len = pos;
        } else if ((k == KEY_UP || k == KEY_DOWN) && h && h->count) {
            if (hidx == h->count) {
                buf[len] = 0;
                strncpy(saved, buf, sizeof saved - 1);
                saved[sizeof saved - 1] = 0;
            }
            if (k == KEY_UP && hidx > 0) hidx--;
            else if (k == KEY_DOWN && hidx < h->count) hidx++;
            else continue;
            strncpy(buf, hidx < h->count ? h->lines[hidx] : saved, max - 1);
            buf[max - 1] = 0;
            len = pos = (int)strlen(buf);
        } else if (k == '\t' && le_complete) {
            char out[256];
            int np;
            buf[len] = 0;
            np = le_complete(buf, pos, out, sizeof out);
            if (np >= 0) {
                strncpy(buf, out, max - 1);
                buf[max - 1] = 0;
                len = (int)strlen(buf);
                pos = np > len ? len : np;
            } else if (np == -2) {
                /* candidates were listed: start a fresh prompt line */
                if (prompt && prompt[0]) con_puts(prompt);
                v.sx = con_x();
                v.sy = con_y();
                v.shown = 0;
            }
        } else if (k >= 32 && k < 127) {
            if (len < max - 1) {
                memmove(buf + pos + 1, buf + pos, len - pos);
                buf[pos] = (char)k;
                pos++;
                len++;
            }
        } else {
            continue;
        }
        buf[len] = 0;
        redraw(&v, buf, len, pos);
    }
}
