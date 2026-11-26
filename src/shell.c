/* shell.c - the CalcIDE command line.
 *
 * DOS-flavoured, with Linux-style file commands. A command line is split
 * into words (double quotes group words), the first word is looked up in
 * the alias table and then the command table; failing that, NAME.cbc in the
 * current directory or in /bin is run as a program.
 */
#include "shell.h"
#include "console.h"
#include "config.h"
#include "vfs.h"
#include "hal.h"
#include "pf.h"
#include "vm.h"
#include "cc.h"
#include "editor.h"
#include "lineedit.h"
#include "sound.h"
#include "selftest.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <strings.h>

#define MAXARGS 16

static LeHistory hist;
static int last_rc;

typedef struct {
    const char *name;
    int (*fn)(int argc, char **argv);
    const char *usage;
    const char *help;
} Cmd;

static const Cmd cmds[];

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static void color_msg(int fg, const char *fmt, const char *arg)
{
    con_set_fg(fg);
    con_printf(fmt, arg);
    con_default_color();
}

static void err(const char *cmd, const char *what, const char *path)
{
    con_set_fg(9);
    if (path) con_printf("%s: %s: %s\n", cmd, path, what);
    else con_printf("%s: %s\n", cmd, what);
    con_default_color();
}

static int need_sd(const char *cmd)
{
    if (vfs_mounted()) return 1;
    err(cmd, "no SD card mounted (try 'mount')", 0);
    return 0;
}

static int resolve(const char *cmd, const char *in, char *out)
{
    if (!vfs_resolve(in, out)) { err(cmd, "path too long", in); return 0; }
    return 1;
}

static int is_yes(void)
{
    int k = con_getkey();
    con_printf("%c\n", isprint(k) ? k : ' ');
    return k == 'y' || k == 'Y';
}

/* wait for a key at a screenful; returns 0 if the user pressed q/Esc */
static int page_rows;
static int more_check(void)
{
    if (++page_rows < CON_ROWS - 1) return 1;
    page_rows = 0;
    con_set_fg(14);
    con_puts("-- more (q to stop) --");
    con_default_color();
    {
        int k = con_getkey();
        con_puts("\r                      \r");
        return !(k == 'q' || k == 'Q' || k == KEY_ESC || k == KEY_CTRL('c') || k == KEY_BREAK);
    }
}

static void fmt_size(uint32_t n, char *buf)
{
    if (n < 10000) sprintf(buf, "%u", (unsigned)n);
    else if (n < 10000u * 1024u) sprintf(buf, "%uK", (unsigned)((n + 1023) / 1024));
    else sprintf(buf, "%uM", (unsigned)((n + 1024 * 1024 - 1) / (1024 * 1024)));
}

