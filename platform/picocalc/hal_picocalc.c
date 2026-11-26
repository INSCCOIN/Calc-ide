/* hal_picocalc.c - CalcIDE on the ClockworkPi PicoCalc (RP2350 or RP2040).
 *
 * Display: 32 rows of 10 pixels; each row is rendered into one 320x10 line
 * buffer (6.4 KB) and sent in a single SPI transaction. 40 columns use the
 * 8x10 font, 64 columns the 5x10 font, so both fill the 320-pixel line.
 *
 * Input comes from the keyboard controller and, at the same time, from the
 * USB serial port (with VT100 escape sequences decoded), so the machine can
 * be driven from a PC terminal. All console output is mirrored to USB.
 */
#include "hal.h"
#include "picocalc.h"
#include "config.h"
#include "console.h"

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/watchdog.h"
#include "hardware/pwm.h"
#include "hardware/adc.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/gpio.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

extern const unsigned char font_8x10[256][10];
extern const unsigned char font_5x10[256][10];

static int font_sel;
static uint16_t linebuf[LCD_W * 10];
static int esc_boot;                 /* Esc held at power-on: safe mode */

/* ------------------------------------------------------------------ */
/* time                                                                */
/* ------------------------------------------------------------------ */

uint32_t hal_millis(void) { return to_ms_since_boot(get_absolute_time()); }
uint64_t hal_micros(void) { return to_us_since_boot(get_absolute_time()); }
void hal_sleep_ms(int ms) { if (ms > 0) sleep_ms((uint32_t)ms); }

/* ------------------------------------------------------------------ */
/* display                                                             */
/* ------------------------------------------------------------------ */

static LcdCfg lcd_cfg(void)
{
    LcdCfg c;
    c.bits18 = g_conf.lcd_bits == 18;
    c.madctl = g_conf.lcd_madctl;
    c.invert = g_conf.lcd_invert;
    c.spi_mhz = 0;
    return c;
}

void hal_set_font(int f) { font_sel = f ? 1 : 0; }

void hal_draw_row(int row, const cell_t *cells, int cols, int cursor_col, int style)
{
    const unsigned char (*font)[10] = (cols >= 64) ? font_5x10 : font_8x10;
    int cw = (cols >= 64) ? 5 : 8, c, y, x;
    if (row < 0 || row >= CON_ROWS) return;
    for (c = 0; c < cols; c++) {
        int ch = cells[c] & 0xFF, a = cells[c] >> 8;
        uint16_t fg = con_palette[a & 15], bg = con_palette[(a >> 4) & 15];
        const unsigned char *g = font[ch];
        int is_cur = (c == cursor_col);
        if (is_cur && style == 1) { uint16_t t = fg; fg = bg; bg = t; }
        for (y = 0; y < 10; y++) {
            unsigned bits = g[y];
            uint16_t *o = &linebuf[y * LCD_W + c * cw];
            if (is_cur && style == 0 && y >= 8) bits = 0xFF;
            for (x = 0; x < cw; x++) o[x] = (bits >> x) & 1 ? fg : bg;
        }
    }
    lcd_window(0, row * 10, LCD_W, 10);
    lcd_blit(linebuf, LCD_W * 10);
}

void hal_pixel(int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= LCD_W || y >= LCD_H) return;
    lcd_window(x, y, 1, 1);
    lcd_blit(&c, 1);
}

void hal_fill(int x, int y, int w, int h, uint16_t c) { lcd_fill(x, y, w, h, c); }

void hal_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int e = dx + dy, guard = 0;
    for (;;) {
        hal_pixel(x0, y0, c);
        if ((x0 == x1 && y0 == y1) || ++guard > 2000) break;
        {
            int e2 = 2 * e;
            if (e2 >= dy) { e += dy; x0 += sx; }
            if (e2 <= dx) { e += dx; y0 += sy; }
        }
    }
}

/* ------------------------------------------------------------------ */
/* USB serial                                                          */
/* ------------------------------------------------------------------ */

void hal_serial_write(const char *s, int n)
{
    if (n > 0) stdio_put_string(s, n, false, false);
}

/* VT100 decoding of serial input */
static int ser_peek(uint32_t wait_us)
{
    int c = getchar_timeout_us(wait_us);
    return c == PICO_ERROR_TIMEOUT ? -1 : c;
}

