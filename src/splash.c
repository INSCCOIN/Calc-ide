/* splash.c - the boot screen.
 *
 * With splash = 1 the machine shows a boot screen for splash_ms (3 s by
 * default) before the command line; any key skips it. If splash_image
 * names a .bmp on the SD card, that picture is shown instead of the CalcIDE
 * logo. BMP files may be 1, 4, 8, 16, 24 or 32 bits per pixel, uncompressed
 * (BI_RGB) or BI_BITFIELDS, bottom-up or top-down; they are centred on the
 * 320x320 panel and cropped if larger.
 */
#include "splash.h"
#include "console.h"
#include "config.h"
#include "hal.h"
#include "vfs.h"
#include "pf.h"
#include "shell.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* BMP                                                                 */
/* ------------------------------------------------------------------ */

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static int mask_shift(uint32_t m) { int s = 0; if (!m) return 0; while (!(m & 1)) { m >>= 1; s++; } return s; }
static int mask_bits(uint32_t m) { int n = 0; while (m) { n += m & 1; m >>= 1; } return n; }

static uint8_t chan(uint32_t v, uint32_t m)
{
    int s = mask_shift(m), n = mask_bits(m);
    uint32_t c;
    if (!m) return 0;
    c = (v & m) >> s;
    if (n >= 8) return (uint8_t)(c >> (n - 8));
    return (uint8_t)((c * 255u) / ((1u << n) - 1u));
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

int bmp_show(const char *path)
{
    char full[VFS_PATH];
    uint8_t h[138];
    uint8_t *row = 0;
    uint16_t line[LCD_W];
    uint16_t pal[256];
    vfile *f;
    uint32_t off, hsize, comp, rmask = 0, gmask = 0, bmask = 0, ncol, stride;
    int32_t w, ht;
    int bpp, topdown = 0, x0, y0, sx0, sy0, vw, vh, y, x, i, got, ok = 0;
    if (!vfs_resolve(path, full)) return 0;
    f = vfs_open(full, "r");
    if (!f) return 0;
    got = vfs_read(f, h, sizeof h);
    if (got < 54 || h[0] != 'B' || h[1] != 'M') goto out;
    off = le32(h + 10);
    hsize = le32(h + 14);
    if (hsize < 40) goto out;              /* OS/2 bitmaps are not supported */
    w = (int32_t)le32(h + 18);
    ht = (int32_t)le32(h + 22);
    bpp = le16(h + 28);
    comp = le32(h + 30);
    ncol = le32(h + 46);
    if (ht < 0) { topdown = 1; ht = -ht; }
    if (w <= 0 || ht <= 0 || w > 8192 || ht > 8192) goto out;
    if (comp != 0 && comp != 3) goto out;  /* no RLE or JPEG/PNG inside */
    if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) goto out;
    if (bpp == 16) { rmask = 0x7C00; gmask = 0x03E0; bmask = 0x001F; }
    if (bpp == 32) { rmask = 0x00FF0000; gmask = 0x0000FF00; bmask = 0x000000FF; }
    if (comp == 3 && got >= 66) { rmask = le32(h + 54); gmask = le32(h + 58); bmask = le32(h + 62); }
    if (bpp <= 8) {
        uint8_t q[4];
        if (!ncol || ncol > 256) ncol = 1u << bpp;
        if (vfs_seek(f, (int32_t)(14 + hsize), 0) != 0) goto out;
        for (i = 0; i < (int)ncol; i++) {
            if (vfs_read(f, q, 4) != 4) goto out;
            pal[i] = rgb565(q[2], q[1], q[0]);
        }
        for (; i < 256; i++) pal[i] = 0;
    }
    stride = (((uint32_t)w * (uint32_t)bpp + 31) / 32) * 4;
    row = (uint8_t *)malloc(stride);
    if (!row) goto out;
    /* centre, crop what does not fit */
    vw = w > LCD_W ? LCD_W : w;
    vh = ht > LCD_H ? LCD_H : ht;
    sx0 = (w - vw) / 2;
    sy0 = (ht - vh) / 2;
    x0 = (LCD_W - vw) / 2;
    y0 = (LCD_H - vh) / 2;
    hal_fill(0, 0, LCD_W, LCD_H, 0);
    for (y = 0; y < vh; y++) {
        int src = sy0 + y;
        int fr = topdown ? src : ht - 1 - src;
        if (vfs_seek(f, (int32_t)(off + (uint32_t)fr * stride), 0) != 0) goto out;
        if (vfs_read(f, row, (int)stride) != (int)stride) goto out;
        for (x = 0; x < vw; x++) {
            int sx = sx0 + x;
            uint32_t v;
            switch (bpp) {
            case 1: line[x] = pal[(row[sx >> 3] >> (7 - (sx & 7))) & 1]; break;
            case 4: line[x] = pal[(row[sx >> 1] >> ((sx & 1) ? 0 : 4)) & 15]; break;
            case 8: line[x] = pal[row[sx]]; break;
            case 16: v = le16(row + sx * 2); line[x] = rgb565(chan(v, rmask), chan(v, gmask), chan(v, bmask)); break;
            case 24: line[x] = rgb565(row[sx * 3 + 2], row[sx * 3 + 1], row[sx * 3]); break;
            default: v = le32(row + sx * 4); line[x] = rgb565(chan(v, rmask), chan(v, gmask), chan(v, bmask)); break;
            }
        }
        hal_blit(x0, y0 + y, vw, 1, line);
    }
    ok = 1;
out:
    free(row);
    vfs_close(f);
    return ok;
}

