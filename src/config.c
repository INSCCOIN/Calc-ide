/* config.c - /CalcIDE.conf: "key = value" lines, '#' or ';' comments.
 *
 * The file is read at boot and rewritten whenever a setting changes from
 * the shell (color, width, font, cursor, mute, alias, set ...). Unknown keys
 * are ignored so a newer file still boots an older firmware.
 */
#include "config.h"
#include "vfs.h"
#include "console.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <stdarg.h>
#include <strings.h>

Config g_conf;

static const char *cnames[16] = {
    "black", "red", "green", "brown", "blue", "magenta", "cyan", "lightgray",
    "darkgray", "lightred", "lightgreen", "yellow", "lightblue", "lightmagenta", "lightcyan", "white"
};

const char *conf_color_name(int c) { return (c >= 0 && c < 16) ? cnames[c] : "?"; }

int conf_color(const char *s)
{
    int i;
    char *e;
    long v;
    if (!s || !*s) return -1;
    for (i = 0; i < 16; i++) if (strcasecmp(s, cnames[i]) == 0) return i;
    if (strcasecmp(s, "gray") == 0 || strcasecmp(s, "grey") == 0) return 7;
    if (strcasecmp(s, "yellowdark") == 0 || strcasecmp(s, "darkyellow") == 0) return 3;
    if (strcasecmp(s, "darkgrey") == 0) return 8;
    v = strtol(s, &e, 0);
    if (*e == 0 && v >= 0 && v < 16) return (int)v;
    return -1;
}

void conf_defaults(void)
{
    memset(&g_conf, 0, sizeof g_conf);
    g_conf.width = 40;
    g_conf.font = 0;
    g_conf.fg = 7;  g_conf.bg = 0;           /* DOS: light gray on black */
    g_conf.ed_fg = 15; g_conf.ed_bg = 4;     /* white on blue, like Turbo C */
    g_conf.ed_kw = 11;                       /* yellow */
    g_conf.ed_type = 14;                     /* light cyan */
    g_conf.ed_str = 10;                      /* light green */
    g_conf.ed_com = 7;                       /* light gray */
    g_conf.ed_num = 13;                      /* light magenta */
    g_conf.ed_pre = 9;                       /* light red */
    g_conf.ed_lnum = 6;                      /* cyan */
    g_conf.ed_status_fg = 0;
    g_conf.ed_status_bg = 6;
    g_conf.ed_sel = 3;
    g_conf.syntax = 1;
    g_conf.line_numbers = 1;
    g_conf.tabsize = 4;
    g_conf.autoindent = 1;
    g_conf.cursor = 1;
    g_conf.cursor_style = 0;
    g_conf.mute = 0;
    g_conf.volume = 100;
    g_conf.backlight = 200;
    g_conf.kbd_light = 64;
    g_conf.cpu_mhz = 0;
    g_conf.vm_mem_kb = 0;                    /* 0 = auto: all free memory */
    g_conf.vm_stack_kb = 32;
    g_conf.history = 32;
    g_conf.click = 0;
    g_conf.lcd_bits = 16;
    g_conf.lcd_madctl = 0x48;
    g_conf.lcd_invert = 1;
    g_conf.bl_swap = 0;
    strcpy(g_conf.prompt, "$p> ");
    strcpy(g_conf.home, "/");
}

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

typedef struct { const char *key; int *ip; int lo, hi; int color; } IntKey;

