/* hal_posix.c - CalcIDE on a Linux/macOS terminal, for development and tests.
 *
 * Interactive mode draws the 32-row console grid on the terminal with ANSI
 * colours. Batch mode (CALCIDE_BATCH=1, or stdin not a terminal) draws
 * nothing and lets the console's serial mirror write to stdout, which is
 * what the test suite reads.
 */
#define _DEFAULT_SOURCE
#include "hal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/select.h>
#include <fcntl.h>

static int batch;
static int raw_on;
static struct termios saved_tio;
static uint64_t t_start;
static int font;
static uint8_t *pf_mem;
static const char *pf_path;

#define PF_PAGES 192
#define PF_PSIZE 4096

static uint64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
}

static void restore(void)
{
    if (raw_on) {
        tcsetattr(0, TCSANOW, &saved_tio);
        fputs("\033[0m\033[?25h\n", stdout);
        fflush(stdout);
        raw_on = 0;
    }
}

int hal_is_batch(void) { return batch; }
int hal_batch(void) { return batch; }
void hal_reconfigure(void) { }
/* scripted runs skip the boot screen (it would eat the first key) */
int hal_safe_mode(void) { return batch && !getenv("CALCIDE_SPLASH"); }

static void grid_dump(void);
static void fb_dump(void);
static void at_exit_grid(void) { grid_dump(); }

void hal_init(void)
{
    const char *b = getenv("CALCIDE_BATCH");
    t_start = now_us();
    batch = (b && *b == '1') || !isatty(0) || !isatty(1);
    if (!batch) {
        struct termios t;
        tcgetattr(0, &saved_tio);
        t = saved_tio;
        t.c_lflag &= ~(ICANON | ECHO | ISIG);
        t.c_iflag &= ~(IXON | ICRNL);
        t.c_cc[VMIN] = 0;
        t.c_cc[VTIME] = 0;
        tcsetattr(0, TCSANOW, &t);
        raw_on = 1;
        atexit(restore);
        fputs("\033[2J\033[?25l", stdout);
    }
    if (getenv("CALCIDE_GRID")) atexit(at_exit_grid);
    if (getenv("CALCIDE_FB")) atexit(fb_dump);
    pf_path = getenv("CALCIDE_PF");
    if (!pf_path) pf_path = "pagefile.bin";
    pf_mem = (uint8_t *)malloc(PF_PAGES * PF_PSIZE);
    memset(pf_mem, 0xFF, PF_PAGES * PF_PSIZE);
    {
        FILE *f = fopen(pf_path, "rb");
        if (f) { if (fread(pf_mem, 1, PF_PAGES * PF_PSIZE, f)) { } fclose(f); }
    }
}

uint32_t hal_millis(void) { return (uint32_t)((now_us() - t_start) / 1000u); }
uint64_t hal_micros(void) { return now_us() - t_start; }
void hal_sleep_ms(int ms) { if (ms > 0) usleep((useconds_t)ms * 1000u); }

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

static int rd_byte(int wait_ms)
{
    unsigned char c;
    fd_set fs;
    struct timeval tv;
    FD_ZERO(&fs);
    FD_SET(0, &fs);
    tv.tv_sec = 0;
    tv.tv_usec = wait_ms * 1000;
    if (select(1, &fs, 0, 0, &tv) <= 0) return -1;
    if (read(0, &c, 1) != 1) return -2;
    return c;
}

static int eof_seen;

/* USB serial: CALCIDE_USB names a tty (e.g. a pty slave) to use as the port */
static int usb_fd = -2;
static int usb_open(void)
{
    if (usb_fd == -2) {
        const char *p = getenv("CALCIDE_USB");
        usb_fd = p ? open(p, O_RDWR | O_NOCTTY) : -1;
        if (usb_fd >= 0) {
            struct termios t;
            if (tcgetattr(usb_fd, &t) == 0) { cfmakeraw(&t); tcsetattr(usb_fd, TCSANOW, &t); }
        }
    }
    return usb_fd;
}

int hal_usb_getc(int timeout_ms)
{
    fd_set fs;
    struct timeval tv;
    unsigned char c;
    int fd = usb_open();
    if (fd < 0) { if (timeout_ms > 0) usleep((useconds_t)timeout_ms * 1000); return -1; }
    FD_ZERO(&fs);
    FD_SET(fd, &fs);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(fd + 1, &fs, 0, 0, &tv) <= 0) return -1;
    if (read(fd, &c, 1) != 1) return -1;
    return c;
}