static int serial_key(void)
{
    int c = ser_peek(0), d, e, n;
    if (c < 0) return -1;
    if (c == '\r') return '\n';
    if (c == 127) return '\b';
    if (c != 27) return c;
    d = ser_peek(20000);
    if (d < 0) return KEY_ESC;
    if (d == 'O') {
        e = ser_peek(20000);
        if (e >= 'P' && e <= 'S') return KEY_F1 + (e - 'P');
        return KEY_ESC;
    }
    if (d != '[') return KEY_ESC;
    e = ser_peek(20000);
    switch (e) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    }
    if (e < '0' || e > '9') return KEY_ESC;
    n = e - '0';
    while ((e = ser_peek(20000)) >= '0' && e <= '9') n = n * 10 + (e - '0');
    if (e != '~') return -1;
    switch (n) {
    case 1: case 7: return KEY_HOME;
    case 2: return KEY_INS;
    case 3: return KEY_DEL;
    case 4: case 8: return KEY_END;
    case 5: return KEY_PGUP;
    case 6: return KEY_PGDN;
    case 11: case 12: case 13: case 14: case 15: return KEY_F1 + (n == 15 ? 4 : n - 11);
    case 17: case 18: case 19: case 20: case 21: return KEY_F6 + (n - 17);
    }
    return -1;
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

static uint32_t last_kbd_us;

int hal_key(void)
{
    int k = serial_key();
    uint64_t now;
    if (k >= 0) return k;
    /* the I2C controller is slow; do not hammer it */
    now = hal_micros();
    if ((uint32_t)now - last_kbd_us < 4000) return -1;
    last_kbd_us = (uint32_t)now;
    k = kbd_poll();
    return k ? k : -1;
}

int hal_poll_break(int (*is_break)(int key), void (*queue)(int key))
{
    int k, guard = 0;
    while ((k = hal_key()) >= 0 && guard++ < 16) {
        if (is_break(k)) return 1;
        queue(k);
    }
    return 0;
}

void hal_blit(int x, int y, int w, int h, const uint16_t *px)
{
    if (w <= 0 || h <= 0) return;
    lcd_window(x, y, w, h);
    lcd_blit(px, w * h);
}

int hal_modifiers(void) { return kbd_modifiers(); }

/* ------------------------------------------------------------------ */
/* sound: GP26 (left) and GP27 (right)                                */
/* ------------------------------------------------------------------ */

#define SND_L 26
#define SND_R 27

static int snd_ready;

void hal_tone_off(void)
{
    if (!snd_ready) return;
    pwm_set_enabled(pwm_gpio_to_slice_num(SND_L), false);
    pwm_set_enabled(pwm_gpio_to_slice_num(SND_R), false);
}

void hal_tone(int hz, int ms)
{
    uint slice_l, slice_r;
    uint32_t clk, div_q4, top, level;
    if (hz <= 0) { hal_tone_off(); if (ms > 0) sleep_ms((uint32_t)ms); return; }
    if (hz < 20) hz = 20;
    if (hz > 20000) hz = 20000;
    if (!snd_ready) {
        gpio_set_function(SND_L, GPIO_FUNC_PWM);
        gpio_set_function(SND_R, GPIO_FUNC_PWM);
        snd_ready = 1;
    }
    clk = (uint32_t)clock_get_hz(clk_sys);
    div_q4 = (uint32_t)(((uint64_t)clk * 16) / ((uint64_t)hz * 65536u)) + 1;
    if (div_q4 < 16) div_q4 = 16;
    if (div_q4 > 255 * 16 + 15) div_q4 = 255 * 16 + 15;
    top = (uint32_t)(((uint64_t)clk * 16) / ((uint64_t)hz * div_q4));
    if (top < 2) top = 2;
    if (top > 65535) top = 65535;
    level = top / 2 * (uint32_t)(g_conf.volume > 0 ? g_conf.volume : 1) / 100u;
    if (level < 1) level = 1;
    slice_l = pwm_gpio_to_slice_num(SND_L);
    slice_r = pwm_gpio_to_slice_num(SND_R);
    pwm_set_clkdiv_int_frac(slice_l, (uint8_t)(div_q4 >> 4), div_q4 & 15);
    pwm_set_clkdiv_int_frac(slice_r, (uint8_t)(div_q4 >> 4), div_q4 & 15);
    pwm_set_wrap(slice_l, (uint16_t)top);
    pwm_set_wrap(slice_r, (uint16_t)top);
    pwm_set_gpio_level(SND_L, (uint16_t)level);
    pwm_set_gpio_level(SND_R, (uint16_t)level);
    /* both speakers in phase, as in the ClockworkPi pwm_sound example */
    pwm_set_output_polarity(slice_l, false, false);
    if (slice_r != slice_l) pwm_set_output_polarity(slice_r, false, false);
    pwm_set_enabled(slice_l, true);
    pwm_set_enabled(slice_r, true);
    if (ms > 0) {
        if (ms > 10000) ms = 10000;
        sleep_ms((uint32_t)ms);
        hal_tone_off();
    }
}