static IntKey ikeys[] = {
    {"width", &g_conf.width, 40, 64, 0},
    {"font", &g_conf.font, 0, 1, 0},
    {"fg", &g_conf.fg, 0, 15, 1},
    {"bg", &g_conf.bg, 0, 15, 1},
    {"editor_fg", &g_conf.ed_fg, 0, 15, 1},
    {"editor_bg", &g_conf.ed_bg, 0, 15, 1},
    {"keyword_color", &g_conf.ed_kw, 0, 15, 1},
    {"type_color", &g_conf.ed_type, 0, 15, 1},
    {"string_color", &g_conf.ed_str, 0, 15, 1},
    {"comment_color", &g_conf.ed_com, 0, 15, 1},
    {"number_color", &g_conf.ed_num, 0, 15, 1},
    {"preproc_color", &g_conf.ed_pre, 0, 15, 1},
    {"linenum_color", &g_conf.ed_lnum, 0, 15, 1},
    {"status_fg", &g_conf.ed_status_fg, 0, 15, 1},
    {"status_bg", &g_conf.ed_status_bg, 0, 15, 1},
    {"match_color", &g_conf.ed_sel, 0, 15, 1},
    {"syntax", &g_conf.syntax, 0, 1, 0},
    {"line_numbers", &g_conf.line_numbers, 0, 1, 0},
    {"tabsize", &g_conf.tabsize, 1, 8, 0},
    {"autoindent", &g_conf.autoindent, 0, 1, 0},
    {"cursor", &g_conf.cursor, 0, 1, 0},
    {"cursor_style", &g_conf.cursor_style, 0, 1, 0},
    {"mute", &g_conf.mute, 0, 1, 0},
    {"volume", &g_conf.volume, 0, 100, 0},
    {"backlight", &g_conf.backlight, 0, 255, 0},
    {"kbd_backlight", &g_conf.kbd_light, 0, 255, 0},
    {"cpu_mhz", &g_conf.cpu_mhz, 0, 300, 0},
    {"vm_mem_kb", &g_conf.vm_mem_kb, 0, 480, 0},
    {"vm_stack_kb", &g_conf.vm_stack_kb, 4, 128, 0},
    {"history", &g_conf.history, 4, 64, 0},
    {"key_click", &g_conf.click, 0, 1, 0},
    {"lcd_bits", &g_conf.lcd_bits, 16, 18, 0},
    {"lcd_madctl", &g_conf.lcd_madctl, 0, 255, 0},
    {"lcd_invert", &g_conf.lcd_invert, 0, 1, 0},
    {"bl_swap", &g_conf.bl_swap, 0, 1, 0},
    {0, 0, 0, 0, 0}
};