void hal_usb_write(const void *buf, int n)
{
    const char *p = (const char *)buf;
    int fd = usb_open();
    while (fd >= 0 && n > 0) {
        ssize_t w = write(fd, p, (size_t)n);
        if (w <= 0) break;
        p += w;
        n -= (int)w;
    }
}

int hal_kbd_key(void) { return batch ? -1 : hal_key(); }

int hal_key(void)
{
    int c, d, e;
    c = rd_byte(0);
    if (c == -2) {
        /* scripted input finished: leave, as if the user switched off */
        eof_seen = 1;
        fflush(stdout);
        exit(0);
    }
    if (c < 0) return -1;
    if (c == '\r') return '\n';
    if (c == 127) return '\b';
    if (c != 27) return c;
    d = rd_byte(30);
    if (d < 0) return KEY_ESC;
    if (d == 'O') {
        e = rd_byte(30);
        if (e >= 'P' && e <= 'S') return KEY_F1 + (e - 'P');
        if (e == 'H') return KEY_HOME;
        if (e == 'F') return KEY_END;
        return KEY_ESC;
    }
    if (d != '[') return KEY_ESC;
    e = rd_byte(30);
    switch (e) {
    case 'A': return KEY_UP;
    case 'B': return KEY_DOWN;
    case 'C': return KEY_RIGHT;
    case 'D': return KEY_LEFT;
    case 'H': return KEY_HOME;
    case 'F': return KEY_END;
    case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': {
        int n = e - '0', f;
        while ((f = rd_byte(30)) >= '0' && f <= '9') n = n * 10 + (f - '0');
        if (f == ';') {
            int mod = rd_byte(30), dir = rd_byte(30);
            (void)mod;
            if (dir == 'A') return KEY_SUP;
            if (dir == 'B') return KEY_SDOWN;
            if (dir == 'C') return KEY_SRIGHT;
            if (dir == 'D') return KEY_SLEFT;
            if (dir == 'H') return KEY_CHOME;
            if (dir == 'F') return KEY_CEND;
            return -1;
        }
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
    }
    return KEY_ESC;
}

int hal_modifiers(void) { return 0; }

int hal_poll_break(int (*is_break)(int key), void (*queue)(int key))
{
    int k;
    if (batch) return 0;   /* scripted input is only read on demand */
    while ((k = hal_key()) >= 0) {
        if (is_break(k)) return 1;
        queue(k);
    }
    return 0;
}

void hal_blit(int x, int y, int w, int h, const uint16_t *px)
{
    int i, j;
    for (j = 0; j < h; j++) for (i = 0; i < w; i++) hal_pixel(x + i, y + j, px[j * w + i]);
}

int hal_eof(void) { return eof_seen; }

/* ------------------------------------------------------------------ */
/* display                                                             */
/* ------------------------------------------------------------------ */

static const int ansi_fg[16] = {30, 31, 32, 33, 34, 35, 36, 37, 90, 91, 92, 93, 94, 95, 96, 97};
static const int ansi_bg[16] = {40, 41, 42, 43, 44, 45, 46, 47, 100, 101, 102, 103, 104, 105, 106, 107};

/* batch mode keeps a copy of the screen text; CALCIDE_SCREEN=1 prints it
 * whenever the editor waits for a key after drawing (used by the tests) */
static char shadow[CON_ROWS][CON_MAXCOLS + 1];
static cell_t shadow_cells[CON_ROWS][CON_MAXCOLS];
static int shadow_cols = 40;
static int shadow_dirty;

/* CALCIDE_GRID=FILE writes the cells (for tools/screenshot.py) */
static void grid_dump(void)
{
    const char *g = getenv("CALCIDE_GRID");
    FILE *f;
    int r, c;
    if (!g || !(f = fopen(g, "w"))) return;
    fprintf(f, "%d\n", shadow_cols);
    for (r = 0; r < CON_ROWS; r++) {
        for (c = 0; c < shadow_cols; c++) fprintf(f, "%04x ", shadow_cells[r][c]);
        fputc('\n', f);
    }
    fclose(f);
}

