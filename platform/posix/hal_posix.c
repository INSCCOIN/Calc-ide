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

int hal_key(void)
{
    int c, d, e;
    if (eof_seen) return KEY_CTRL('c');
    c = rd_byte(0);
    if (c == -2) {
        /* batch input finished: behave like a user pressing Ctrl+C forever */
        eof_seen = 1;
        return KEY_CTRL('c');
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

int hal_poll_break(void (*queue)(int key))
{
    int k;
    if (batch) return eof_seen ? 0 : 0;   /* scripted input is only read on demand */
    while ((k = hal_key()) >= 0) {
        if (k == KEY_BREAK || k == KEY_CTRL('c')) return 1;
        queue(k);
    }
    return 0;
}

int hal_eof(void) { return eof_seen; }

/* ------------------------------------------------------------------ */
/* display                                                             */
/* ------------------------------------------------------------------ */

static const int ansi_fg[16] = {30, 31, 32, 33, 34, 35, 36, 37, 90, 91, 92, 93, 94, 95, 96, 97};
static const int ansi_bg[16] = {40, 41, 42, 43, 44, 45, 46, 47, 100, 101, 102, 103, 104, 105, 106, 107};

void hal_draw_row(int row, const cell_t *cells, int cols, int cursor_col, int style)
{
    int c, last = -1;
    char buf[CON_MAXCOLS * 24 + 64];
    int n = 0;
    (void)style;
    if (batch) return;
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
void hal_pixel(int x, int y, uint16_t c) { (void)x; (void)y; (void)c; }
void hal_fill(int x, int y, int w, int h, uint16_t c) { (void)x; (void)y; (void)w; (void)h; (void)c; }
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
int hal_set_cpu_mhz(int mhz) { (void)mhz; return 0; }
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
