/* lcd.c - ST7365P / ILI9488 driver for the PicoCalc's 320x320 panel.
 *
 * Adapted from CalcOS (platform/picocalc/lcd.c). The two rules that make
 * this panel work at all:
 *
 *  1. Chip select stays LOW across a whole transaction (a command and its
 *     parameters; the window registers and the pixels that follow).
 *  2. Pixels go out with blocking SPI, not DMA.
 *
 * The register sequence matches the ClockworkPi reference drivers.
 */
#include "picocalc.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include <string.h>

#define LCD_SPI   spi1
#define LCD_SCK   10
#define LCD_TX    11
#define LCD_RX    12
#define LCD_CS    13
#define LCD_DC    14
#define LCD_RST   15
#define LCD_W     320
#define LCD_H     320

static int bits18;
static int spi_mhz;

static uint32_t lcd_hz(void)
{
    /* a quarter of the peripheral clock, as in CalcOS */
    uint32_t hz = spi_mhz > 0 ? (uint32_t)spi_mhz * 1000000u : clock_get_hz(clk_peri) / 4u;
    if (hz > 62500000u) hz = 62500000u;
    return hz;
}

static inline void cs_lo(void) { gpio_put(LCD_CS, 0); }
static inline void cs_hi(void) { gpio_put(LCD_CS, 1); }

static void wr_cmd(uint8_t c)
{
    gpio_put(LCD_DC, 0);
    spi_write_blocking(LCD_SPI, &c, 1);
}

static void wr_dat(const uint8_t *d, size_t n)
{
    if (!n) return;
    gpio_put(LCD_DC, 1);
    spi_write_blocking(LCD_SPI, d, n);
}

static void reg(uint8_t c, const uint8_t *d, size_t n)
{
    wr_cmd(c);
    wr_dat(d, n);
}

#define REG(c, ...) do { static const uint8_t _d[] = { __VA_ARGS__ }; reg((c), _d, sizeof _d); } while (0)

void lcd_window(int x, int y, int w, int h)
{
    uint8_t b[4];
    int x1 = x + w - 1, y1 = y + h - 1;
    cs_lo();
    b[0] = (uint8_t)(x >> 8);  b[1] = (uint8_t)x;
    b[2] = (uint8_t)(x1 >> 8); b[3] = (uint8_t)x1;
    reg(0x2A, b, 4);
    b[0] = (uint8_t)(y >> 8);  b[1] = (uint8_t)y;
    b[2] = (uint8_t)(y1 >> 8); b[3] = (uint8_t)y1;
    reg(0x2B, b, 4);
    wr_cmd(0x2C);
}

static void blit_raw(const uint16_t *px, int count)
{
    if (count <= 0) return;
    gpio_put(LCD_DC, 1);
    if (bits18) {
        static uint8_t tmp[960];
        int i = 0;
        while (i < count) {
            int n = 0;
            while (i < count && n + 3 <= (int)sizeof tmp) {
                uint16_t p = px[i++];
                tmp[n++] = (uint8_t)((p >> 8) & 0xF8);
                tmp[n++] = (uint8_t)((p >> 3) & 0xFC);
                tmp[n++] = (uint8_t)((p << 3) & 0xF8);
            }
            spi_write_blocking(LCD_SPI, tmp, (size_t)n);
        }
        return;
    }
    spi_set_format(LCD_SPI, 16, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_write16_blocking(LCD_SPI, px, (size_t)count);
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
}

void lcd_blit(const uint16_t *px, int count)
{
    blit_raw(px, count);
    cs_hi();
}

void lcd_fill(int x, int y, int w, int h, uint16_t color)
{
    static uint16_t buf[LCD_W];
    int i, row;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > LCD_W) w = LCD_W - x;
    if (y + h > LCD_H) h = LCD_H - y;
    if (w <= 0 || h <= 0) return;
    for (i = 0; i < w; i++) buf[i] = color;
    lcd_window(x, y, w, h);
    for (row = 0; row < h; row++) blit_raw(buf, w);
    cs_hi();
}

void lcd_reclock(void) { spi_set_baudrate(LCD_SPI, lcd_hz()); }

void lcd_init(const LcdCfg *cfg)
{
    uint8_t pixfmt, madctl;
    bits18 = cfg->bits18;
    spi_mhz = cfg->spi_mhz;
    pixfmt = bits18 ? 0x66 : 0x55;
    madctl = (uint8_t)cfg->madctl;

    spi_init(LCD_SPI, lcd_hz());
    spi_set_format(LCD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(LCD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(LCD_TX, GPIO_FUNC_SPI);
    gpio_set_function(LCD_RX, GPIO_FUNC_SPI);
    gpio_init(LCD_CS);  gpio_set_dir(LCD_CS, GPIO_OUT);
    gpio_init(LCD_DC);  gpio_set_dir(LCD_DC, GPIO_OUT);
    gpio_init(LCD_RST); gpio_set_dir(LCD_RST, GPIO_OUT);
    cs_hi();
    gpio_put(LCD_DC, 0);

    gpio_put(LCD_RST, 1); sleep_ms(10);
    gpio_put(LCD_RST, 0); sleep_ms(10);
    gpio_put(LCD_RST, 1); sleep_ms(150);

    cs_lo();
    REG(0xE0, 0x00, 0x03, 0x09, 0x08, 0x16, 0x0A, 0x3F, 0x78, 0x4C, 0x09, 0x0A, 0x08, 0x16, 0x1A, 0x0F);
    REG(0xE1, 0x00, 0x16, 0x19, 0x03, 0x0F, 0x05, 0x32, 0x45, 0x46, 0x04, 0x0E, 0x0D, 0x35, 0x37, 0x0F);
    REG(0xC0, 0x17, 0x15);             /* power control 1   */
    REG(0xC1, 0x41);                   /* power control 2   */
    REG(0xC5, 0x00, 0x12, 0x80);       /* VCOM              */
    reg(0x36, &madctl, 1);             /* memory access     */
    reg(0x3A, &pixfmt, 1);             /* pixel format      */
    REG(0xB0, 0x00);                   /* interface mode    */
    REG(0xB1, 0xA0);                   /* frame rate        */
    wr_cmd(cfg->invert ? 0x21 : 0x20); /* inversion         */
    REG(0xB4, 0x02);
    REG(0xB6, 0x02, 0x02, 0x3B);       /* display function  */
    REG(0xB7, 0xC6);
    REG(0xE9, 0x00);
    REG(0xF7, 0xA9, 0x51, 0x2C, 0x82);
    wr_cmd(0x11);                      /* sleep out         */
    cs_hi();
    sleep_ms(120);
    cs_lo();
    wr_cmd(0x29);                      /* display on        */
    cs_hi();
    sleep_ms(20);
}

void lcd_sleep(void)
{
    cs_lo();
    wr_cmd(0x28);
    wr_cmd(0x10);
    cs_hi();
    sleep_ms(20);
}
