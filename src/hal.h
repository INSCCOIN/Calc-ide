/* hal.h - the hardware abstraction layer.
 *
 * Everything above this line (console, shell, editor, compiler, VM) is
 * portable C. Two implementations exist: platform/picocalc (the real board)
 * and platform/posix (a terminal build used for development and tests).
 */
#ifndef HAL_H
#define HAL_H

#include <stdint.h>
#include <stddef.h>

/* key codes above ASCII (same values as the PicoCalc keyboard controller) */
enum {
    KEY_F1 = 0x81, KEY_F2 = 0x82, KEY_F3 = 0x83, KEY_F4 = 0x84, KEY_F5 = 0x85,
    KEY_F6 = 0x86, KEY_F7 = 0x87, KEY_F8 = 0x88, KEY_F9 = 0x89, KEY_F10 = 0x8A,
    KEY_ESC = 0xB1, KEY_LEFT = 0xB4, KEY_UP = 0xB5, KEY_DOWN = 0xB6,
    KEY_RIGHT = 0xB7, KEY_BREAK = 0xD0, KEY_INS = 0xD1, KEY_HOME = 0xD2,
    KEY_DEL = 0xD4, KEY_END = 0xD5, KEY_PGUP = 0xD6, KEY_PGDN = 0xD7,
    KEY_CAPS = 0xC1,
    /* synthesised: shift / ctrl + arrows */
    KEY_SUP = 0xE0, KEY_SDOWN = 0xE1, KEY_SLEFT = 0xE2, KEY_SRIGHT = 0xE3,
    KEY_CHOME = 0xE4, KEY_CEND = 0xE5
};
#define KEY_CTRL(c) ((c) & 0x1F)

/* display geometry */
#define LCD_W 320
#define LCD_H 320
#define CON_ROWS 32           /* 10-pixel rows */
#define CON_MAXCOLS 64

/* a console cell: low byte character, high byte attribute (bg<<4 | fg) */
typedef uint16_t cell_t;

void     hal_init(void);
uint32_t hal_millis(void);
uint64_t hal_micros(void);
void     hal_sleep_ms(int ms);

int  hal_key(void);                     /* -1 if nothing is waiting */
/* drain the keyboard: 1 if Ctrl+C / BREAK was pressed; other keys go to queue() */
int  hal_poll_break(void (*queue)(int key));
int  hal_modifiers(void);

/* draw one text row; cursor_col < 0 means no cursor on this row */
void hal_draw_row(int row, const cell_t *cells, int cols, int cursor_col, int cursor_style);
void hal_set_font(int font);            /* 0 = 8x10 (40 cols), 1 = 5x10 (64 cols) */
void hal_pixel(int x, int y, uint16_t rgb565);
void hal_fill(int x, int y, int w, int h, uint16_t rgb565);
void hal_line(int x0, int y0, int x1, int y1, uint16_t rgb565);
void hal_serial_write(const char *s, int n);  /* USB serial mirror */

void hal_tone(int hz, int ms);          /* ms <= 0: start and return */
void hal_tone_off(void);

void hal_backlight(int level);
void hal_kbdlight(int level);
int  hal_battery(int *charging);        /* percent, or -1 */
int  hal_cpu_khz(void);
int  hal_set_cpu_mhz(int mhz);          /* returns 1 on success */
float hal_temp_c(void);

size_t hal_ram_total(void);
size_t hal_ram_free(void);
size_t hal_ram_largest(void);
const char *hal_chip(void);             /* "RP2350" / "RP2040" / "host" */

void hal_reboot(void);
void hal_poweroff(void);
void hal_reboot_to_bootloader(void);

/* flash page file (4 KB pages) */
int         hal_pf_pages(void);
int         hal_pf_page_size(void);
const void *hal_pf_read(int page);
int         hal_pf_write(int page, const void *data, int len);
int         hal_pf_erase(int page);

/* SD card */
int  hal_sd_present(void);
int  hal_sd_hc(void);                   /* 1: SDHC/SDXC, 0: SDSC */

#endif
