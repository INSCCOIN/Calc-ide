/* selftest.c - the 'selftest' command: check the hardware and the toolchain.
 *
 *   selftest          everything, including the display, keyboard and sound
 *                     checks that ask you what you saw or heard
 *   selftest quick    only the automatic checks
 *   selftest flash    also write and erase a page-file object (flash wear)
 */
#include "selftest.h"
#include "console.h"
#include "config.h"
#include "hal.h"
#include "vfs.h"
#include "pf.h"
#include "vm.h"
#include "cc.h"
#include "sound.h"
#include "shell.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>

static int npass, nfail, nskip;

static void result(const char *name, int r, const char *detail)
{
    /* r: 1 pass, 0 fail, -1 skipped */
    con_printf("%-10s ", name);
    if (r > 0) { con_set_fg(10); con_puts("PASS"); npass++; }
    else if (r == 0) { con_set_fg(9); con_puts("FAIL"); nfail++; }
    else { con_set_fg(11); con_puts("SKIP"); nskip++; }
    con_default_color();
    if (detail && *detail) con_printf("  %s", detail);
    con_puts("\n");
    con_flush();
}

static int ask(const char *q)
{
    int k;
    con_set_fg(14);
    con_printf("%s (y/n) ", q);
    con_default_color();
    k = con_getkey();
    con_printf("%c\n", (k >= 32 && k < 127) ? k : '?');
    return k == 'y' || k == 'Y';
}

static void t_memory(void)
{
    char d[64];
    size_t n = hal_ram_largest();
    uint32_t *p, i, words, bad = 0;
    if (n > 64 * 1024) n = 64 * 1024;          /* enough to exercise the heap */
    p = (uint32_t *)malloc(n);
    if (!p) { result("memory", 0, "malloc failed"); return; }
    words = (uint32_t)(n / 4);
    for (i = 0; i < words; i++) p[i] = i * 2654435761u;
    for (i = 0; i < words; i++) if (p[i] != i * 2654435761u) bad++;
    for (i = 0; i < words; i++) p[i] = ~(i * 2654435761u);
    for (i = 0; i < words; i++) if (p[i] != ~(i * 2654435761u)) bad++;
    free(p);
    snprintf(d, sizeof d, "%u KB tested, %u KB free, largest %u KB", (unsigned)(n / 1024),
             (unsigned)(hal_ram_free() / 1024), (unsigned)(hal_ram_largest() / 1024));
    result("memory", bad == 0, bad ? "pattern mismatch" : d);
}

static void t_sd(void)
{
    char d[64];
    const char *path = "/calcide/selftest.tmp";
    uint8_t *buf, *back;
    int n = 32 * 1024, i, ok = 0, len = 0;
    uint32_t t0, tw, tr;
    if (!vfs_mounted()) { result("sd card", 0, "no card mounted"); return; }
    if (!vfs_stat("/calcide", 0, 0)) vfs_mkdir("/calcide");
    buf = (uint8_t *)malloc((size_t)n);
    if (!buf) { result("sd card", 0, "out of memory"); return; }
    for (i = 0; i < n; i++) buf[i] = (uint8_t)(i * 7 + (i >> 8));
    t0 = hal_millis();
    if (vfs_save(path, buf, n)) {
        tw = hal_millis() - t0;
        t0 = hal_millis();
        back = (uint8_t *)vfs_load(path, &len, 0);
        tr = hal_millis() - t0;
        if (back) {
            ok = len == n && memcmp(buf, back, (size_t)n) == 0;
            free(back);
        }
        vfs_remove(path);
        snprintf(d, sizeof d, "%s %s, write %u KB/s, read %u KB/s", vfs_fstype(), ok ? "32 KB verified" : "data mismatch",
                 (unsigned)(32 * 1000 / (tw ? tw : 1)), (unsigned)(32 * 1000 / (tr ? tr : 1)));
    } else snprintf(d, sizeof d, "write failed: %s", vfs_error());
    free(buf);
    result("sd card", ok, d);
}

static void t_pagefile(int write)
{
    char d[64];
    if (!pf_available()) { result("page file", 0, "not available"); return; }
    if (write) {
        char data[256];
        const void *p;
        int i, len = 0, kind, ok;
        for (i = 0; i < 256; i++) data[i] = (char)(i ^ 0x5A);
        ok = pf_put("selftest/tmp", data, 256, PF_RAW) == PF_OK;
        p = pf_find("selftest/tmp", &len, &kind);
        ok = ok && p && len == 256 && memcmp(p, data, 256) == 0;
        pf_delete("selftest/tmp");
        snprintf(d, sizeof d, "write/read %s, %d KB free", ok ? "ok" : "FAILED", pf_free_pages() * 4);
        result("page file", ok, d);
        return;
    }
    snprintf(d, sizeof d, "%d objects, %d KB free (selftest flash writes)", pf_count(), pf_free_pages() * 4);
    result("page file", 1, d);
}