/* ------------------------------------------------------------------ */
/* power, clocks, sensors                                              */
/* ------------------------------------------------------------------ */

void hal_backlight(int level) { kbd_set_backlight(level, g_conf.bl_swap); }
void hal_kbdlight(int level) { kbd_set_kbd_backlight(level, g_conf.bl_swap); }

int hal_battery(int *charging)
{
    int ch = 0, p = kbd_battery(&ch);
    if (charging) *charging = ch;
    return p;
}

int hal_cpu_khz(void) { return (int)(clock_get_hz(clk_sys) / 1000u); }

#if PICO_RP2350
#define DEFAULT_MHZ 150
#else
#define DEFAULT_MHZ 125
#endif

int hal_set_cpu_mhz(int mhz)
{
    int was = hal_cpu_khz() / 1000;
    if (esc_boot) return 0;
    if (mhz == 0) mhz = DEFAULT_MHZ;           /* 0: the chip's own default */
    if (mhz < 48 || mhz > 300) return 0;
#if !PICO_RP2350
    if (mhz > 250) return 0;
#endif
    if (mhz == was) return 1;
    /* raise the core voltage before speeding up */
    if (mhz > DEFAULT_MHZ + 8) { vreg_set_voltage(mhz > 250 ? VREG_VOLTAGE_1_25 : VREG_VOLTAGE_1_15); sleep_ms(5); }
    if (!set_sys_clock_khz((uint32_t)mhz * 1000u, false)) return 0;
    /* and lower it again after slowing down */
    if (mhz <= DEFAULT_MHZ + 8) vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
    /* clk_peri follows clk_sys: re-time the buses */
    lcd_reclock();
    sd_reclock();
    kbd_reclock();
    return 1;
}

float hal_temp_c(void)
{
    static int inited;
    uint32_t sum = 0;
    int i;
    float v;
    if (!inited) {
        adc_init();
        adc_set_temp_sensor_enabled(true);
        inited = 1;
    }
    adc_select_input(ADC_TEMPERATURE_CHANNEL_NUM);
    for (i = 0; i < 16; i++) sum += adc_read();
    v = (float)sum / 16.0f * 3.3f / 4096.0f;
    return 27.0f - (v - 0.706f) / 0.001721f;
}

extern char end;
extern char __StackLimit;

size_t hal_ram_total(void)
{
#if PICO_RP2350
    return 520u * 1024u;
#else
    return 264u * 1024u;
#endif
}

size_t hal_ram_free(void)
{
    struct mallinfo mi = mallinfo();
    size_t never = (size_t)(&__StackLimit - &end) - (size_t)mi.arena;
    return never + (size_t)mi.fordblks;
}

size_t hal_ram_largest(void)
{
    /* binary search for the largest block malloc will hand out */
    size_t lo = 0, hi = hal_ram_free() + 4096;
    while (hi - lo > 1024) {
        size_t mid = (lo + hi) / 2;
        void *p = malloc(mid);
        if (p) { free(p); lo = mid; }
        else hi = mid;
    }
    return lo;
}

const char *hal_chip(void)
{
#if PICO_RP2350
    return "RP2350";
#else
    return "RP2040";
#endif
}

