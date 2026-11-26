/* picocalc.h - board-level declarations for the ClockworkPi PicoCalc.
 *
 * Pins (PicoCalc mainboard v2.0):
 *   LCD  ST7365P/ILI9488 on SPI1: SCK 10, MOSI 11, MISO 12, CS 13, DC 14, RST 15
 *   SD   on SPI0: MISO 16, CS 17, SCK 18, MOSI 19
 *   Keyboard / battery / backlights: STM32 at I2C1 0x1F, SDA 6, SCL 7
 *   Speaker: PWM on GP26 (left) and GP27 (right)
 */
#ifndef PICOCALC_H
#define PICOCALC_H

#include <stdint.h>
#include <stddef.h>

/* display (lcd.c) */
typedef struct { int bits18; int madctl; int invert; int spi_mhz; } LcdCfg;
void lcd_init(const LcdCfg *cfg);
void lcd_window(int x, int y, int w, int h);
void lcd_blit(const uint16_t *px, int count);
void lcd_fill(int x, int y, int w, int h, uint16_t color);
void lcd_sleep(void);
void lcd_reclock(void);

/* keyboard controller (keyboard.c) */
void kbd_init(void);
void kbd_reclock(void);
int  kbd_poll(void);              /* key code, or 0 */
int  kbd_modifiers(void);
void kbd_set_backlight(int level, int swap);
void kbd_set_kbd_backlight(int level, int swap);
int  kbd_battery(int *charging);  /* percent or -1 */
int  kbd_poweroff(void);

enum { MOD_SHIFT_L = 1, MOD_SHIFT_R = 2, MOD_ALT = 4, MOD_CTRL = 8 };

/* SD card over SPI (sd_spi.c) */
int  sd_init(void);
int  sd_ready(void);
int  sd_is_hc(void);
int  sd_read_blocks(uint32_t lba, uint8_t *buf, int n);
int  sd_write_blocks(uint32_t lba, const uint8_t *buf, int n);
int  sd_sync(void);
uint32_t sd_sectors(void);
void sd_reclock(void);
void sd_deinit(void);

#endif