/* ------------------------------------------------------------------ */
/* the logo screen                                                     */
/* ------------------------------------------------------------------ */

/* "CalcIDE" in a 3x5 block font */
static const char *const big[7][5] = {
    {"###", "#..", "#..", "#..", "###"},   /* C */
    {"...", ".##", "#.#", "#.#", ".##"},   /* a */
    {"#..", "#..", "#..", "#..", "##."},   /* l */
    {"...", ".##", "#..", "#..", ".##"},   /* c */
    {"###", ".#.", ".#.", ".#.", "###"},   /* I */
    {"##.", "#.#", "#.#", "#.#", "##."},   /* D */
    {"###", "#..", "##.", "#..", "###"},   /* E */
};

static void center(int y, const char *s, int a)
{
    int cols = con_cols(), n = (int)strlen(s);
    con_text_at((cols - n) / 2 < 0 ? 0 : (cols - n) / 2, y, s, a, cols);
}

static void draw_logo(void)
{
    int cols = con_cols(), rows = con_rows(), x, y, l, r;
    int bg = 4, a = ATTR(15, bg), dim = ATTR(7, bg), frame = ATTR(14, bg);   /* ANSI order: 4 = blue */
    int lx = (cols - 27) / 2, ly = 6;
    char s[64];
    con_fill_rect(0, 0, cols, rows, ' ', a);
    /* double-line frame */
    con_put_at(0, 0, 0xC9, frame);
    con_put_at(cols - 1, 0, 0xBB, frame);
    con_put_at(0, rows - 1, 0xC8, frame);
    con_put_at(cols - 1, rows - 1, 0xBC, frame);
    for (x = 1; x < cols - 1; x++) { con_put_at(x, 0, 0xCD, frame); con_put_at(x, rows - 1, 0xCD, frame); }
    for (y = 1; y < rows - 1; y++) { con_put_at(0, y, 0xBA, frame); con_put_at(cols - 1, y, 0xBA, frame); }
    /* the name, with a shadow */
    for (l = 0; l < 7; l++)
        for (r = 0; r < 5; r++)
            for (x = 0; x < 3; x++)
                if (big[l][r][x] == '#') {
                    con_put_at(lx + l * 4 + x + 1, ly + r + 1, 0xB1, ATTR(0, bg));
                    con_put_at(lx + l * 4 + x, ly + r, 0xDB, ATTR(l < 4 ? 11 : 15, bg));
                }
    center(ly + 7, "Tiny-C IDE for the PicoCalc", a);
    snprintf(s, sizeof s, "version %s", CALCIDE_VERSION);
    center(ly + 9, s, dim);
    snprintf(s, sizeof s, "%s  %d MHz  %u KB free", hal_chip(), hal_cpu_khz() / 1000, (unsigned)(hal_ram_free() / 1024));
    center(ly + 12, s, dim);
    if (vfs_mounted()) {
        uint32_t t, f;
        if (vfs_space(&t, &f)) snprintf(s, sizeof s, "SD card: %u MB free", (unsigned)(f / 1024));
        else snprintf(s, sizeof s, "SD card ready");
    } else snprintf(s, sizeof s, "no SD card");
    center(ly + 13, s, dim);
    if (pf_available()) {
        snprintf(s, sizeof s, "page file: %d KB free", pf_free_pages() * 4);
        center(ly + 14, s, dim);
    }
    center(rows - 3, "any key to skip", ATTR(12, bg));
}

/* a progress bar on the logo screen, 0..1000 */
static void draw_bar(int permille, int secs_left)
{
    int cols = con_cols(), rows = con_rows(), w = cols - 12, x, filled = w * permille / 1000;
    int y = rows - 6, x0 = 6;
    char s[32];
    for (x = 0; x < w; x++)
        con_put_at(x0 + x, y, x < filled ? 0xDB : 0xB0, ATTR(x < filled ? 10 : 12, 4));
    snprintf(s, sizeof s, "  starting in %d s  ", secs_left);
    center(y - 2, s, ATTR(15, 4));
}

void splash_show(void)
{
    uint32_t t0, ms = (uint32_t)g_conf.splash_ms, el;
    int logo = 1, k;
    if (!g_conf.splash || g_conf.splash_ms <= 0) return;
    t0 = hal_millis();
    con_cursor(0);
    if (g_conf.splash_image[0] && bmp_show(g_conf.splash_image)) logo = 0;
    if (logo) draw_logo();
    for (;;) {
        el = hal_millis() - t0;
        if (el >= ms) break;
        if (logo) {
            draw_bar((int)(el * 1000u / ms), (int)((ms - el + 999u) / 1000u));
            con_flush();
        }
        k = hal_key();
        if (k >= 0) break;
        hal_sleep_ms(30);
    }
    if (logo) { draw_bar(1000, 0); con_flush(); }
    con_cursor(g_conf.cursor);
    con_default_color();
    con_invalidate();
    con_clear();
    con_flush();
}