static int parse_bool(const char *v, int *out)
{
    if (!strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true")) { *out = 1; return 1; }
    if (!strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false")) { *out = 0; return 1; }
    return 0;
}

int conf_set(const char *key, const char *val)
{
    int i, v;
    char *e;
    if (strncasecmp(key, "alias.", 6) == 0) return alias_set(key + 6, val) >= 0;
    if (!strcasecmp(key, "prompt")) { snprintf(g_conf.prompt, sizeof g_conf.prompt, "%s", val); return 1; }
    if (!strcasecmp(key, "autoexec")) { snprintf(g_conf.autoexec, sizeof g_conf.autoexec, "%s", val); return 1; }
    if (!strcasecmp(key, "home")) { snprintf(g_conf.home, sizeof g_conf.home, "%s", val); return 1; }
    for (i = 0; ikeys[i].key; i++) {
        if (strcasecmp(key, ikeys[i].key) != 0) continue;
        if (ikeys[i].color && (v = conf_color(val)) >= 0) { *ikeys[i].ip = v; return 1; }
        if (parse_bool(val, &v)) { *ikeys[i].ip = clamp(v, ikeys[i].lo, ikeys[i].hi); return 1; }
        v = (int)strtol(val, &e, 0);
        if (e == val) return -1;
        if (ikeys[i].ip == &g_conf.width) v = (v >= 64) ? 64 : 40;
        *ikeys[i].ip = clamp(v, ikeys[i].lo, ikeys[i].hi);
        return 1;
    }
    return 0;
}

static char *trim(char *s)
{
    char *e;
    while (isspace((unsigned char)*s)) s++;
    e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

int conf_load(void)
{
    int len;
    char *txt = vfs_load(CONF_PATH, &len, 0);
    char *line, *next;
    if (!txt) return 0;
    for (line = txt; line && *line; line = next) {
        char *eq, *k, *v;
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        line = trim(line);
        if (!*line || *line == '#' || *line == ';' || *line == '[') continue;
        eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        k = trim(line);
        v = trim(eq + 1);
        if (*v == '"' && v[strlen(v) - 1] == '"' && strlen(v) >= 2) { v[strlen(v) - 1] = 0; v++; }
        conf_set(k, v);
    }
    free(txt);
    g_conf.font = g_conf.width == 64 ? 1 : 0;
    return 1;
}

static void emit(char **p, char *end, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(*p, (size_t)(end - *p), fmt, ap);
    va_end(ap);
    if (n > 0) *p += (n < end - *p) ? n : (end - *p - 1);
}

int conf_save(void)
{
    char *buf, *p, *end;
    int i, ok;
    if (!vfs_mounted()) return 0;
    buf = (char *)malloc(6144);
    if (!buf) return 0;
    p = buf;
    end = buf + 6144;
    emit(&p, end, "# CalcIDE.conf - CalcIDE system settings\n");
    emit(&p, end, "# Written by CalcIDE; edit freely. Colours: 0-15 or names\n");
    emit(&p, end, "# (black red green brown blue magenta cyan lightgray darkgray\n");
    emit(&p, end, "#  lightred lightgreen yellow lightblue lightmagenta lightcyan white)\n\n");
    for (i = 0; ikeys[i].key; i++) {
        if (ikeys[i].color) emit(&p, end, "%s = %s\n", ikeys[i].key, conf_color_name(*ikeys[i].ip));
        else emit(&p, end, "%s = %d\n", ikeys[i].key, *ikeys[i].ip);
    }
    emit(&p, end, "prompt = \"%s\"\n", g_conf.prompt);
    emit(&p, end, "home = %s\n", g_conf.home);
    emit(&p, end, "autoexec = %s\n", g_conf.autoexec);
    for (i = 0; i < CONF_MAX_ALIAS; i++)
        if (g_conf.alias_name[i][0]) emit(&p, end, "alias.%s = %s\n", g_conf.alias_name[i], g_conf.alias_cmd[i]);
    ok = vfs_save(CONF_PATH, buf, (int)(p - buf));
    free(buf);
    return ok;
}

void conf_print(void)
{
    int i;
    for (i = 0; ikeys[i].key; i++) {
        if (ikeys[i].color) con_printf("%-14s %s\n", ikeys[i].key, conf_color_name(*ikeys[i].ip));
        else con_printf("%-14s %d\n", ikeys[i].key, *ikeys[i].ip);
    }
    con_printf("%-14s \"%s\"\n", "prompt", g_conf.prompt);
    con_printf("%-14s %s\n", "home", g_conf.home);
    con_printf("%-14s %s\n", "autoexec", g_conf.autoexec);
}

const char *alias_find(const char *name)
{
    int i;
    for (i = 0; i < CONF_MAX_ALIAS; i++)
        if (g_conf.alias_name[i][0] && strcasecmp(g_conf.alias_name[i], name) == 0) return g_conf.alias_cmd[i];
    return 0;
}

int alias_set(const char *name, const char *cmd)
{
    int i, fr = -1;
    if (!name[0] || strlen(name) >= sizeof g_conf.alias_name[0]) return -1;
    for (i = 0; i < CONF_MAX_ALIAS; i++) {
        if (g_conf.alias_name[i][0] && strcasecmp(g_conf.alias_name[i], name) == 0) {
            if (!cmd || !cmd[0]) { g_conf.alias_name[i][0] = 0; return 0; }
            snprintf(g_conf.alias_cmd[i], sizeof g_conf.alias_cmd[i], "%s", cmd);
            return i;
        }
        if (!g_conf.alias_name[i][0] && fr < 0) fr = i;
    }
    if (!cmd || !cmd[0]) return 0;
    if (fr < 0) return -1;
    snprintf(g_conf.alias_name[fr], sizeof g_conf.alias_name[fr], "%s", name);
    snprintf(g_conf.alias_cmd[fr], sizeof g_conf.alias_cmd[fr], "%s", cmd);
    return fr;
}