void hal_dump_screen(void)
{
    int r;
    grid_dump();
    if (!shadow_dirty || !getenv("CALCIDE_SCREEN")) return;
    shadow_dirty = 0;
    fputs("\n=== screen ===\n", stdout);
    for (r = 0; r < CON_ROWS; r++) {
        int e = (int)strlen(shadow[r]);
        while (e > 0 && shadow[r][e - 1] == ' ') e--;
        printf("|%.*s\n", e, shadow[r]);
    }
    fputs("=== end ===\n", stdout);
}

void hal_draw_row(int row, const cell_t *cells, int cols, int cursor_col, int style)
{
    int c, last = -1;
    char buf[CON_MAXCOLS * 24 + 64];
    int n = 0;
    (void)style;
    if (batch) {
        for (c = 0; c < cols; c++) {
            int ch = cells[c] & 0xFF;
            shadow[row][c] = (char)(ch >= 32 && ch < 127 ? ch : '#');
            shadow_cells[row][c] = cells[c];
        }
        shadow_cols = cols;
        shadow[row][cols] = 0;
        shadow_dirty = 1;
        return;
    }
    n += sprintf(buf + n, "\033[%d;1H", row + 1);
    for (c = 0; c < cols; c++) {
        int a = cells[c] >> 8, ch = cells[c] & 0xFF;
        if (c == cursor_col) a = ((a & 15) << 4) | ((a >> 4) & 15);
        if (a != last) { n += sprintf(buf + n, "\033[%d;%dm", ansi_fg[a & 15], ansi_bg[(a >> 4) & 15]); last = a; }
        if (ch < 32 || ch > 126) ch = ch >= 128 ? '#' : ' ';
        buf[n++] = (char)ch;
    }
    n += sprintf(buf + n, "\033[0m\033[K");
    fwrite(buf, 1, (size_t)n, stdout);
    fflush(stdout);
}

void hal_set_font(int f) { font = f; }
/* a framebuffer for the pixel calls; CALCIDE_FB=FILE dumps it at exit
 * (raw RGB565, 320x320) so tests can check what was drawn */
static uint16_t fb[LCD_H][LCD_W];

static void fb_dump(void)
{
    const char *p = getenv("CALCIDE_FB");
    FILE *f;
    if (!p || !(f = fopen(p, "wb"))) return;
    fwrite(fb, sizeof fb, 1, f);
    fclose(f);
}

void hal_pixel(int x, int y, uint16_t c) { if (x >= 0 && y >= 0 && x < LCD_W && y < LCD_H) fb[y][x] = c; }
void hal_fill(int x, int y, int w, int h, uint16_t c)
{
    int i, j;
    for (j = y; j < y + h; j++) for (i = x; i < x + w; i++) hal_pixel(i, j, c);
}
void hal_line(int x0, int y0, int x1, int y1, uint16_t c) { (void)x0; (void)y0; (void)x1; (void)y1; (void)c; }