static int countdown(const char *what, int secs)
{
    uint32_t end;
    if (secs <= 0) return 1;
    con_printf("%s in %d s (any key cancels)\n", what, secs);
    end = hal_millis() + (uint32_t)secs * 1000u;
    while ((int32_t)(end - hal_millis()) > 0) {
        if (con_inkey() >= 0) { con_puts("cancelled\n"); return 0; }
        con_flush();
        hal_sleep_ms(20);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* system commands                                                     */
/* ------------------------------------------------------------------ */

static int c_battery(int argc, char **argv)
{
    int ch = 0, pct = hal_battery(&ch), i, bar;
    if (pct < 0) { err("battery", "cannot read the battery (keyboard controller did not answer)", 0); return 1; }
    con_printf("Battery: %d%%  %s\n[", pct, ch ? "charging" : "on battery");
    bar = pct / 5;
    con_set_fg(pct > 50 ? 10 : pct > 20 ? 11 : 9);
    for (i = 0; i < 20; i++) con_putc(i < bar ? '#' : '.');
    con_default_color();
    con_puts("]\n");
    return 0;
}

static int c_shutdown(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 0;
    if (!countdown("Power off", n)) return 1;
    vfs_sync();
    con_puts("Powering off...\n");
    con_flush();
    hal_poweroff();
    return 0;
}

static int c_reset(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 0;
    if (!countdown("Restart", n)) return 1;
    vfs_sync();
    con_puts("Restarting...\n");
    con_flush();
    hal_reboot();
    return 0;
}

static int c_width(int argc, char **argv)
{
    if (argc < 2) { con_printf("width %d (40 = 8x10 font, 64 = 5x10 font)\n", con_cols()); return 0; }
    if (atoi(argv[1]) != 40 && atoi(argv[1]) != 64) { err("width", "use 40 or 64", 0); return 1; }
    g_conf.width = atoi(argv[1]);
    g_conf.font = g_conf.width == 64;
    con_set_width(g_conf.width);
    con_clear();
    conf_save();
    con_printf("%d x %d\n", con_cols(), con_rows());
    return 0;
}

static int c_font(int argc, char **argv)
{
    if (argc < 2) {
        con_printf("font %s  (fonts: 8x10 = 40 columns, 5x10 = 64 columns)\n", g_conf.font ? "5x10" : "8x10");
        return 0;
    }
    if (!strcmp(argv[1], "8x10") || !strcmp(argv[1], "0") || !strcasecmp(argv[1], "dos")) g_conf.font = 0;
    else if (!strcmp(argv[1], "5x10") || !strcmp(argv[1], "1") || !strcasecmp(argv[1], "small")) g_conf.font = 1;
    else { err("font", "use 8x10 or 5x10", 0); return 1; }
    g_conf.width = g_conf.font ? 64 : 40;
    con_set_width(g_conf.width);
    con_clear();
    conf_save();
    return 0;
}

static int c_cls(int argc, char **argv)
{
    con_default_color();
    con_clear();
    return 0;
}

typedef struct { const char *name; int fg, bg, efg, ebg; } Theme;
static const Theme themes[] = {
    {"dos", 7, 0, 15, 4},
    {"classic", 15, 4, 15, 4},
    {"turbo", 14, 1 + 3, 14, 4},
    {"green", 10, 0, 10, 0},
    {"amber", 11, 0, 11, 0},
    {"paper", 0, 15, 0, 15},
    {"matrix", 2, 0, 10, 0},
    {"night", 12, 0, 7, 0},
    {0, 0, 0, 0, 0}
};

static int c_color(int argc, char **argv)
{
    int fg, bg, i;
    if (argc < 2) {
        con_printf("color %s on %s   (editor %s on %s)\n", conf_color_name(g_conf.fg), conf_color_name(g_conf.bg),
                   conf_color_name(g_conf.ed_fg), conf_color_name(g_conf.ed_bg));
        con_puts("usage: color FG [BG] | color theme NAME | color list\n");
        con_puts("themes:");
        for (i = 0; themes[i].name; i++) con_printf(" %s", themes[i].name);
        con_puts("\n");
        return 0;
    }
    if (!strcmp(argv[1], "list")) {
        for (i = 0; i < 16; i++) {
            con_set_color(i == 0 ? 7 : i, i == 0 ? 8 : 0);
            con_printf("%2d %-13s", i, conf_color_name(i));
            con_default_color();
            if (i % 2 == 1 || con_cols() < 64) con_puts("\n");
        }
        con_default_color();
        return 0;
    }
    if (!strcmp(argv[1], "theme") || !strcmp(argv[0], "theme")) {
        const char *n = !strcmp(argv[0], "theme") ? argv[1] : (argc > 2 ? argv[2] : "");
        for (i = 0; themes[i].name; i++) if (!strcasecmp(themes[i].name, n)) break;
        if (!themes[i].name) { err("theme", "unknown theme", n); return 1; }
        g_conf.fg = themes[i].fg; g_conf.bg = themes[i].bg;
        g_conf.ed_fg = themes[i].efg; g_conf.ed_bg = themes[i].ebg;
        con_default_color();
        con_clear();
        conf_save();
        return 0;
    }
    fg = conf_color(argv[1]);
    bg = argc > 2 ? conf_color(argv[2]) : g_conf.bg;
    if (fg < 0 || bg < 0) { err("color", "colours are 0-15 or names (color list)", 0); return 1; }
    if (fg == bg) { err("color", "foreground and background are the same", 0); return 1; }
    g_conf.fg = fg;
    g_conf.bg = bg;
    con_default_color();
    con_clear();
    conf_save();
    return 0;
}

static int onoff(const char *s, int cur)
{
    if (!s) return !cur;
    if (!strcasecmp(s, "on") || !strcmp(s, "1") || !strcasecmp(s, "show")) return 1;
    if (!strcasecmp(s, "off") || !strcmp(s, "0") || !strcasecmp(s, "hide")) return 0;
    return -1;
}

static int c_cursor(int argc, char **argv)
{
    int v;
    if (argc > 1 && (!strcasecmp(argv[1], "block") || !strcasecmp(argv[1], "underline"))) {
        g_conf.cursor_style = !strcasecmp(argv[1], "block");
        conf_save();
        return 0;
    }
    v = onoff(argc > 1 ? argv[1] : 0, g_conf.cursor);
    if (v < 0) { err("cursor", "use on, off, block or underline", 0); return 1; }
    g_conf.cursor = v;
    con_cursor(v);
    conf_save();
    con_printf("cursor %s\n", v ? "on" : "off");
    return 0;
}

static int c_beep(int argc, char **argv)
{
    int hz = argc > 1 ? atoi(argv[1]) : 880;
    int ms = argc > 2 ? atoi(argv[2]) : 200;
    if (hz < 20 || hz > 20000 || ms < 1 || ms > 10000) { err("beep", "frequency 20-20000 Hz, duration 1-10000 ms", 0); return 1; }
    if (g_conf.mute) { con_puts("(muted - 'mute off' to hear it)\n"); return 0; }
    hal_tone(hz, ms);
    hal_tone_off();
    return 0;
}

static int c_mute(int argc, char **argv)
{
    int v = onoff(argc > 1 ? argv[1] : 0, g_conf.mute);
    if (v < 0) { err("mute", "use on or off", 0); return 1; }
    g_conf.mute = v;
    if (v) hal_tone_off();
    conf_save();
    con_printf("sound %s\n", v ? "muted" : "on");
    return 0;
}

static int c_df(int argc, char **argv)
{
    uint32_t tot, fr;
    if (!need_sd("df")) return 1;
    if (!vfs_space(&tot, &fr)) { err("df", vfs_error(), 0); return 1; }
    con_printf("SD card (%s)\n", vfs_fstype());
    con_printf(" total %8u KB  (%u MB)\n", (unsigned)tot, (unsigned)(tot / 1024));
    con_printf(" used  %8u KB  %d%%\n", (unsigned)(tot - fr), tot ? (int)((uint64_t)(tot - fr) * 100 / tot) : 0);
    con_printf(" free  %8u KB  (%u MB)\n", (unsigned)fr, (unsigned)(fr / 1024));
    return 0;
}

static uint32_t tree_files, tree_dirs, tree_bytes;

static void tree_walk(const char *path, char *prefix, int depth)
{
    void *d;
    vdirent e, cur;
    int have, pl = (int)strlen(prefix);
    if (depth > 12) return;
    d = vfs_opendir(path);
    if (!d) return;
    have = vfs_readdir(d, &cur);
    while (have) {
        int last;
        char sz[16];
        have = vfs_readdir(d, &e);
        last = !have;
        if (!more_check()) { vfs_closedir(d); return; }
        con_printf("%s%s", prefix, last ? "`-- " : "|-- ");
        if (cur.isdir) {
            con_set_fg(12);
            con_printf("%s/\n", cur.name);
            con_default_color();
            tree_dirs++;
            if (pl + 5 < 60) {
                char sub[VFS_PATH];
                snprintf(sub, sizeof sub, "%s%s%s", path, strcmp(path, "/") ? "/" : "", cur.name);
                strcat(prefix, last ? "    " : "|   ");
                tree_walk(sub, prefix, depth + 1);
                prefix[pl] = 0;
            }
        } else {
            fmt_size(cur.size, sz);
            con_printf("%s ", cur.name);
            con_set_fg(8);
            con_printf("%s\n", sz);
            con_default_color();
            tree_files++;
            tree_bytes += cur.size;
        }
        cur = e;
    }
    vfs_closedir(d);
}

static int c_tree(int argc, char **argv)
{
    char path[VFS_PATH], prefix[80] = "";
    uint32_t tot, fr;
    if (!need_sd("tree")) return 1;
    if (!resolve("tree", argc > 1 ? argv[1] : ".", path)) return 1;
    tree_files = tree_dirs = tree_bytes = 0;
    page_rows = 0;
    con_set_fg(12);
    con_printf("%s\n", path);
    con_default_color();
    tree_walk(path, prefix, 0);
    con_printf("%u directories, %u files, %u KB\n", (unsigned)tree_dirs, (unsigned)tree_files, (unsigned)((tree_bytes + 1023) / 1024));
    if (vfs_space(&tot, &fr)) con_printf("SD: %u KB used, %u KB free\n", (unsigned)(tot - fr), (unsigned)fr);
    return 0;
}

static int c_sd(int argc, char **argv)
{
    uint32_t tot, fr;
    char label[40] = "";
    con_printf("SD card : %s\n", hal_sd_present() ? "present" : "not detected");
    con_printf("Type    : %s\n", hal_sd_present() ? (hal_sd_hc() ? "SDHC/SDXC (block addressed)" : "SDSC (byte addressed)") : "-");
    con_printf("Mounted : %s\n", vfs_mounted() ? "yes" : "no");
    if (vfs_mounted()) {
        vfs_label(label, sizeof label);
        con_printf("Format  : %s\n", vfs_fstype());
        if (label[0]) con_printf("Label   : %s\n", label);
        if (vfs_space(&tot, &fr)) {
            con_printf("Size    : %u MB\n", (unsigned)(tot / 1024));
            con_printf("Free    : %u MB\n", (unsigned)(fr / 1024));
        }
    }
    return 0;
}

static int c_mount(int argc, char **argv)
{
    if (vfs_mounted()) { con_puts("already mounted\n"); return 0; }
    if (!vfs_mount()) { err("mount", "no card, or not FAT/exFAT formatted", 0); return 1; }
    strcpy(vfs_cwd, "/");
    con_puts("SD card mounted\n");
    if (conf_load()) con_puts("settings loaded from " CONF_PATH "\n");
    return 0;
}

static int c_unmount(int argc, char **argv)
{
    if (!vfs_mounted()) { con_puts("not mounted\n"); return 0; }
    vfs_sync();
    vfs_unmount();
    strcpy(vfs_cwd, "/");
    con_puts("SD card unmounted - safe to remove\n");
    return 0;
}

static int c_sync(int argc, char **argv)
{
    if (!need_sd("sync")) return 1;
    vfs_sync();
    con_puts("buffers flushed\n");
    return 0;
}

static int c_mem(int argc, char **argv)
{
    size_t tot = hal_ram_total(), fr = hal_ram_free(), lg = hal_ram_largest();
    con_printf("SRAM total    %4u KB\n", (unsigned)(tot / 1024));
    con_printf("heap free     %4u KB\n", (unsigned)(fr / 1024));
    con_printf("largest block %4u KB\n", (unsigned)(lg / 1024));
    con_printf("for programs  %4u KB%s\n",
               (unsigned)(g_conf.vm_mem_kb ? (unsigned)g_conf.vm_mem_kb : (unsigned)(lg > 24 * 1024 ? (lg - 16 * 1024) / 1024 : 0)),
               g_conf.vm_mem_kb ? "" : " (auto)");
    return 0;
}

static int c_uptime(int argc, char **argv)
{
    uint32_t s = hal_millis() / 1000;
    con_printf("up %ud %02u:%02u:%02u\n", (unsigned)(s / 86400), (unsigned)(s / 3600 % 24), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    return 0;
}

static int c_version(int argc, char **argv)
{
    con_printf("CalcIDE %s - on-device C IDE for the PicoCalc\n", CALCIDE_VERSION);
    con_printf("Tiny-C compiler %s, bytecode v2, self-hosting\n", CALCIDE_VERSION);
    con_printf("chip %s, built %s\n", hal_chip(), __DATE__);
    return 0;
}

static int c_temp(int argc, char **argv)
{
    float t = hal_temp_c();
    con_printf("%s die temperature: %.1f C (%.1f F)\n", hal_chip(), t, t * 9.0f / 5.0f + 32.0f);
    return 0;
}

static int c_clock(int argc, char **argv)
{
    if (argc > 1) {
        int *slot = &g_conf.cpu_mhz;
        const char *v = argv[1];
        int m;
        if (!strcasecmp(argv[1], "idle")) { slot = &g_conf.cpu_mhz_idle; v = argc > 2 ? argv[2] : "0"; }
        else if (!strcasecmp(argv[1], "run")) { slot = &g_conf.cpu_mhz_run; v = argc > 2 ? argv[2] : "0"; }
        m = atoi(v);
        if (m != 0 && (m < 48 || m > 300)) { err("clock", "MHz between 48 and 300, or 0 for the default", 0); return 1; }
        *slot = m;
        conf_save();
        clock_idle();
    }
    con_printf("CPU clock: %d.%03d MHz\n", hal_cpu_khz() / 1000, hal_cpu_khz() % 1000);
    con_printf("  base %s, idle %s, run %s\n",
               g_conf.cpu_mhz ? "set" : "default",
               g_conf.cpu_mhz_idle ? "set" : "= base", g_conf.cpu_mhz_run ? "set" : "= base");
    if (g_conf.cpu_mhz || g_conf.cpu_mhz_idle || g_conf.cpu_mhz_run)
        con_printf("  (cpu_mhz %d, cpu_mhz_idle %d, cpu_mhz_run %d)\n", g_conf.cpu_mhz, g_conf.cpu_mhz_idle, g_conf.cpu_mhz_run);
    return 0;
}

static int c_backlight(int argc, char **argv)
{
    if (argc < 2) { con_printf("backlight %d, keyboard %d (0-255)\n", g_conf.backlight, g_conf.kbd_light); return 0; }
    g_conf.backlight = atoi(argv[1]) < 0 ? 0 : atoi(argv[1]) > 255 ? 255 : atoi(argv[1]);
    if (argc > 2) g_conf.kbd_light = atoi(argv[2]) < 0 ? 0 : atoi(argv[2]) > 255 ? 255 : atoi(argv[2]);
    hal_backlight(g_conf.backlight);
    hal_kbdlight(g_conf.kbd_light);
    conf_save();
    return 0;
}

static int c_exit(int argc, char **argv)
{
    /* With the PicoCalc SD boot loader in flash, a restart lands back in
     * its menu; on a stand-alone install it simply restarts CalcIDE. */
    vfs_sync();
    con_puts("Returning to the PicoCalc launcher...\n");
    con_flush();
    hal_sleep_ms(200);
    hal_reboot();
    return 0;
}

static int c_bye(int argc, char **argv)
{
    con_puts("Bye!\n");
    con_flush();
    hal_sleep_ms(400);
    vfs_sync();
    hal_poweroff();
    return 0;
}

static int c_bootsel(int argc, char **argv)
{
    con_puts("Rebooting into the USB bootloader (drop a .uf2 on the RP2350/RPI-RP2 drive)...\n");
    vfs_sync();
    hal_reboot_to_bootloader();
    return 0;
}

static int c_play(int argc, char **argv)
{
    char notes[256] = "";
    int i, r;
    if (argc < 2) { con_puts("usage: play NOTES | play FILE.song\n  e.g. play T140 C4 E4 G4/2 R C5/2 | K/4 SN/4 K/4 SN/4\n"); return 1; }
    for (i = 1; i < argc; i++) {
        if (i > 1) strncat(notes, " ", sizeof notes - strlen(notes) - 1);
        strncat(notes, argv[i], sizeof notes - strlen(notes) - 1);
    }
    if (g_conf.mute) con_puts("(muted - 'mute off' to hear it)\n");
    /* one argument that names a file: a .song */
    if (argc == 2 && (strchr(argv[1], '.') || strchr(argv[1], '/')) && vfs_mounted()) {
        char full[VFS_PATH];
        int isdir = 0;
        if (vfs_resolve(argv[1], full) && vfs_stat(full, &isdir, 0) && !isdir) {
            con_printf("playing %s (break key stops)\n", vfs_basename(full));
            con_flush();
            r = play_file(full);
            if (r < 0) { err("play", play_error(), 0); return 1; }
            return 0;
        }
    }
    r = play_notes(notes);
    if (r < 0) { err("play", play_error(), 0); return 1; }
    return 0;
}

static int c_wav(int argc, char **argv)
{
    int r;
    if (argc < 2) { con_puts("usage: wav FILE.wav\n"); return 1; }
    if (!need_sd("wav")) return 1;
    if (g_conf.mute) con_puts("(muted - 'mute off' to hear it)\n");
    r = play_wav(argv[1]);
    if (r < 0) { err("wav", play_error(), 0); return 1; }
    if (r == 1) con_puts("stopped\n");
    return 0;
}

static int c_gpio(int argc, char **argv)
{
    int pin, v;
    if (argc < 2) {
        con_puts("usage: gpio PIN [in|out|up|down|0|1]\nfree pins:");
        for (pin = 0; pin < 30; pin++) if (hal_gpio_free(pin)) con_printf(" %d", pin);
        con_puts("\n");
        return 0;
    }
    pin = atoi(argv[1]);
    if (!hal_gpio_free(pin)) { err("gpio", "pin is used by the PicoCalc or does not exist", argv[1]); return 1; }
    if (argc > 2) {
        const char *m = argv[2];
        if (!strcasecmp(m, "in")) hal_gpio_mode(pin, HAL_GPIO_IN);
        else if (!strcasecmp(m, "out")) hal_gpio_mode(pin, HAL_GPIO_OUT);
        else if (!strcasecmp(m, "up")) hal_gpio_mode(pin, HAL_GPIO_PULLUP);
        else if (!strcasecmp(m, "down")) hal_gpio_mode(pin, HAL_GPIO_PULLDOWN);
        else if (!strcmp(m, "0") || !strcmp(m, "1")) { hal_gpio_mode(pin, HAL_GPIO_OUT); hal_gpio_write(pin, m[0] == '1'); }
        else { err("gpio", "use in, out, up, down, 0 or 1", 0); return 1; }
    }
    v = hal_gpio_read(pin);
    con_printf("GP%d = %d\n", pin, v);
    return 0;
}

static int c_selftest(int argc, char **argv)
{
    return selftest_run(argc, argv);
}

#define HIST_FILE "/calcide/history.txt"
static int c_history(int argc, char **argv)
{
    int i;
    if (argc > 1 && (!strcmp(argv[1], "-c") || !strcmp(argv[1], "clear"))) {
        le_history_clear(&hist);
        if (vfs_mounted()) vfs_remove(HIST_FILE);
        return 0;
    }
    page_rows = 0;
    for (i = 0; i < hist.count; i++) {
        if (!more_check()) break;
        con_printf("%3d  %s\n", i + 1, hist.lines[i]);
    }
    return 0;
}

static int c_alias(int argc, char **argv)
{
    int i;
    char cmd[80] = "";
    char name[20];
    if (argc < 2) {
        for (i = 0; i < CONF_MAX_ALIAS; i++)
            if (g_conf.alias_name[i][0]) con_printf("%-10s = %s\n", g_conf.alias_name[i], g_conf.alias_cmd[i]);
        return 0;
    }
    {
        char *eq = strchr(argv[1], '=');
        if (eq) {
            int nl = (int)(eq - argv[1]);
            if (nl >= (int)sizeof name) nl = sizeof name - 1;
            memcpy(name, argv[1], (size_t)nl);
            name[nl] = 0;
            snprintf(cmd, sizeof cmd, "%s", eq + 1);
            i = 2;
        } else {
            snprintf(name, sizeof name, "%s", argv[1]);
            i = 2;
            if (argc == 2) {
                const char *a = alias_find(name);
                if (a) con_printf("%s = %s\n", name, a);
                else err("alias", "no such alias", name);
                return a ? 0 : 1;
            }
        }
        for (; i < argc; i++) {
            if (cmd[0] && strlen(cmd) + 1 < sizeof cmd) strcat(cmd, " ");
            strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
        }
    }
    if (alias_set(name, cmd) < 0) { err("alias", "alias table full or name too long", 0); return 1; }
    conf_save();
    return 0;
}

static int c_unalias(int argc, char **argv)
{
    if (argc < 2) { con_puts("usage: unalias NAME\n"); return 1; }
    alias_set(argv[1], "");
    conf_save();
    return 0;
}

static int c_help(int argc, char **argv);

static int c_set(int argc, char **argv)
{
    char val[100] = "";
    int i, r;
    if (argc < 2) { page_rows = 0; conf_print(); return 0; }
    if (!strcmp(argv[1], "save")) return conf_save() ? 0 : 1;
    if (!strcmp(argv[1], "reload")) { conf_load(); con_default_color(); con_set_width(g_conf.width); return 0; }
    if (!strcmp(argv[1], "defaults")) {
        conf_defaults();
        con_set_width(g_conf.width);
        con_default_color();
        con_clear();
        conf_save();
        return 0;
    }
    for (i = 2; i < argc; i++) {
        if (i > 2) strncat(val, " ", sizeof val - strlen(val) - 1);
        strncat(val, argv[i], sizeof val - strlen(val) - 1);
    }
    if (argc == 2) {
        char *eq = strchr(argv[1], '=');
        if (!eq) { err("set", "usage: set KEY VALUE", 0); return 1; }
        *eq = 0;
        snprintf(val, sizeof val, "%s", eq + 1);
    }
    r = conf_set(argv[1], val);
    if (r == 0) { err("set", "unknown setting", argv[1]); return 1; }
    if (r < 0) { err("set", "bad value", val); return 1; }
    con_set_width(g_conf.width);
    hal_backlight(g_conf.backlight);
    hal_kbdlight(g_conf.kbd_light);
    con_cursor(g_conf.cursor);
    hist.max = g_conf.history;
    if (!strcasecmp(argv[1], "fg") || !strcasecmp(argv[1], "bg")) con_default_color();
    conf_save();
    return 0;
}

static int c_echo(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) con_printf("%s%s", argv[i], i + 1 < argc ? " " : "");
    con_puts("\n");
    return 0;
}

static int c_sleep(int argc, char **argv)
{
    uint32_t end = hal_millis() + (uint32_t)(argc > 1 ? atoi(argv[1]) : 1) * 1000u;
    while ((int32_t)(end - hal_millis()) > 0) {
        if (con_poll_break()) return 1;
        hal_sleep_ms(10);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* files                                                               */
/* ------------------------------------------------------------------ */

static int c_pwd(int argc, char **argv)
{
    con_printf("%s\n", vfs_cwd);
    return 0;
}

static int c_cd(int argc, char **argv)
{
    char p[VFS_PATH];
    int isdir = 0;
    if (!need_sd("cd")) return 1;
    if (!resolve("cd", argc > 1 ? argv[1] : (g_conf.home[0] ? g_conf.home : "/"), p)) return 1;
    if (!vfs_stat(p, &isdir, 0) || !isdir) { err("cd", "no such directory", p); return 1; }
    strcpy(vfs_cwd, p);
    return 0;
}

static int ent_cmp(const void *a, const void *b)
{
    const vdirent *x = (const vdirent *)a, *y = (const vdirent *)b;
    if (x->isdir != y->isdir) return y->isdir - x->isdir;   /* directories first */
    return strcasecmp(x->name, y->name);
}

/* read a whole directory, sorted; caller frees */
static vdirent *read_sorted(const char *dir, int *count)
{
    void *d = vfs_opendir(dir);
    vdirent *list = 0, *nl;
    int n = 0, cap = 0;
    if (!d) return 0;
    for (;;) {
        if (n == cap) {
            cap = cap ? cap * 2 : 32;
            nl = (vdirent *)realloc(list, (size_t)cap * sizeof *list);
            if (!nl) break;
            list = nl;
        }
        if (!vfs_readdir(d, &list[n])) break;
        n++;
    }
    vfs_closedir(d);
    if (list) qsort(list, (size_t)n, sizeof *list, ent_cmp);
    *count = n;
    return list ? list : (vdirent *)calloc(1, sizeof(vdirent));
}

static int ls_one(const char *dir, int lng)
{
    int cnt = 0, idx;
    vdirent *all = read_sorted(dir, &cnt), e;
    int n = 0, col = 0, cols = con_cols(), w;
    uint32_t bytes = 0;
    if (!all) return -1;
    page_rows = 0;
    w = cols >= 64 ? 16 : 13;
    for (idx = 0; idx < cnt; idx++) {
        e = all[idx];
        n++;
        if (lng) {
            char sz[16];
            if (!more_check()) break;
            fmt_size(e.size, sz);
            if (e.isdir) { con_set_fg(12); con_printf("  <DIR>  %s/\n", e.name); }
            else {
                const char *ext = strrchr(e.name, '.');
                con_printf(" %7s  ", sz);
                if (ext && (!strcasecmp(ext, ".c") || !strcasecmp(ext, ".h"))) con_set_fg(10);
                else if (ext && !strcasecmp(ext, ".cbc")) con_set_fg(14);
                con_printf("%s\n", e.name);
                bytes += e.size;
            }
            con_default_color();
        } else {
            int l = (int)strlen(e.name) + (e.isdir ? 1 : 0);
            int slots = (l + 2 + w - 1) / w;
            const char *ext = strrchr(e.name, '.');
            if (col + slots * w > cols && col) { con_puts("\n"); col = 0; if (!more_check()) break; }
            if (e.isdir) con_set_fg(12);
            else if (ext && (!strcasecmp(ext, ".c") || !strcasecmp(ext, ".h"))) con_set_fg(10);
            else if (ext && !strcasecmp(ext, ".cbc")) con_set_fg(14);
            con_printf("%s%s", e.name, e.isdir ? "/" : "");
            con_default_color();
            col += l;
            while (col % w) { con_putc(' '); col++; }
            if (col >= cols) { col = 0; }
        }
    }
    free(all);
    if (!lng && col) con_puts("\n");
    if (lng) con_printf("%d entries, %u bytes\n", n, (unsigned)bytes);
    return n;
}

static int c_ls(int argc, char **argv)
{
    char p[VFS_PATH];
    int lng = !strcmp(argv[0], "dir"), i, any = 0;
    if (!need_sd("ls")) return 1;
    for (i = 1; i < argc; i++) if (!strcmp(argv[i], "-l")) lng = 1;
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        any = 1;
        if (!resolve("ls", argv[i], p)) return 1;
        {
            int isdir = 0;
            uint32_t sz = 0;
            if (!vfs_stat(p, &isdir, &sz)) { err("ls", "not found", p); continue; }
            if (!isdir) { con_printf("%s  %u bytes\n", p, (unsigned)sz); continue; }
        }
        if (ls_one(p, lng) < 0) err("ls", vfs_error(), p);
    }
    if (!any && ls_one(vfs_cwd, lng) < 0) err("ls", vfs_error(), vfs_cwd);
    return 0;
}

static int c_mkdir(int argc, char **argv)
{
    char p[VFS_PATH];
    int i, rc = 0, parents = 0;
    if (!need_sd("mkdir")) return 1;
    if (argc < 2) { con_puts("usage: mkdir [-p] DIR...\n"); return 1; }
    for (i = 1; i < argc; i++) if (!strcmp(argv[i], "-p")) parents = 1;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-p")) continue;
        if (!resolve("mkdir", argv[i], p)) return 1;
        if (parents) {
            char part[VFS_PATH];
            char *s;
            strcpy(part, p);
            for (s = part + 1; *s; s++) {
                if (*s == '/') { *s = 0; if (!vfs_stat(part, 0, 0)) vfs_mkdir(part); *s = '/'; }
            }
            if (!vfs_stat(part, 0, 0) && !vfs_mkdir(part)) { err("mkdir", vfs_error(), p); rc = 1; }
        } else if (!vfs_mkdir(p)) { err("mkdir", vfs_error(), p); rc = 1; }
    }
    return rc;
}

static int rm_tree(const char *p)
{
    int isdir = 0;
    if (!vfs_stat(p, &isdir, 0)) return 0;
    if (isdir) {
        for (;;) {
            /* re-open each time: removing entries while listing is unsafe */
            void *d = vfs_opendir(p);
            vdirent e;
            char sub[VFS_PATH];
            int got;
            if (!d) return 0;
            got = vfs_readdir(d, &e);
            vfs_closedir(d);
            if (!got) break;
            snprintf(sub, sizeof sub, "%s/%s", p, e.name);
            if (!rm_tree(sub)) return 0;
        }
    }
    return vfs_remove(p);
}

static int c_rm(int argc, char **argv)
{
    char p[VFS_PATH];
    int i, rec = 0, rc = 0;
    if (!need_sd("rm")) return 1;
    for (i = 1; i < argc; i++) if (!strcmp(argv[i], "-r") || !strcmp(argv[i], "-rf")) rec = 1;
    if (argc < 2) { con_puts("usage: rm [-r] PATH...\n"); return 1; }
    for (i = 1; i < argc; i++) {
        int isdir = 0;
        if (argv[i][0] == '-') continue;
        if (!resolve("rm", argv[i], p)) return 1;
        if (!strcmp(p, "/")) { err("rm", "refusing to remove /", 0); return 1; }
        if (!vfs_stat(p, &isdir, 0)) { err("rm", "not found", p); rc = 1; continue; }
        if (isdir && !rec) { err("rm", "is a directory (use rm -r or rmdir)", p); rc = 1; continue; }
        if (!(rec ? rm_tree(p) : vfs_remove(p))) { err("rm", vfs_error(), p); rc = 1; }
    }
    return rc;
}

static int c_rmdir(int argc, char **argv)
{
    char p[VFS_PATH];
    int isdir = 0;
    if (!need_sd("rmdir")) return 1;
    if (argc < 2) { con_puts("usage: rmdir DIR\n"); return 1; }
    if (!resolve("rmdir", argv[1], p)) return 1;
    if (!vfs_stat(p, &isdir, 0) || !isdir) { err("rmdir", "no such directory", p); return 1; }
    if (!vfs_remove(p)) { err("rmdir", "directory not empty?", p); return 1; }
    return 0;
}

static int copy_file(const char *from, const char *to)
{
    vfile *a, *b;
    char *buf;
    int n, ok = 1;
    a = vfs_open(from, "r");
    if (!a) { err("cp", vfs_error(), from); return 0; }
    b = vfs_open(to, "w");
    if (!b) { vfs_close(a); err("cp", vfs_error(), to); return 0; }
    buf = (char *)malloc(4096);
    if (!buf) { vfs_close(a); vfs_close(b); return 0; }
    while ((n = vfs_read(a, buf, 4096)) > 0)
        if (vfs_write(b, buf, n) != n) { ok = 0; err("cp", "write failed (card full?)", to); break; }
    free(buf);
    vfs_close(a);
    if (vfs_close(b) != 0) ok = 0;
    return ok;
}

/* "cp a dir" puts a inside dir */
static int target_path(const char *src, const char *dst, char *out)
{
    int isdir = 0;
    if (vfs_stat(dst, &isdir, 0) && isdir) {
        if (strlen(dst) + strlen(vfs_basename(src)) + 2 > VFS_PATH) return 0;
        sprintf(out, "%s%s%s", dst, strcmp(dst, "/") ? "/" : "", vfs_basename(src));
    } else strcpy(out, dst);
    return 1;
}

static int c_cp(int argc, char **argv)
{
    char a[VFS_PATH], b[VFS_PATH], t[VFS_PATH];
    int isdir = 0;
    if (!need_sd("cp")) return 1;
    if (argc < 3) { con_puts("usage: cp FROM TO\n"); return 1; }
    if (!resolve("cp", argv[1], a) || !resolve("cp", argv[2], b) || !target_path(a, b, t)) return 1;
    if (!vfs_stat(a, &isdir, 0)) { err("cp", "not found", a); return 1; }
    if (isdir) { err("cp", "cannot copy a directory", a); return 1; }
    if (!strcmp(a, t)) { err("cp", "source and destination are the same", 0); return 1; }
    return copy_file(a, t) ? 0 : 1;
}

static int c_mv(int argc, char **argv)
{
    char a[VFS_PATH], b[VFS_PATH], t[VFS_PATH];
    if (!need_sd("mv")) return 1;
    if (argc < 3) { con_puts("usage: mv FROM TO\n"); return 1; }
    if (!resolve("mv", argv[1], a) || !resolve("mv", argv[2], b) || !target_path(a, b, t)) return 1;
    if (!vfs_stat(a, 0, 0)) { err("mv", "not found", a); return 1; }
    if (!vfs_rename(a, t)) { err("mv", vfs_error(), t); return 1; }
    return 0;
}

static int c_cat(int argc, char **argv)
{
    char p[VFS_PATH];
    int i, rc = 0, pager = !strcmp(argv[0], "more");
    for (i = 1; i < argc; i++) {
        vfile *f;
        char buf[256];
        int n, col = 0;
        if (!resolve("cat", argv[i], p)) return 1;
        f = vfs_open(p, "r");
        if (!f) { err(argv[0], vfs_error(), p); rc = 1; continue; }
        page_rows = 0;
        col = 0;
        while ((n = vfs_read(f, buf, sizeof buf)) > 0) {
            int k, stop = 0;
            for (k = 0; k < n; k++) {
                char c = buf[k];
                if (c == '\r') continue;
                con_putc(c);
                col = (c == '\n') ? 0 : col + 1;
                if (c == '\n' || col >= con_cols()) {
                    if (col >= con_cols()) col = 0;
                    if (pager && !more_check()) { stop = 1; break; }
                }
            }
            if (stop || con_poll_break()) break;
        }
        vfs_close(f);
        if (col != 0) con_putc('\n');   /* the file did not end with a newline */
    }
    if (argc < 2) { con_printf("usage: %s FILE...\n", argv[0]); return 1; }
    return rc;
}

static int c_touch(int argc, char **argv)
{
    char p[VFS_PATH];
    int i;
    if (!need_sd("touch")) return 1;
    for (i = 1; i < argc; i++) {
        vfile *f;
        if (!resolve("touch", argv[i], p)) return 1;
        if (vfs_stat(p, 0, 0)) continue;
        f = vfs_open(p, "w");
        if (!f) { err("touch", vfs_error(), p); return 1; }
        vfs_close(f);
    }
    return 0;
}

static void hexdump(const uint8_t *d, int n, uint32_t base)
{
    int i, j, per = con_cols() >= 64 ? 16 : 8;
    page_rows = 0;
    for (i = 0; i < n; i += per) {
        if (!more_check()) return;
        con_printf("%05x ", (unsigned)(base + (uint32_t)i));
        for (j = 0; j < per; j++) {
            if (i + j < n) con_printf("%02x ", d[i + j]);
            else con_puts("   ");
        }
        for (j = 0; j < per && i + j < n; j++) con_putc(d[i + j] >= 32 && d[i + j] < 127 ? d[i + j] : '.');
        con_puts("\n");
    }
}

static int c_hex(int argc, char **argv)
{
    char *data;
    int n;
    if (argc < 2) { con_puts("usage: hex FILE\n"); return 1; }
    data = vfs_load(argv[1], &n, 0);
    if (!data) { err("hex", "cannot read", argv[1]); return 1; }
    hexdump((uint8_t *)data, n, 0);
    free(data);
    return 0;
}

static int c_edit(int argc, char **argv)
{
    if (!need_sd("edit")) return 1;
    return editor_open(argc - 1, argv + 1);
}

/* ------------------------------------------------------------------ */
/* compiler and runner                                                 */
/* ------------------------------------------------------------------ */

static int *err_line_out;
static char err_file_want[VFS_PATH];
char shell_err_file[VFS_PATH];

/* ------------------------------------------------------------------ */
/* CPU clock: cpu_mhz_run while compiling and running, cpu_mhz_idle    */
/* at the prompt and in the editor (0 = cpu_mhz, then the default)     */
/* ------------------------------------------------------------------ */

static int clock_now = -1;

static void clock_set(int mhz)
{
    if (mhz == clock_now) return;
    if (hal_safe_mode()) return;
    if (hal_set_cpu_mhz(mhz)) clock_now = mhz;
}

void clock_run(void)
{
    clock_set(g_conf.cpu_mhz_run ? g_conf.cpu_mhz_run : g_conf.cpu_mhz);
}

void clock_idle(void)
{
    clock_set(g_conf.cpu_mhz_idle ? g_conf.cpu_mhz_idle : g_conf.cpu_mhz);
}

/* run a program: faster clock, statistics, exit code */
static int run_program(const char *path, int argc, char **argv)
{
    int rc;
    clock_run();
    rc = vm_run_file(path, argc, argv);
    clock_idle();
    con_default_color();
    con_cursor(g_conf.cursor);
    if (g_conf.show_stats) {
        con_set_fg(8);
        if (con_x() != 0) con_putc('\n');
        con_printf("[%u instr, %u ms, heap %u KB, stack %u B, exit %d]\n", (unsigned)vm_last.instructions,
                   (unsigned)vm_last.ms, (unsigned)((vm_last.heap_peak + 1023) / 1024), (unsigned)vm_last.stack_peak, rc);
        con_default_color();
    } else if (rc != 0) con_printf("[exit code %d]\n", rc);
    return rc;
}

static int cc_hook(const char *s, int n)
{
    /* "file:LINE:COL: error: ..." - remember the first error and its file */
    if (err_line_out && *err_line_out == 0) {
        const char *c1 = memchr(s, ':', (size_t)n);
        if (c1 && strstr(s, "error")) {
            int ln = atoi(c1 + 1);
            if (ln > 0) {
                int fl = (int)(c1 - s);
                *err_line_out = ln;
                if (fl >= VFS_PATH) fl = VFS_PATH - 1;
                memcpy(shell_err_file, s, (size_t)fl);
                shell_err_file[fl] = 0;
            }
        }
    }
    if (strstr(s, "error")) con_set_fg(9);
    else if (strstr(s, "warning")) con_set_fg(11);
    con_write(s, n);
    con_default_color();
    return n;
}

int shell_compile(const char *src, char *out, int *errline)
{
    char in[VFS_PATH];
    char *argv[5];
    int rc;
    size_t l;
    uint32_t t0;
    if (!vfs_resolve(src, in)) return 1;
    l = strlen(in);
    strcpy(out, in);
    if (l > 2 && (!strcasecmp(in + l - 2, ".c"))) out[l - 2] = 0;
    if (strlen(out) + 5 > VFS_PATH) return 1;
    strcat(out, ".cbc");
    argv[0] = "cc";
    argv[1] = in;
    argv[2] = "-o";
    argv[3] = out;
    argv[4] = 0;
    if (errline) *errline = 0;
    shell_err_file[0] = 0;
    err_line_out = errline;
    snprintf(err_file_want, sizeof err_file_want, "%s", in);
    cc_out_hook = cc_hook;
    clock_run();
    t0 = hal_millis();
    rc = cc_compile(4, argv);
    clock_idle();
    cc_out_hook = 0;
    err_line_out = 0;
    if (rc == 0) {
        con_set_fg(8);
        con_printf("ok, %u ms, %u KB\n", (unsigned)(hal_millis() - t0), (unsigned)(cc_peak_bytes() / 1024));
        con_default_color();
    }
    return rc;
}

int shell_run_program(const char *path, const char *name)
{
    char *argv[2];
    int rc;
    argv[0] = (char *)(name ? name : path);
    argv[1] = 0;
    rc = run_program(path, 1, argv);
    return rc;
}

static int c_cc(int argc, char **argv)
{
    char in[VFS_PATH];
    int rc, i;
    uint32_t t0;
    char *av[MAXARGS + 1];
    int has_o = 0;
    if (!need_sd("cc")) return 1;
    if (argc < 2) { con_puts("usage: cc FILE.c [-o OUT.cbc] [-v]\n"); return 1; }
    for (i = 1; i < argc; i++) if (!strcmp(argv[i], "-o")) has_o = 1;
    if (!has_o) {
        char out[VFS_PATH];
        int line;
        return shell_compile(argv[1], out, &line);
    }
    /* explicit options: hand them through */
    av[0] = "cc";
    for (i = 1; i < argc && i < MAXARGS; i++) {
        if (i == 1 || (i > 1 && !strcmp(argv[i - 1], "-o"))) {
            if (!resolve("cc", argv[i], in)) return 1;
            av[i] = strdup(in);
        } else av[i] = strdup(argv[i]);
    }
    av[i] = 0;
    cc_out_hook = cc_hook;
    clock_run();
    t0 = hal_millis();
    rc = cc_compile(i, av);
    clock_idle();
    cc_out_hook = 0;
    while (--i > 0) free(av[i]);
    if (rc == 0) {
        con_set_fg(8);
        con_printf("compiled in %u ms\n", (unsigned)(hal_millis() - t0));
        con_default_color();
    }
    return rc;
}

static int c_run(int argc, char **argv)
{
    char p[VFS_PATH], out[VFS_PATH];
    int rc, line;
    size_t l;
    if (argc < 2) { con_puts("usage: run PROG[.c|.cbc] [ARGS...] | run pf:NAME\n"); return 1; }
    if (strncmp(argv[1], "pf:", 3) == 0) return run_program(argv[1], argc - 1, argv + 1);
    if (!need_sd("run")) return 1;
    if (!resolve("run", argv[1], p)) return 1;
    l = strlen(p);
    if (l > 2 && !strcasecmp(p + l - 2, ".c")) {
        /* compile first if the .cbc is missing or the source is newer */
        if (shell_compile(p, out, &line) != 0) return 1;
        strcpy(p, out);
    } else if (!vfs_stat(p, 0, 0) && l + 4 < VFS_PATH) {
        strcat(p, ".cbc");
    }
    rc = run_program(p, argc - 1, argv + 1);
    return rc;
}

static int c_install(int argc, char **argv)
{
    int i, n = 0, force = argc > 1 && !strcmp(argv[1], "-f");
    if (!need_sd("install")) return 1;
    if (!vfs_stat("/bin", 0, 0)) vfs_mkdir("/bin");   /* programs found by name */
    for (i = 0; i < embedded_count; i++) {
        const EmbeddedFile *f = &embedded_files[i];
        char dir[VFS_PATH];
        char *s;
        if (!force && vfs_stat(f->path, 0, 0)) continue;
        strcpy(dir, f->path);
        for (s = dir + 1; *s; s++) if (*s == '/') { *s = 0; if (!vfs_stat(dir, 0, 0)) vfs_mkdir(dir); *s = '/'; }
        if (vfs_save(f->path, f->data, (int)f->len)) { con_printf("  %s\n", f->path); n++; }
        else err("install", vfs_error(), f->path);
    }
    con_printf("%d file(s) written%s\n", n, n ? "" : " (all present; install -f overwrites)");
    return 0;
}

/* ------------------------------------------------------------------ */
/* the page file                                                       */
/* ------------------------------------------------------------------ */

static int pf_kind_for(const char *name, const uint8_t *d, int n)
{
    int i;
    if (n >= 4 && d[0] == 'C' && d[1] == 'B' && d[2] == 'C' && d[3] == '1') return PF_PROG;
    for (i = 0; i < n && i < 512; i++) if (d[i] == 0 || (d[i] < 32 && d[i] != '\n' && d[i] != '\r' && d[i] != '\t')) return PF_RAW;
    (void)name;
    return PF_TEXT;
}

static void pf_tree_print(void)
{
    /* names may contain '/', shown as folders; listed in sorted order */
    int n = pf_count(), i, j;
    char prev[PF_NAMELEN] = "", last_dir[PF_NAMELEN] = "";
    con_puts("pf:\n");
    for (i = 0; i < n; i++) {
        const char *name, *best = 0, *s, *leaf;
        int len, kind, bl = 0, bk = 0, depth = 0, k, common;
        char sz[16], dir[PF_NAMELEN];
        for (j = 0; j < n; j++) {
            pf_entry(j, &name, &len, &kind, 0, 0);
            if (strcmp(name, prev) > 0 && (!best || strcmp(name, best) < 0)) { best = name; bl = len; bk = kind; }
        }
        if (!best) break;
        snprintf(prev, sizeof prev, "%s", best);
        leaf = strrchr(best, '/');
        leaf = leaf ? leaf + 1 : best;
        k = (int)(leaf - best);
        memcpy(dir, best, (size_t)k);
        dir[k] = 0;
        /* print the folders that differ from the previous entry's */
        common = 0;
        for (j = 0; dir[j] && dir[j] == last_dir[j]; j++) if (dir[j] == '/') common = j + 1;
        if (strcmp(dir, last_dir) != 0) {
            int d = 0;
            for (j = 0; j < common; j++) if (dir[j] == '/') d++;
            for (s = dir + common; *s; ) {
                const char *e = strchr(s, '/');
                for (k = 0; k < d; k++) con_puts("|   ");
                con_set_fg(12);
                con_printf("|-- %.*s/\n", (int)(e - s), s);
                con_default_color();
                d++;
                s = e + 1;
            }
            snprintf(last_dir, sizeof last_dir, "%s", dir);
        }
        for (s = dir; *s; s++) if (*s == '/') depth++;
        for (j = 0; j < depth; j++) con_puts("|   ");
        fmt_size((uint32_t)bl, sz);
        con_printf("|-- %s ", leaf);
        con_set_fg(8);
        con_printf("(%s, %s)\n", pf_kind_name(bk), sz);
        con_default_color();
        if (!more_check()) break;
    }
}

static int c_pf(int argc, char **argv)
{
    const char *sub = argc > 1 ? argv[1] : "list";
    int r;
    if (!pf_available()) { err("pf", "page file not available on this build", 0); return 1; }
    page_rows = 0;
    if (!strcmp(sub, "list") || !strcmp(sub, "ls")) {
        int i, n = pf_count();
        con_printf("%-*s %-7s %7s pages\n", con_cols() >= 64 ? 32 : 16, "name", "kind", "bytes");
        for (i = 0; i < n; i++) {
            const char *name;
            int len, kind, page, pages;
            pf_entry(i, &name, &len, &kind, &page, &pages);
            if (!more_check()) break;
            con_printf("%-*.*s %-7s %7d %d\n", con_cols() >= 64 ? 32 : 16, con_cols() >= 64 ? 32 : 16, name, pf_kind_name(kind), len, pages);
        }
        con_printf("%d object(s), %d KB free of %d KB\n", n, pf_free_pages() * 4, (pf_total_pages() - 1) * 4);
        return 0;
    }
    if (!strcmp(sub, "free") || !strcmp(sub, "df")) {
        int fp = pf_free_pages(), tp = pf_total_pages();
        con_printf("page file: %d KB (%d pages of 4 KB)\n", tp * 4, tp);
        con_printf(" used %4d KB  (%d objects, directory 4 KB)\n", (tp - fp) * 4, pf_count());
        con_printf(" free %4d KB\n", fp * 4);
        return 0;
    }
    if (!strcmp(sub, "set")) {
        char val[256] = "";
        int i;
        if (argc < 3) { con_puts("usage: pf set NAME VALUE...\n"); return 1; }
        for (i = 3; i < argc; i++) {
            if (i > 3) strncat(val, " ", sizeof val - strlen(val) - 1);
            strncat(val, argv[i], sizeof val - strlen(val) - 1);
        }
        r = pf_put(argv[2], val, (int)strlen(val), PF_VAR);
        if (r != PF_OK) { err("pf", pf_strerror(r), argv[2]); return 1; }
        return 0;
    }
    if (!strcmp(sub, "store") || !strcmp(sub, "put")) {
        char *data;
        int n;
        const char *name;
        if (argc < 3) { con_puts("usage: pf store FILE [NAME]\n"); return 1; }
        data = vfs_load(argv[2], &n, 0);
        if (!data) { err("pf", "cannot read", argv[2]); return 1; }
        name = argc > 3 ? argv[3] : vfs_basename(argv[2]);
        r = pf_put(name, data, n, pf_kind_for(name, (uint8_t *)data, n));
        free(data);
        if (r != PF_OK) { err("pf", pf_strerror(r), name); return 1; }
        con_printf("stored %s (%d bytes)\n", name, n);
        return 0;
    }
    if (!strcmp(sub, "get") || !strcmp(sub, "load")) {
        int len, kind;
        const void *p;
        if (argc < 4) { con_puts("usage: pf get NAME FILE\n"); return 1; }
        p = pf_find(argv[2], &len, &kind);
        if (!p) { err("pf", "no such object", argv[2]); return 1; }
        if (!vfs_save(argv[3], p, len)) { err("pf", vfs_error(), argv[3]); return 1; }
        con_printf("wrote %d bytes to %s\n", len, argv[3]);
        return 0;
    }
    if (!strcmp(sub, "view") || !strcmp(sub, "cat") || !strcmp(sub, "get")) {
        int len, kind, i, col = 0;
        const char *p;
        if (argc < 3) { con_puts("usage: pf view NAME\n"); return 1; }
        p = (const char *)pf_find(argv[2], &len, &kind);
        if (!p) { err("pf", "no such object", argv[2]); return 1; }
        if (kind == PF_PROG || kind == PF_RAW) { con_printf("%s is %s (%d bytes) - use pf dump\n", argv[2], pf_kind_name(kind), len); return 0; }
        for (i = 0; i < len; i++) {
            con_putc(p[i]);
            col = p[i] == '\n' ? 0 : col + 1;
            if ((p[i] == '\n' || col >= con_cols()) && !more_check()) break;
            if (col >= con_cols()) col = 0;
        }
        if (len && p[len - 1] != '\n') con_puts("\n");
        return 0;
    }
    if (!strcmp(sub, "dump") || !strcmp(sub, "hex")) {
        int len, kind;
        const void *p;
        if (argc < 3) { con_puts("usage: pf dump NAME\n"); return 1; }
        p = pf_find(argv[2], &len, &kind);
        if (!p) { err("pf", "no such object", argv[2]); return 1; }
        hexdump((const uint8_t *)p, len, 0);
        return 0;
    }
    if (!strcmp(sub, "wipe") || !strcmp(sub, "rm") || !strcmp(sub, "del")) {
        if (argc < 3) { con_puts("usage: pf wipe NAME\n"); return 1; }
        r = pf_delete(argv[2]);
        if (r != PF_OK) { err("pf", pf_strerror(r), argv[2]); return 1; }
        con_printf("wiped %s\n", argv[2]);
        return 0;
    }
    if (!strcmp(sub, "format")) {
        con_printf("Erase the whole %d KB page file? (y/n) ", pf_total_pages() * 4);
        if (!is_yes()) return 1;
        if (!pf_format()) { err("pf", "format failed", 0); return 1; }
        con_puts("page file formatted\n");
        return 0;
    }
    if (!strcmp(sub, "tree")) { pf_tree_print(); return 0; }
    if (!strcmp(sub, "run")) {
        char pn[PF_NAMELEN + 4];
        if (argc < 3) { con_puts("usage: pf run NAME\n"); return 1; }
        snprintf(pn, sizeof pn, "pf:%s", argv[2]);
        return run_program(pn, argc - 2, argv + 2);
    }
    con_puts("usage: pf set|store|get|view|list|dump|free|wipe|format|tree|run\n");
    return 1;
}

/* ------------------------------------------------------------------ */
/* the command table                                                   */
/* ------------------------------------------------------------------ */

static const Cmd cmds[] = {
    {"help", c_help, "help [CMD]", "list commands, or describe one"},
    {"battery", c_battery, "battery", "battery level and charging status"},
    {"shutdown", c_shutdown, "shutdown [N]", "power off (after N seconds)"},
    {"reset", c_reset, "reset [N]", "restart the device (after N seconds)"},
    {"restart", c_reset, "restart [N]", "same as reset"},
    {"width", c_width, "width [40|64]", "switch between 40 and 64 column modes"},
    {"cls", c_cls, "cls", "clear the screen"},
    {"clear", c_cls, "clear", "clear the screen"},
    {"color", c_color, "color FG [BG] | color list | color theme NAME", "default colours (saved)"},
    {"theme", c_color, "theme NAME", "colour theme: dos classic turbo green amber paper matrix night"},
    {"font", c_font, "font [8x10|5x10]", "switch font (8x10 = 40 cols, 5x10 = 64 cols)"},
    {"cursor", c_cursor, "cursor [on|off|block|underline]", "show/hide the cursor, or its shape"},
    {"beep", c_beep, "beep [HZ] [MS]", "play a tone"},
    {"mute", c_mute, "mute [on|off]", "mute / unmute system sounds"},
    {"df", c_df, "df", "free and used space on the SD card"},
    {"tree", c_tree, "tree [DIR]", "directory tree with sizes and SD usage"},
    {"sd", c_sd, "sd", "SD card information"},
    {"sdcard", c_sd, "sdcard", "SD card information"},
    {"mount", c_mount, "mount", "mount the SD card"},
    {"unmount", c_unmount, "unmount", "unmount the SD card (safe removal)"},
    {"umount", c_unmount, "umount", "same as unmount"},
    {"sync", c_sync, "sync", "flush file system buffers"},
    {"mem", c_mem, "mem", "free SRAM and the largest block"},
    {"free", c_mem, "free", "same as mem"},
    {"uptime", c_uptime, "uptime", "time since boot"},
    {"version", c_version, "version", "environment and compiler version"},
    {"ver", c_version, "ver", "same as version"},
    {"temp", c_temp, "temp", "RP2350 internal temperature"},
    {"clock", c_clock, "clock [MHZ] | clock idle MHZ | clock run MHZ", "show or set the CPU clock (0 = default)"},
    {"backlight", c_backlight, "backlight [0-255] [KBD]", "screen and keyboard backlight"},
    {"exit", c_exit, "exit", "leave CalcIDE (back to the PicoCalc launcher)"},
    {"bye", c_bye, "bye", "say goodbye and power off"},
    {"bootsel", c_bootsel, "bootsel", "reboot into the USB bootloader to flash new firmware"},
    {"history", c_history, "history [-c]", "command history (!N or !! repeats)"},
    {"play", c_play, "play NOTES | play FILE.song", "play a tune: up to 4 channels split by |, drums K SN HH OH CP TH TL CY"},
    {"wav", c_wav, "wav FILE", "play a PCM .wav file"},
    {"gpio", c_gpio, "gpio PIN [in|out|up|down|0|1]", "read or set a free GPIO pin"},
    {"selftest", c_selftest, "selftest [quick] [flash]", "check memory, SD, flash, compiler, sensors, screen, keys, sound"},
    {"alias", c_alias, "alias [NAME=CMD]", "list or define aliases (saved)"},
    {"unalias", c_unalias, "unalias NAME", "remove an alias"},
    {"set", c_set, "set [KEY VALUE] | set save|reload|defaults", "CalcIDE.conf settings"},
    {"echo", c_echo, "echo TEXT", "print text"},
    {"sleep", c_sleep, "sleep [S]", "wait S seconds"},
    {"ls", c_ls, "ls [-l] [PATH]", "list a directory"},
    {"dir", c_ls, "dir [PATH]", "long directory listing"},
    {"cd", c_cd, "cd [DIR]", "change directory"},
    {"pwd", c_pwd, "pwd", "print the current directory"},
    {"mkdir", c_mkdir, "mkdir [-p] DIR", "make a directory"},
    {"md", c_mkdir, "md DIR", "same as mkdir"},
    {"rmdir", c_rmdir, "rmdir DIR", "remove an empty directory"},
    {"rd", c_rmdir, "rd DIR", "same as rmdir"},
    {"rm", c_rm, "rm [-r] PATH", "remove files (and directories with -r)"},
    {"del", c_rm, "del FILE", "same as rm"},
    {"cp", c_cp, "cp FROM TO", "copy a file"},
    {"copy", c_cp, "copy FROM TO", "same as cp"},
    {"mv", c_mv, "mv FROM TO", "move or rename"},
    {"ren", c_mv, "ren FROM TO", "same as mv"},
    {"cat", c_cat, "cat FILE", "print a file"},
    {"type", c_cat, "type FILE", "same as cat"},
    {"more", c_cat, "more FILE", "print a file a screen at a time"},
    {"touch", c_touch, "touch FILE", "create an empty file"},
    {"hex", c_hex, "hex FILE", "hex dump of a file"},
    {"edit", c_edit, "edit [FILE...]", "open the editor (several files open as tabs)"},
    {"ed", c_edit, "ed [FILE...]", "same as edit"},
    {"cc", c_cc, "cc FILE.c [-o OUT] [-v]", "compile C to bytecode (.cbc)"},
    {"run", c_run, "run PROG [ARGS]", "run a .cbc (compiles a .c first)"},
    {"install", c_install, "install [-f]", "write examples and compiler source to the SD card"},
    {"pf", c_pf, "pf set|store|get|view|list|dump|free|wipe|format|tree|run", "the flash page file (768 KB)"},
    {0, 0, 0, 0}
};

static int c_help(int argc, char **argv)
{
    int i, col = 0, w = con_cols() >= 64 ? 16 : 10;
    if (argc > 1) {
        for (i = 0; cmds[i].name; i++) {
            if (!strcmp(cmds[i].name, argv[1])) {
                con_set_fg(14);
                con_printf("%s\n", cmds[i].usage);
                con_default_color();
                con_printf("  %s\n", cmds[i].help);
                return 0;
            }
        }
        err("help", "no such command", argv[1]);
        return 1;
    }
    con_set_fg(14);
    con_puts("CalcIDE commands (help CMD for details):\n");
    con_default_color();
    for (i = 0; cmds[i].name; i++) {
        con_printf("%-*s", w, cmds[i].name);
        col += w;
        if (col + w > con_cols()) { con_puts("\n"); col = 0; }
    }
    if (col) con_puts("\n");
    con_set_fg(8);
    con_puts("Keys: Up/Down history, Tab completes, Ctrl+C cancels/breaks\n");
    con_default_color();
    return 0;
}

/* ------------------------------------------------------------------ */
/* parsing and dispatch                                                */
/* ------------------------------------------------------------------ */

static int split(char *s, char **argv)
{
    int n = 0;
    while (*s && n < MAXARGS) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s) break;
        if (*s == '"') {
            argv[n++] = ++s;
            while (*s && *s != '"') s++;
        } else {
            argv[n++] = s;
            while (*s && *s != ' ' && *s != '\t') s++;
        }
        if (*s) *s++ = 0;
    }
    argv[n] = 0;
    return n;
}