void hal_reboot(void)
{
    hal_tone_off();
    con_flush();
    sleep_ms(100);
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

void hal_poweroff(void)
{
    hal_tone_off();
    con_flush();
    sleep_ms(100);
    kbd_set_backlight(0, g_conf.bl_swap);
    kbd_set_kbd_backlight(0, g_conf.bl_swap);
    lcd_sleep();
    kbd_poweroff();
    for (;;) __wfi();
}

void hal_reboot_to_bootloader(void)
{
    con_flush();
    sleep_ms(100);
    reset_usb_boot(0, 0);
}

/* ------------------------------------------------------------------ */
/* the page file: the top 768 KB of flash                              */
/* ------------------------------------------------------------------ */

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif
#define PF_FLASH_BYTES (768 * 1024)
#define PF_OFFSET (PICO_FLASH_SIZE_BYTES - PF_FLASH_BYTES)
#define PF_PAGES (PF_FLASH_BYTES / FLASH_SECTOR_SIZE)

extern char __flash_binary_end;
static int pf_ok = -1;

static int pf_check(void)
{
    if (pf_ok < 0) {
        uintptr_t fw_end = (uintptr_t)&__flash_binary_end;
        uintptr_t first = XIP_BASE + PF_OFFSET;
        pf_ok = (fw_end + FLASH_SECTOR_SIZE <= first) ? 1 : 0;   /* never overwrite the firmware */
    }
    return pf_ok;
}

int hal_pf_pages(void) { return pf_check() ? PF_PAGES : 0; }
int hal_pf_page_size(void) { return FLASH_SECTOR_SIZE; }

const void *hal_pf_read(int page)
{
    if (!pf_check() || page < 0 || page >= PF_PAGES) return 0;
    return (const void *)(XIP_BASE + PF_OFFSET + (uint32_t)page * FLASH_SECTOR_SIZE);
}

int hal_pf_write(int page, const void *data, int len)
{
    static uint8_t buf[FLASH_SECTOR_SIZE];
    uint32_t off, ints;
    if (!pf_check() || page < 0 || page >= PF_PAGES || len < 0 || len > FLASH_SECTOR_SIZE) return 0;
    memset(buf, 0xFF, sizeof buf);
    if (len) memcpy(buf, data, (size_t)len);
    off = (uint32_t)PF_OFFSET + (uint32_t)page * FLASH_SECTOR_SIZE;
    ints = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    flash_range_program(off, buf, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    return memcmp((const void *)(XIP_BASE + off), buf, FLASH_SECTOR_SIZE) == 0;
}

int hal_pf_erase(int page)
{
    uint32_t off, ints;
    if (!pf_check() || page < 0 || page >= PF_PAGES) return 0;
    off = (uint32_t)PF_OFFSET + (uint32_t)page * FLASH_SECTOR_SIZE;
    ints = save_and_disable_interrupts();
    flash_range_erase(off, FLASH_SECTOR_SIZE);
    restore_interrupts(ints);
    return 1;
}

int hal_sd_present(void) { return sd_ready(); }
int hal_sd_hc(void) { return sd_is_hc(); }

/* ------------------------------------------------------------------ */
/* start-up                                                            */
/* ------------------------------------------------------------------ */

void hal_init(void)
{
    LcdCfg c;
    int i;
    stdio_init_all();
    kbd_init();
    /* Esc held at power-on: keep the default clock and skip autoexec */
    for (i = 0; i < 40; i++) {
        int k = kbd_poll();
        if (k == KEY_ESC) { esc_boot = 1; break; }
        sleep_ms(5);
    }
    c = lcd_cfg();
    lcd_init(&c);
    lcd_fill(0, 0, LCD_W, LCD_H, con_palette[g_conf.bg]);
    kbd_set_backlight(g_conf.backlight, g_conf.bl_swap);
    kbd_set_kbd_backlight(g_conf.kbd_light, g_conf.bl_swap);
}

int hal_safe_mode(void) { return esc_boot; }
int hal_batch(void) { return 0; }

/* The SDK gives core 0 a 4 KB stack in scratch RAM. The compiler, the VM
 * and printf together want more, so main() moves onto a bigger one. MSPLIM
 * is left at 0 by crt0, so nothing else needs to change. */
#if PICO_RP2350
#define BIG_STACK_WORDS (24 * 1024 / 4)
#else
#define BIG_STACK_WORDS (12 * 1024 / 4)
#endif
static uint32_t big_stack[BIG_STACK_WORDS] __attribute__((aligned(8)));

int calcide_main(void);

static void __attribute__((noinline, noreturn)) run_on_big_stack(void)
{
    calcide_main();
    for (;;) tight_loop_contents();
}

int main(void)
{
    __asm volatile("msr msp, %0" : : "r"(big_stack + BIG_STACK_WORDS) : "memory");
    run_on_big_stack();
}

/* the settings file may change the panel parameters */
void hal_reconfigure(void)
{
    static LcdCfg applied;
    static int have;
    LcdCfg c = lcd_cfg();
    if (!have) { applied.bits18 = 0; applied.madctl = 0x48; applied.invert = 1; applied.spi_mhz = 0; have = 1; }
    if (c.bits18 != applied.bits18 || c.madctl != applied.madctl || c.invert != applied.invert) {
        lcd_init(&c);
        applied = c;
        con_invalidate();
    }
    lcd_fill(0, 0, LCD_W, LCD_H, con_palette[g_conf.bg]);
}

/* ------------------------------------------------------------------ */
/* PCM audio: the speaker PWM at a 256-step, ~586 kHz carrier          */
/* ------------------------------------------------------------------ */

void hal_pcm_start(int rate)
{
    uint slice_l = pwm_gpio_to_slice_num(SND_L), slice_r = pwm_gpio_to_slice_num(SND_R);
    (void)rate;
    gpio_set_function(SND_L, GPIO_FUNC_PWM);
    gpio_set_function(SND_R, GPIO_FUNC_PWM);
    snd_ready = 1;
    pwm_set_clkdiv_int_frac(slice_l, 1, 0);
    pwm_set_clkdiv_int_frac(slice_r, 1, 0);
    pwm_set_wrap(slice_l, 255);
    pwm_set_wrap(slice_r, 255);
    pwm_set_output_polarity(slice_l, false, false);
    if (slice_r != slice_l) pwm_set_output_polarity(slice_r, false, false);
    pwm_set_gpio_level(SND_L, 128);
    pwm_set_gpio_level(SND_R, 128);
    pwm_set_enabled(slice_l, true);
    pwm_set_enabled(slice_r, true);
}

void hal_pcm_write(uint8_t v)
{
    pwm_set_gpio_level(SND_L, v);
    pwm_set_gpio_level(SND_R, v);
}

void hal_pcm_stop(void) { hal_tone_off(); }

/* ------------------------------------------------------------------ */
/* GPIO and UART for programs                                          */
/* ------------------------------------------------------------------ */

/* GP6/7 keyboard, GP10-15 LCD, GP16-19 SD card, GP26/27 sound, and
 * GP23-25/29 belong to the Pico module itself (power, LED, Wi-Fi). */
int hal_gpio_free(int pin)
{
    if (pin < 0 || pin > 29) return 0;
    if (pin == 6 || pin == 7 || (pin >= 10 && pin <= 19) || pin == 26 || pin == 27) return 0;
    if (pin >= 23 && pin <= 25) return 0;
    if (pin == 29) return 0;
    return 1;
}

int hal_gpio_mode(int pin, int mode)
{
    if (!hal_gpio_free(pin)) return 0;
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, mode == HAL_GPIO_OUT);
    gpio_disable_pulls((uint)pin);
    if (mode == HAL_GPIO_PULLUP) gpio_pull_up((uint)pin);
    if (mode == HAL_GPIO_PULLDOWN) gpio_pull_down((uint)pin);
    return 1;
}

int hal_gpio_write(int pin, int v)
{
    if (!hal_gpio_free(pin)) return 0;
    gpio_put((uint)pin, v != 0);
    return 1;
}

int hal_gpio_read(int pin)
{
    if (!hal_gpio_free(pin)) return -1;
    return gpio_get((uint)pin) ? 1 : 0;
}

#include "hardware/uart.h"

static uart_inst_t *uart_of(int port) { return port == 0 ? uart0 : port == 1 ? uart1 : 0; }

int hal_uart_open(int port, int baud)
{
    uart_inst_t *u = uart_of(port);
    int tx = port == 0 ? 0 : 4, rx = port == 0 ? 1 : 5;
    if (!u || baud < 300 || baud > 3000000) return 0;
    uart_init(u, (uint)baud);
    gpio_set_function((uint)tx, GPIO_FUNC_UART);
    gpio_set_function((uint)rx, GPIO_FUNC_UART);
    return 1;
}

void hal_uart_close(int port)
{
    uart_inst_t *u = uart_of(port);
    if (u) uart_deinit(u);
}

int hal_uart_putc(int port, int c)
{
    uart_inst_t *u = uart_of(port);
    if (!u) return -1;
    uart_putc_raw(u, (char)c);
    return c & 255;
}

int hal_uart_getc(int port)
{
    uart_inst_t *u = uart_of(port);
    if (!u || !uart_is_readable(u)) return -1;
    return (uint8_t)uart_getc(u);
}

int hal_uart_available(int port)
{
    uart_inst_t *u = uart_of(port);
    return u && uart_is_readable(u) ? 1 : 0;
}