void hal_serial_write(const char *s, int n)
{
    int i;
    if (!batch) return;
    for (i = 0; i < n; i++) if (s[i] != '\r') putchar(s[i]);
    fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* everything else                                                     */
/* ------------------------------------------------------------------ */

void hal_tone(int hz, int ms) { (void)hz; if (ms > 0) hal_sleep_ms(ms); }
void hal_tone_off(void) { }
void hal_backlight(int level) { (void)level; }
void hal_kbdlight(int level) { (void)level; }
int hal_battery(int *charging) { if (charging) *charging = 0; return 100; }
int hal_cpu_khz(void) { return 150000; }
int hal_set_cpu_mhz(int mhz) { (void)mhz; return 1; }
float hal_temp_c(void) { return 27.0f; }
size_t hal_ram_total(void) { return 520 * 1024; }
size_t hal_ram_free(void) { return 400 * 1024; }
size_t hal_ram_largest(void) { const char *e = getenv("CALCIDE_RAM_KB"); return (e ? (size_t)atoi(e) : 380) * 1024; }
const char *hal_chip(void) { return "host"; }

void hal_reboot(void) { restore(); exit(0); }
void hal_poweroff(void) { restore(); exit(0); }
void hal_reboot_to_bootloader(void) { restore(); exit(0); }

int hal_pf_pages(void) { return PF_PAGES; }
int hal_pf_page_size(void) { return PF_PSIZE; }
const void *hal_pf_read(int page)
{
    if (page < 0 || page >= PF_PAGES) return 0;
    return pf_mem + (size_t)page * PF_PSIZE;
}

static void pf_persist(void)
{
    FILE *f = fopen(pf_path, "wb");
    if (f) { fwrite(pf_mem, 1, PF_PAGES * PF_PSIZE, f); fclose(f); }
}

int hal_pf_write(int page, const void *data, int len)
{
    if (page < 0 || page >= PF_PAGES || len < 0 || len > PF_PSIZE) return 0;
    memset(pf_mem + (size_t)page * PF_PSIZE, 0xFF, PF_PSIZE);
    if (len) memcpy(pf_mem + (size_t)page * PF_PSIZE, data, (size_t)len);
    pf_persist();
    return 1;
}

int hal_pf_erase(int page)
{
    if (page < 0 || page >= PF_PAGES) return 0;
    memset(pf_mem + (size_t)page * PF_PSIZE, 0xFF, PF_PSIZE);
    pf_persist();
    return 1;
}

int hal_sd_present(void) { return 1; }
int hal_sd_hc(void) { return 1; }

/* ------------------------------------------------------------------ */
/* host stand-ins: PCM is counted, GPIO is a register file, and each   */
/* UART loops its output back to its input (for tests)                 */
/* ------------------------------------------------------------------ */

/* CALCIDE_PCM=FILE appends every sample (8-bit unsigned) for tests */
static unsigned long pcm_samples;
static FILE *pcm_out;
void hal_pcm_start(int rate)
{
    const char *p = getenv("CALCIDE_PCM");
    (void)rate;
    pcm_samples = 0;
    if (p && !pcm_out) pcm_out = fopen(p, "ab");
}
void hal_pcm_write(uint8_t v) { pcm_samples++; if (pcm_out) fputc(v, pcm_out); }
void hal_pcm_stop(void) { if (pcm_out) { fclose(pcm_out); pcm_out = 0; } }
int hal_pcm_realtime(void) { return !batch; }

static int gpio_val[30], gpio_dir[30];

int hal_gpio_free(int pin)
{
    if (pin < 0 || pin > 29) return 0;
    if (pin == 6 || pin == 7 || (pin >= 10 && pin <= 19) || pin == 26 || pin == 27) return 0;
    if ((pin >= 23 && pin <= 25) || pin == 29) return 0;
    return 1;
}

int hal_gpio_mode(int pin, int mode)
{
    if (!hal_gpio_free(pin)) return 0;
    gpio_dir[pin] = mode;
    if (mode == HAL_GPIO_PULLUP) gpio_val[pin] = 1;
    if (mode == HAL_GPIO_PULLDOWN) gpio_val[pin] = 0;
    return 1;
}

int hal_gpio_write(int pin, int v) { if (!hal_gpio_free(pin)) return 0; gpio_val[pin] = v != 0; return 1; }
int hal_gpio_read(int pin) { if (!hal_gpio_free(pin)) return -1; return gpio_val[pin]; }

static uint8_t uq[2][256];
static int uh[2], ut[2], uopen[2];

int hal_uart_open(int port, int baud)
{
    if (port < 0 || port > 1 || baud < 300 || baud > 3000000) return 0;
    uopen[port] = 1;
    uh[port] = ut[port] = 0;
    return 1;
}
void hal_uart_close(int port) { if (port >= 0 && port < 2) uopen[port] = 0; }
int hal_uart_putc(int port, int c)
{
    if (port < 0 || port > 1 || !uopen[port]) return -1;
    if ((ut[port] + 1) % 256 != uh[port]) { uq[port][ut[port]] = (uint8_t)c; ut[port] = (ut[port] + 1) % 256; }
    return c & 255;
}
int hal_uart_getc(int port)
{
    int c;
    if (port < 0 || port > 1 || !uopen[port] || uh[port] == ut[port]) return -1;
    c = uq[port][uh[port]];
    uh[port] = (uh[port] + 1) % 256;
    return c;
}
int hal_uart_available(int port) { return port >= 0 && port < 2 && uopen[port] && uh[port] != ut[port]; }