static int dispatch(char *line, int depth)
{
    char *argv[MAXARGS + 1];
    char buf[256];
    int argc, i;
    const char *a;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line || *line == '#' || *line == ';') return 0;
    snprintf(buf, sizeof buf, "%s", line);
    argc = split(buf, argv);
    if (!argc) return 0;
    /* aliases, at most a few levels deep */
    if (depth < 4 && (a = alias_find(argv[0])) != 0) {
        char exp[320];
        const char *rest = line + strlen(argv[0]);
        snprintf(exp, sizeof exp, "%s%s", a, rest);
        return dispatch(exp, depth + 1);
    }
    for (i = 0; cmds[i].name; i++)
        if (!strcasecmp(cmds[i].name, argv[0])) return cmds[i].fn(argc, argv);
    /* a program: NAME, NAME.cbc here or in /bin */
    if (vfs_mounted()) {
        char p[VFS_PATH], q[VFS_PATH + 8];
        const char *tries[3];
        int t;
        snprintf(q, sizeof q, "%s.cbc", argv[0]);
        tries[0] = argv[0];
        tries[1] = q;
        tries[2] = 0;
        for (t = 0; tries[t]; t++) {
            int isdir = 0;
            size_t l = strlen(tries[t]);
            if (l < 4 || strcasecmp(tries[t] + l - 4, ".cbc")) continue;
            if (vfs_resolve(tries[t], p) && vfs_stat(p, &isdir, 0) && !isdir) return run_program(p, argc, argv);
            snprintf(p, sizeof p, "/bin/%s", vfs_basename(tries[t]));
            if (vfs_stat(p, &isdir, 0) && !isdir) return run_program(p, argc, argv);
        }
    }
    color_msg(9, "unknown command: %s (try 'help')\n", argv[0]);
    return 1;
}

