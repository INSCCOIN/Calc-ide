/* sd_spi.c - SD card in SPI mode, the block device under FatFs.
 *
 * The initialisation and single-block transfers are from CalcOS
 * (platform/picocalc/sdfat.c). Added here: the CSD read for the card's
 * capacity, multi-block loops and a sync used by FatFs.
 */
#include "picocalc.h"
#include "pico/stdlib.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include <string.h>

#define SD_SPI   spi0
#define SD_MISO  16
#define SD_CS    17
#define SD_SCK   18
#define SD_MOSI  19

#define SD_SLOW_HZ  (400 * 1000)
#define SD_FAST_HZ  (12 * 1000 * 1000)

static int sd_ok;
static int sd_hc;
static uint32_t sectors;

static inline void cs_lo(void) { gpio_put(SD_CS, 0); }
static inline void cs_hi(void) { gpio_put(SD_CS, 1); }

static uint8_t xchg(uint8_t v)
{
    uint8_t r = 0xFF;
    spi_write_read_blocking(SD_SPI, &v, &r, 1);
    return r;
}

static void clock_bytes(int n) { while (n-- > 0) xchg(0xFF); }

static uint8_t wait_ready(int tries)
{
    uint8_t r;
    do { r = xchg(0xFF); } while (r != 0xFF && --tries > 0);
    return r;
}

static uint8_t sd_cmd(uint8_t cmd, uint32_t arg)
{
    uint8_t buf[6], r;
    int i;
    buf[0] = (uint8_t)(0x40 | cmd);
    buf[1] = (uint8_t)(arg >> 24);
    buf[2] = (uint8_t)(arg >> 16);
    buf[3] = (uint8_t)(arg >> 8);
    buf[4] = (uint8_t)arg;
    buf[5] = (cmd == 0) ? 0x95 : (cmd == 8 ? 0x87 : 0x01);
    for (i = 0; i < 6; i++) xchg(buf[i]);
    for (i = 0; i < 10; i++) {
        r = xchg(0xFF);
        if (!(r & 0x80)) return r;
    }
    return 0xFF;
}

static int read_data(uint8_t *buf, int n)
{
    uint8_t tok;
    int tries = 100000;
    do { tok = xchg(0xFF); } while (tok == 0xFF && --tries > 0);
    if (tok != 0xFE) return 0;
    memset(buf, 0xFF, (size_t)n);
    spi_read_blocking(SD_SPI, 0xFF, buf, (size_t)n);
    xchg(0xFF);
    xchg(0xFF);
    return 1;
}

static void read_csd(void)
{
    uint8_t csd[16];
    sectors = 0;
    cs_lo();
    if (sd_cmd(9, 0) == 0 && read_data(csd, 16)) {
        if ((csd[0] >> 6) == 1) {                     /* CSD v2: SDHC/SDXC */
            uint32_t c_size = ((uint32_t)(csd[7] & 0x3F) << 16) | ((uint32_t)csd[8] << 8) | csd[9];
            sectors = (c_size + 1) * 1024u;
        } else {                                      /* CSD v1 */
            uint32_t c_size = ((uint32_t)(csd[6] & 3) << 10) | ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
            uint32_t mult = ((csd[9] & 3) << 1) | (csd[10] >> 7);
            uint32_t bl = csd[5] & 15;
            sectors = (c_size + 1) << (mult + 2 + bl - 9);
        }
    }
    cs_hi();
    clock_bytes(1);
}

int sd_init(void)
{
    int i;
    uint8_t r;
    if (sd_ok) return 1;
    gpio_init(SD_CS);
    gpio_set_dir(SD_CS, GPIO_OUT);
    cs_hi();
    spi_init(SD_SPI, SD_SLOW_HZ);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SD_SCK, GPIO_FUNC_SPI);
    gpio_set_function(SD_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(SD_MISO, GPIO_FUNC_SPI);
    gpio_pull_up(SD_MISO);
    sd_hc = 0;
    clock_bytes(10);
    cs_lo();
    for (i = 0; i < 16; i++) {
        if (sd_cmd(0, 0) == 1) break;
        sleep_ms(2);
    }
    if (i == 16) { cs_hi(); return 0; }
    r = sd_cmd(8, 0x1AA);
    if (r == 1) {
        uint8_t v[4];
        for (i = 0; i < 4; i++) v[i] = xchg(0xFF);
        if (v[2] != 0x01 || v[3] != 0xAA) { cs_hi(); return 0; }
        for (i = 0; i < 2000; i++) {
            sd_cmd(55, 0);
            if (sd_cmd(41, 0x40000000) == 0) break;
            sleep_ms(1);
        }
        if (i == 2000) { cs_hi(); return 0; }
        if (sd_cmd(58, 0) == 0) {
            uint8_t o[4];
            for (i = 0; i < 4; i++) o[i] = xchg(0xFF);
            sd_hc = (o[0] & 0x40) ? 1 : 0;
        }
    } else {
        for (i = 0; i < 2000; i++) {
            sd_cmd(55, 0);
            if (sd_cmd(41, 0) == 0) break;
            sleep_ms(1);
        }
        if (i == 2000) { cs_hi(); return 0; }
        sd_cmd(16, 512);
    }
    cs_hi();
    clock_bytes(1);
    spi_set_baudrate(SD_SPI, SD_FAST_HZ);
    sd_ok = 1;
    read_csd();
    return 1;
}

void sd_deinit(void) { sd_ok = 0; }
int sd_ready(void) { return sd_ok; }
int sd_is_hc(void) { return sd_hc; }
uint32_t sd_sectors(void) { return sectors; }
void sd_reclock(void) { if (sd_ok) spi_set_baudrate(SD_SPI, SD_FAST_HZ); }

static int write_block(uint32_t lba, const uint8_t *buf)
{
    uint8_t r;
    int tries = 200000;
    cs_lo();
    wait_ready(50000);
    if (sd_cmd(24, sd_hc ? lba : lba * 512u) != 0) { cs_hi(); return 0; }
    xchg(0xFF);
    xchg(0xFE);
    spi_write_blocking(SD_SPI, buf, 512);
    xchg(0xFF);
    xchg(0xFF);
    r = xchg(0xFF);
    if ((r & 0x1F) != 0x05) { cs_hi(); return 0; }
    do { r = xchg(0xFF); } while (r == 0x00 && --tries > 0);
    cs_hi();
    clock_bytes(1);
    return tries > 0;
}

static int read_block(uint32_t lba, uint8_t *buf)
{
    int ok;
    cs_lo();
    wait_ready(50000);
    if (sd_cmd(17, sd_hc ? lba : lba * 512u) != 0) { cs_hi(); return 0; }
    ok = read_data(buf, 512);
    cs_hi();
    clock_bytes(1);
    return ok;
}

int sd_read_blocks(uint32_t lba, uint8_t *buf, int n)
{
    int i, t;
    if (!sd_ok) return 0;
    for (i = 0; i < n; i++) {
        for (t = 0; t < 3; t++) if (read_block(lba + (uint32_t)i, buf + i * 512)) break;
        if (t == 3) return 0;
    }
    return 1;
}

int sd_write_blocks(uint32_t lba, const uint8_t *buf, int n)
{
    int i, t;
    if (!sd_ok) return 0;
    for (i = 0; i < n; i++) {
        for (t = 0; t < 3; t++) if (write_block(lba + (uint32_t)i, buf + i * 512)) break;
        if (t == 3) return 0;
    }
    return 1;
}

int sd_sync(void)
{
    if (!sd_ok) return 0;
    cs_lo();
    wait_ready(200000);
    cs_hi();
    clock_bytes(1);
    return 1;
}
