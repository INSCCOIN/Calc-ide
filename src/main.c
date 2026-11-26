/* main.c - CalcIDE boot sequence.
 *
 * Boot order: hardware, SD card, /CalcIDE.conf (created with defaults on
 * first boot), console, banner, the autoexec command, then the shell.
 */
#include "hal.h"
#include "console.h"
#include "config.h"
#include "vfs.h"
#include "shell.h"
#include "pf.h"
#include <string.h>
#include <stdio.h>

static void banner(void)
{
    char title[CON_MAXCOLS + 1];
    const char *t = "CalcIDE " CALCIDE_VERSION " for the PicoCalc";
    int cols = con_cols(), pad = (cols - (int)strlen(t)) / 2;
    snprintf(title, sizeof title, "%*s%s%*s", pad, "", t, cols - pad - (int)strlen(t), "");
    con_set_color(15, 1);
    con_puts(title);
    con_putc('\n');
    con_default_color();
    con_printf("Tiny-C compiler & VM  -  %s, %d MHz\n", hal_chip(), hal_cpu_khz() / 1000);
    con_printf("%u KB free", (unsigned)(hal_ram_free() / 1024));
    if (vfs_mounted()) {
        uint32_t t, f;
        if (vfs_space(&t, &f)) con_printf(", SD %u MB free", (unsigned)(f / 1024));
    } else con_printf(", no SD card");
    if (pf_available()) con_printf(", pf %d KB", pf_free_pages() * 4);
    con_puts("\n");
    con_set_fg(8);
    con_puts("Type 'help' for commands, 'edit FILE.c' to code.\n\n");
    con_default_color();
}

int main(void)
{
    int fresh = 0;
    conf_defaults();
    hal_init();                         /* may read early settings */
    if (vfs_mount()) {
        if (!conf_load()) fresh = 1;
    }
    hal_backlight(g_conf.backlight);
    hal_kbdlight(g_conf.kbd_light);
    if (g_conf.cpu_mhz) hal_set_cpu_mhz(g_conf.cpu_mhz);
    con_init();
    shell_init();
    if (fresh && vfs_mounted()) conf_save();   /* first boot: write the defaults */
    if (vfs_mounted() && g_conf.home[0]) {
        int isdir = 0;
        if (vfs_stat(g_conf.home, &isdir, 0) && isdir) snprintf(vfs_cwd, sizeof vfs_cwd, "%s", g_conf.home);
    }
    banner();
    if (fresh && vfs_mounted()) {
        con_set_fg(11);
        con_puts("First boot: created " CONF_PATH "\nRun 'install' to copy the examples.\n\n");
        con_default_color();
    }
    if (g_conf.autoexec[0]) shell_exec(g_conf.autoexec);
    con_flush();
    shell_main();
    con_flush();
    return 0;
}