/* compile a program that checks itself and exits with 42 when all is well */
static const char *probe =
    "#include <stdio.h>\n"
    "struct p { int x; double y; };\n"
    "long long f(int n) { return n < 2 ? 1 : n * f(n - 1); }\n"
    "int sum(int n, ...) { va_list a; int s = 0; va_start(a, n); while (n--) s += va_arg(a, int); va_end(a); return s; }\n"
    "int main(void) {\n"
    "    struct p q = {.x = 6, .y = 0.5};\n"
    "    char buf[32];\n"
    "    int ok = f(20) == 2432902008176640000LL;\n"
    "    ok = ok && q.x * q.y == 3.0 && sum(3, 1, 2, 3) == 6;\n"
    "    sprintf(buf, \"%.2f|%lld\", 1.0 / 3, -5LL);\n"
    "    ok = ok && strcmp(buf, \"0.33|-5\") == 0;\n"
    "    return ok ? 42 : 1;\n"
    "}\n";

static int quiet(const char *s, int n) { (void)s; return n; }

static void t_compiler(void)
{
    char d[64];
    const char *src = "/calcide/selftest.c", *out = "/calcide/selftest.cbc";
    char *argv[5];
    int rc;
    uint32_t t0;
    if (!vfs_mounted()) { result("compiler", -1, "needs the SD card"); return; }
    if (!vfs_stat("/calcide", 0, 0)) vfs_mkdir("/calcide");
    if (!vfs_save(src, probe, (int)strlen(probe))) { result("compiler", 0, "cannot write test file"); return; }
    argv[0] = "cc"; argv[1] = (char *)src; argv[2] = "-o"; argv[3] = (char *)out; argv[4] = 0;
    cc_out_hook = quiet;
    t0 = hal_millis();
    rc = cc_compile(4, argv);
    cc_out_hook = 0;
    if (rc != 0) { result("compiler", 0, "the test program did not compile"); vfs_remove(src); return; }
    snprintf(d, sizeof d, "compiled in %u ms", (unsigned)(hal_millis() - t0));
    result("compiler", 1, d);
    {
        char *av[2];
        av[0] = (char *)out;
        av[1] = 0;
        rc = vm_run_file(out, 1, av);
    }
    snprintf(d, sizeof d, "%u instructions in %u ms", (unsigned)vm_last.instructions, (unsigned)vm_last.ms);
    result("vm", rc == 42, rc == 42 ? d : "wrong result from the test program");
    vfs_remove(src);
    vfs_remove(out);
}

static void t_sensors(void)
{
    char d[64];
    int ch = 0, b = hal_battery(&ch);
    float t = hal_temp_c();
    if (b < 0) result("battery", 0, "keyboard controller did not answer");
    else {
        snprintf(d, sizeof d, "%d%%%s", b, ch ? ", charging" : "");
        result("battery", b <= 100, d);
    }
    snprintf(d, sizeof d, "%.1f C", t);
    result("temp", t > -20 && t < 95, d);
    snprintf(d, sizeof d, "%s at %d MHz", hal_chip(), hal_cpu_khz() / 1000);
    result("clock", hal_cpu_khz() > 10000, d);
}

static void t_display(void)
{
    int cols = con_cols(), c, x, y = con_y();
    con_puts("\n\n\n\n");
    y = con_y() - 4;
    if (y < 0) y = 0;
    for (c = 0; c < 16; c++)
        for (x = c * cols / 16; x < (c + 1) * cols / 16; x++) {
            con_put_at(x, y, 0xDB, ATTR(c, 0));
            con_put_at(x, y + 1, 0xB1, ATTR(c, 0));
        }
    con_text_at(0, y + 2, "\xC9\xCD\xCD\xCB\xCD\xCD\xBB box \xDA\xC4\xC2\xC4\xBF", ATTR(15, 0), cols);
    con_flush();
    result("display", ask("16 colour bars, then double and single box lines?"), "");
}

static void t_keyboard(void)
{
    uint32_t end = hal_millis() + 10000;
    int k = -1;
    char d[48];
    con_set_fg(14);
    con_puts("Press any key (10 s)... ");
    con_default_color();
    con_flush();
    while ((int32_t)(end - hal_millis()) > 0) {
        k = con_inkey();
        if (k >= 0) break;
        hal_sleep_ms(5);
    }
    con_puts("\n");
    if (k < 0) { result("keyboard", 0, "no key within 10 s"); return; }
    if (k >= 32 && k < 127) snprintf(d, sizeof d, "got '%c' (code %d)", k, k);
    else snprintf(d, sizeof d, "got code 0x%02X", k);
    result("keyboard", 1, d);
}

static void t_sound(void)
{
    if (g_conf.mute) { result("sound", -1, "muted ('mute off' to test)"); return; }
    con_puts("Playing three rising notes...\n");
    con_flush();
    play_notes("T180 C5/8 E5/8 G5/4");
    result("sound", ask("Did you hear three rising notes?"), "");
}

int selftest_run(int argc, char **argv)
{
    int quick = 0, flash = 0, i;
    for (i = 1; i < argc; i++) {
        if (!strcasecmp(argv[i], "quick")) quick = 1;
        else if (!strcasecmp(argv[i], "flash")) flash = 1;
    }
    npass = nfail = nskip = 0;
    con_set_fg(14);
    con_printf("CalcIDE %s self test\n", CALCIDE_VERSION);
    con_default_color();
    t_memory();
    t_sd();
    t_pagefile(flash);
    t_compiler();
    t_sensors();
    if (!quick) {
        t_display();
        t_keyboard();
        t_sound();
    }
    con_printf("%d passed, %d failed, %d skipped\n", npass, nfail, nskip);
    return nfail ? 1 : 0;
}
