/* console.h - the text console: a grid of coloured cells with an ANSI parser.
 *
 * 32 rows of 10-pixel text, 40 columns with the 8x10 font or 64 with the
 * 5x10 font. Output is mirrored to USB serial.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdarg.h>
#include "hal.h"

void con_init(void);
void con_set_width(int cols);       /* 40 or 64 */
int  con_cols(void);
int  con_rows(void);

void con_putc(int c);
void con_puts(const char *s);
void con_write(const char *s, int n);
int  con_printf(const char *fmt, ...);
int  con_vprintf(const char *fmt, va_list ap);

void con_clear(void);
void con_clear_eol(void);
void con_gotoxy(int x, int y);
int  con_x(void);
int  con_y(void);
void con_set_color(int fg, int bg);
void con_set_fg(int fg);
void con_set_bg(int bg);
int  con_fg(void);
int  con_bg(void);
void con_default_color(void);       /* back to the configured theme */
void con_cursor(int on);
int  con_cursor_on(void);

/* direct cell access for full-screen programs (the editor) */
void con_put_at(int x, int y, int ch, int attr);
void con_fill_rect(int x, int y, int w, int h, int ch, int attr);
void con_text_at(int x, int y, const char *s, int attr, int maxw);
cell_t con_get_at(int x, int y);
void con_place_cursor(int x, int y);  /* cursor without moving the stream */

void con_flush(void);
void con_invalidate(void);
int  con_mirror(int on);            /* serial mirroring; returns the old state */

/* keyboard */
int  con_getkey(void);              /* blocking, flushes first */
int  con_inkey(void);               /* non-blocking, -1 if none */
int  con_kbhit(void);
void con_unget(int k);
int  con_poll_break(void);          /* 1 if Ctrl+C / BREAK was pressed */
void con_bell(void);

#define ATTR(fg, bg) ((((bg) & 15) << 4) | ((fg) & 15))

extern const uint16_t con_palette[16];

#endif