int shell_exec(const char *line)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", line);
    last_rc = dispatch(buf, 0);
    return last_rc;
}

/* ------------------------------------------------------------------ */
/* tab completion                                                      */
/* ------------------------------------------------------------------ */

static int complete(const char *line, int pos, char *out, int cap)
{
    int ws = pos, first, i, nmatch = 0, common = -1;
    char word[VFS_PATH], match[VFS_PATH] = "", dir[VFS_PATH], pre[VFS_PATH];
    const char *base = "";
    while (ws > 0 && line[ws - 1] != ' ') ws--;
    first = 1;
    for (i = 0; i < ws; i++) if (line[i] != ' ') first = 0;
    if (pos - ws >= (int)sizeof word) return -1;
    memcpy(word, line + ws, (size_t)(pos - ws));
    word[pos - ws] = 0;
    if (first && !strchr(word, '/')) {
        for (i = 0; cmds[i].name; i++) {
            if (strncmp(cmds[i].name, word, strlen(word))) continue;
            if (!nmatch) { strcpy(match, cmds[i].name); common = (int)strlen(match); }
            else { int k = 0; while (k < common && match[k] == cmds[i].name[k]) k++; common = k; }
            nmatch++;
        }
        if (nmatch == 1) strcat(match, " ");
        else if (nmatch > 1) match[common] = 0;
    } else if (vfs_mounted()) {
        void *d;
        vdirent e;
        const char *sl = strrchr(word, '/');
        if (sl) {
            int dl = (int)(sl - word) + 1;
            memcpy(pre, word, (size_t)dl);
            pre[dl] = 0;
            base = sl + 1;
        } else { pre[0] = 0; base = word; }
        if (!vfs_resolve(pre[0] ? pre : ".", dir)) return -1;
        d = vfs_opendir(dir);
        if (!d) return -1;
        while (vfs_readdir(d, &e)) {
            char cand[VFS_PATH];
            if (strncasecmp(e.name, base, strlen(base))) continue;
            snprintf(cand, sizeof cand, "%s%s%s", pre, e.name, e.isdir ? "/" : "");
            if (!nmatch) { strcpy(match, cand); common = (int)strlen(match); }
            else { int k = 0; while (k < common && tolower((unsigned char)match[k]) == tolower((unsigned char)cand[k])) k++; common = k; }
            nmatch++;
        }
        vfs_closedir(d);
        if (nmatch == 1 && match[strlen(match) - 1] != '/') strcat(match, " ");
        else if (nmatch > 1) match[common] = 0;
    }
    if (!nmatch) { con_bell(); return -1; }
    if (nmatch > 1 && (int)strlen(match) <= (int)strlen(word)) {
        /* nothing more in common: list the candidates */
        con_puts("\n");
        if (first && !strchr(word, '/')) {
            for (i = 0; cmds[i].name; i++) if (!strncmp(cmds[i].name, word, strlen(word))) con_printf("%s  ", cmds[i].name);
        } else {
            void *d = vfs_opendir(dir);
            vdirent e;
            if (d) {
                while (vfs_readdir(d, &e)) if (!strncasecmp(e.name, base, strlen(base))) con_printf("%s%s  ", e.name, e.isdir ? "/" : "");
                vfs_closedir(d);
            }
        }
        con_puts("\n");
        snprintf(out, (size_t)cap, "%s", line);
        return -2;
    }
    if ((int)(ws + strlen(match) + strlen(line + pos)) >= cap) return -1;
    memcpy(out, line, (size_t)ws);
    strcpy(out + ws, match);
    i = (int)strlen(out);
    strcat(out, line + pos);
    return i;
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */
/* ------------------------------------------------------------------ */

static void make_prompt(char *out, int cap)
{
    const char *p = g_conf.prompt;
    int n = 0;
    while (*p && n < cap - 1) {
        if (p[0] == '$' && p[1] == 'p') { n += snprintf(out + n, (size_t)(cap - n), "%s", vfs_cwd); p += 2; }
        else if (p[0] == '$' && p[1] == 'g') { out[n++] = '>'; p += 2; }
        else out[n++] = *p++;
        if (n >= cap) n = cap - 1;
    }
    out[n] = 0;
}

/* history survives a restart: /calcide/history.txt, at most 16 KB */
#define HIST_FILE "/calcide/history.txt"
#define HIST_MAX_BYTES (16 * 1024)

static void history_save(void)
{
    char *buf;
    int i, first = 0, total = 0;
    if (!g_conf.history_save || !vfs_mounted()) return;
    for (i = 0; i < hist.count; i++) total += (int)strlen(hist.lines[i]) + 1;
    while (total > HIST_MAX_BYTES && first < hist.count) total -= (int)strlen(hist.lines[first++]) + 1;
    buf = (char *)malloc((size_t)total + 1);
    if (!buf) return;
    total = 0;
    for (i = first; i < hist.count; i++) {
        size_t l = strlen(hist.lines[i]);
        memcpy(buf + total, hist.lines[i], l);
        total += (int)l;
        buf[total++] = '\n';
    }
    if (!vfs_stat("/calcide", 0, 0)) vfs_mkdir("/calcide");
    vfs_save(HIST_FILE, buf, total);
    free(buf);
}

static void history_load(void)
{
    int len;
    char *txt, *line, *nl;
    if (!g_conf.history_save || !vfs_mounted()) return;
    txt = vfs_load(HIST_FILE, &len, 0);
    if (!txt) return;
    if (len > HIST_MAX_BYTES) {
        /* keep the newest 16 KB, starting at a line boundary */
        line = txt + len - HIST_MAX_BYTES;
        while (*line && *line != '\n') line++;
    } else line = txt;
    for (; line && *line; line = nl) {
        nl = strchr(line, '\n');
        if (nl) *nl++ = 0;
        if (*line && *line != '\r') le_history_add(&hist, line);
    }
    free(txt);
}

void shell_init(void)
{
    hist.max = g_conf.history;
    le_complete = complete;
    history_load();
}

extern int hal_eof(void) __attribute__((weak));

void shell_main(void)
{
    char line[200], prompt[96];
    for (;;) {
        int n;
        make_prompt(prompt, sizeof prompt);
        con_set_fg(14);
        con_puts(prompt);
        con_default_color();
        n = le_readline(line, sizeof line, "", &hist);
        if (n < 0) {
            if (hal_eof && hal_eof()) return;
            continue;
        }
        if (line[0] == '!') {
            int idx = -1;
            if (line[1] == '!') idx = hist.count - 1;
            else if (isdigit((unsigned char)line[1])) idx = atoi(line + 1) - 1;
            if (idx < 0 || idx >= hist.count) { color_msg(9, "%s: no such history entry\n", line); continue; }
            snprintf(line, sizeof line, "%s", hist.lines[idx]);
            con_printf("%s\n", line);
        }
        le_history_add(&hist, line);
        history_save();
        shell_exec(line);
        con_flush();
    }
}
