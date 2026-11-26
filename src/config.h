/* config.h - system settings, kept in /CalcIDE.conf on the SD card. */
#ifndef CONFIG_H
#define CONFIG_H

#define CONF_PATH "/CalcIDE.conf"
#define CONF_MAX_ALIAS 24

typedef struct {
    int width;          /* 40 or 64 */
    int font;           /* 0 = 8x10, 1 = 5x10 (follows width) */
    int fg, bg;         /* shell colours */
    int ed_fg, ed_bg;   /* editor text */
    int ed_kw, ed_type, ed_str, ed_com, ed_num, ed_pre;  /* syntax colours */
    int ed_lnum;        /* line number gutter */
    int ed_status_fg, ed_status_bg;
    int ed_sel;         /* search match highlight bg */
    int syntax;         /* syntax colouring on/off */
    int line_numbers;
    int tabsize;
    int autoindent;
    int cursor;         /* show cursor */
    int cursor_style;   /* 0 underline, 1 block */
    int mute;
    int volume;         /* not all hardware honours it */
    int backlight;      /* 0..255 */
    int kbd_light;      /* 0..255 */
    int cpu_mhz;        /* 0 = leave at default */
    int vm_mem_kb;      /* VM memory for programs */
    int vm_stack_kb;
    int history;        /* lines of shell history */
    int click;          /* key click */
    char prompt[32];    /* prompt format, $p = path */
    char autoexec[96];  /* command run at boot */
    char home[64];      /* start directory */
    char alias_name[CONF_MAX_ALIAS][16];
    char alias_cmd[CONF_MAX_ALIAS][80];
} Config;

extern Config g_conf;

void conf_defaults(void);
int  conf_load(void);           /* 1 if the file was read */
int  conf_save(void);
int  conf_set(const char *key, const char *val);   /* 0 if unknown key */
void conf_print(void);
int  conf_color(const char *name);                 /* name or number, -1 if bad */
const char *conf_color_name(int c);
const char *alias_find(const char *name);
int  alias_set(const char *name, const char *cmd); /* cmd NULL/"" removes */

#endif
