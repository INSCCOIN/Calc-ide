/* keyboard.c - the PicoCalc keyboard controller: an STM32 at I2C1 0x1F.
 *
 * Adapted from CalcOS (platform/picocalc/keyboard.c). Register numbers are
 * from the ClockworkPi keyboard firmware (picocalc_keyboard/reg.h):
 *   0x05 LCD backlight   0x09 key FIFO   0x0A keyboard backlight
 *   0x0B battery (bit 7 = charging)      0x0E power off
 * Reading a register is: write its number, then read two bytes. Writing is
 * (reg | 0x80, value). A STOP is sent between the two halves; the
 * controller does not answer a repeated start reliably.
 */
#include "picocalc.h"
#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#define KBD_I2C   i2c1
#define KBD_SDA   6
#define KBD_SCL   7
#define KBD_ADDR  0x1F
#define KBD_HZ    20000

enum { REG_BKL = 0x05, REG_FIF = 0x09, REG_BK2 = 0x0A, REG_BAT = 0x0B, REG_OFF = 0x0E };
enum { ST_IDLE = 0, ST_PRESSED = 1, ST_HOLD = 2, ST_RELEASED = 3 };

#define KEY_ALT_CODE    0xA1
#define KEY_LSHIFT_CODE 0xA2
#define KEY_RSHIFT_CODE 0xA3
#define KEY_CTRL_CODE   0xA5

static int modifiers;
static int ok;

static int cmd_read(uint8_t regno, uint8_t *out, int n)
{
    if (i2c_write_timeout_us(KBD_I2C, KBD_ADDR, &regno, 1, false, 4000) != 1) return 0;
    if (i2c_read_timeout_us(KBD_I2C, KBD_ADDR, out, (size_t)n, false, 4000) != n) return 0;
    return 1;
}

static int write_reg(uint8_t regno, uint8_t value)
{
    uint8_t b[2];
    b[0] = (uint8_t)(regno | 0x80);
    b[1] = value;
    return i2c_write_timeout_us(KBD_I2C, KBD_ADDR, b, 2, false, 4000) == 2;
}

static int read_fifo(void)
{
    uint8_t b[2];
    if (!cmd_read(REG_FIF, b, 2)) return 0;
    return ((int)b[1] << 8) | b[0];
}

void kbd_init(void)
{
    int guard = 0;
    i2c_init(KBD_I2C, KBD_HZ);
    gpio_set_function(KBD_SDA, GPIO_FUNC_I2C);
    gpio_set_function(KBD_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_SDA);
    gpio_pull_up(KBD_SCL);
    modifiers = 0;
    ok = 1;
    while (read_fifo() != 0 && ++guard < 64) { }
}

void kbd_reclock(void) { i2c_set_baudrate(KBD_I2C, KBD_HZ); }
int kbd_modifiers(void) { return modifiers; }

void kbd_set_backlight(int level, int swap) { write_reg(swap ? REG_BK2 : REG_BKL, (uint8_t)level); }
void kbd_set_kbd_backlight(int level, int swap) { write_reg(swap ? REG_BKL : REG_BK2, (uint8_t)level); }

/* The power rail is switched by the keyboard MCU. Its firmware waits at
 * least six seconds after the request before cutting power. */
int kbd_poweroff(void) { return write_reg(REG_OFF, 6); }

int kbd_battery(int *charging)
{
    uint8_t b[2];
    if (!cmd_read(REG_BAT, b, 2)) return -1;
    if (charging) *charging = (b[1] & 0x80) != 0;
    return b[1] & 0x7F;
}

static void update_modifiers(int code, int state)
{
    int bit;
    switch (code) {
    case KEY_CTRL_CODE: bit = MOD_CTRL; break;
    case KEY_ALT_CODE: bit = MOD_ALT; break;
    case KEY_LSHIFT_CODE: bit = MOD_SHIFT_L; break;
    case KEY_RSHIFT_CODE: bit = MOD_SHIFT_R; break;
    default: return;
    }
    if (state == ST_PRESSED) modifiers |= bit;
    else if (state == ST_RELEASED) modifiers &= ~bit;
}

/* a key code on press or auto-repeat, 0 when nothing is pending;
 * Ctrl+letter becomes a control character */
int kbd_poll(void)
{
    int v, code, state;
    if (!ok) return 0;
    v = read_fifo();
    if (!v) return 0;
    code = (v >> 8) & 0xFF;
    state = v & 0xFF;
    update_modifiers(code, state);
    if (state != ST_PRESSED && state != ST_HOLD) return 0;
    if (code == KEY_CTRL_CODE || code == KEY_ALT_CODE || code == KEY_LSHIFT_CODE || code == KEY_RSHIFT_CODE) return 0;
    if ((modifiers & MOD_CTRL) && code >= 'a' && code <= 'z') return code - 'a' + 1;
    if ((modifiers & MOD_CTRL) && code >= 'A' && code <= 'Z') return code - 'A' + 1;
    return code;
}
