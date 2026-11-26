/* xfer.c - copy files to and from a PC over the USB serial port.
 *
 * `xfer` turns the USB serial port into a small file server until the PC
 * sends BYE or a key is pressed on the PicoCalc. tools/calcxfer.py is the
 * PC side. The protocol is lines of text, with raw bytes for file data:
 *
 *   HELLO                 -> OK CalcIDE <version>
 *   LS <dir>              -> D|F <size> <name> lines, then END
 *   GET <path>            -> OK <size>, <size> bytes, then CRC <hex>
 *   PUT <path> <size>     -> OK; then the PC sends 1 KB blocks and waits
 *                            for "A" after each; finally OK <crc hex>
 *   MKDIR <path>, RM <path> -> OK
 *   BYE                   -> OK, and the shell is back
 *
 * Any failure answers ERR <reason>. CRCs are CRC-32 (as in zip).
 */
#include "xfer.h"
#include "hal.h"
#include "vfs.h"
#include "console.h"
#include "shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define BLOCK 1024

static uint32_t crc_table[256];

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, int n)
{
    int i;
    if (!crc_table[1]) {
        uint32_t c, k, j;
        for (k = 0; k < 256; k++) {
            c = k;
            for (j = 0; j < 8; j++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            crc_table[k] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

static void say(const char *fmt, ...)
{
    char b[200];
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof b - 1) n = sizeof b - 1;
    hal_usb_write(b, n);
}

/* a command line; 0 when a key was pressed on the PicoCalc */
static int read_line(char *b, int cap)
{
    int n = 0, c;
    for (;;) {
        c = hal_usb_getc(50);
        if (c < 0) {
            if (hal_kbd_key() >= 0) return 0;
            continue;
        }
        if (c == '\r') continue;
        if (c == '\n') { b[n] = 0; return 1; }
        if (n < cap - 1) b[n++] = (char)c;
    }
}

static int resolve(const char *p, char *full)
{
    if (!p || !*p) { say("ERR missing path\n"); return 0; }
    if (!vfs_resolve(p, full)) { say("ERR bad path\n"); return 0; }
    return 1;
}

static void do_ls(const char *arg)
{
    char full[VFS_PATH];
    void *d;
    vdirent e;
    if (!resolve(arg && *arg ? arg : "/", full)) return;
    if (!(d = vfs_opendir(full))) { say("ERR cannot open %s\n", full); return; }
    while (vfs_readdir(d, &e) == 1) say("%c %lu %s\n", e.isdir ? 'D' : 'F', (unsigned long)e.size, e.name);
    vfs_closedir(d);
    say("END\n");
}

static void do_get(const char *arg, uint8_t *buf)
{
    char full[VFS_PATH];
    vfile *f;
    int32_t size;
    uint32_t crc = 0;
    int n;
    if (!resolve(arg, full)) return;
    if (!(f = vfs_open(full, "r"))) { say("ERR cannot open %s\n", full); return; }
    size = vfs_size(f);
    say("OK %ld\n", (long)size);
    while (size > 0 && (n = vfs_read(f, buf, size > BLOCK ? BLOCK : (int)size)) > 0) {
        hal_usb_write(buf, n);
        crc = crc32_update(crc, buf, n);
        size -= n;
    }
    vfs_close(f);
    /* a short read leaves the PC waiting: pad, and the CRC will disagree */
    if (size > 0) { memset(buf, 0, BLOCK); while (size > 0) { n = size > BLOCK ? BLOCK : (int)size; hal_usb_write(buf, n); size -= n; } }
    say("CRC %08lx\n", (unsigned long)crc);
    con_printf("sent %s\n", full);
}

static void do_put(char *arg, uint8_t *buf)
{
    char full[VFS_PATH], *sp = arg ? strrchr(arg, ' ') : 0;
    vfile *f;
    long size;
    uint32_t crc = 0;
    int ok = 1;
    if (!sp) { say("ERR usage: PUT path size\n"); return; }
    *sp = 0;
    size = atol(sp + 1);
    if (size < 0) { say("ERR bad size\n"); return; }
    if (!resolve(arg, full)) return;
    if (!(f = vfs_open(full, "w"))) { say("ERR cannot create %s\n", full); return; }
    say("OK\n");
    while (size > 0) {
        int want = size > BLOCK ? BLOCK : (int)size, got = 0, c;
        while (got < want) {
            if ((c = hal_usb_getc(3000)) < 0) break;
            buf[got++] = (uint8_t)c;
        }
        if (got < want) { ok = 0; break; }
        if (ok && vfs_write(f, buf, got) != got) ok = -1;
        crc = crc32_update(crc, buf, got);
        size -= got;
        say("A\n");
    }
    vfs_close(f);
    if (ok == 0) { vfs_remove(full); con_printf("xfer: %s timed out\n", full); return; }   /* the PC gave up */
    if (ok < 0) { say("ERR write failed (disk full?)\n"); return; }
    say("OK %08lx\n", (unsigned long)crc);
    con_printf("received %s\n", full);
}

int xfer_serve(void)
{
    char line[300];
    uint8_t *buf = (uint8_t *)malloc(BLOCK);
    int old;
    if (!buf) { con_puts("xfer: out of memory\n"); return 1; }
    con_puts("USB file transfer is on. On the PC run:\n"
             "  python3 calcxfer.py PORT ls /\n"
             "Press any key here to stop.\n");
    con_flush();
    old = con_mirror(0);         /* the port is ours now */
    while (read_line(line, sizeof line)) {
        char *arg = strchr(line, ' ');
        if (arg) *arg++ = 0;
        if (!strcmp(line, "HELLO")) say("OK CalcIDE %s\n", CALCIDE_VERSION);
        else if (!strcmp(line, "LS")) do_ls(arg);
        else if (!strcmp(line, "GET")) do_get(arg, buf);
        else if (!strcmp(line, "PUT")) do_put(arg, buf);
        else if (!strcmp(line, "MKDIR")) {
            char full[VFS_PATH];
            if (resolve(arg, full)) { if (vfs_mkdir(full)) say("OK\n"); else say("ERR cannot make %s\n", full); }
        } else if (!strcmp(line, "RM")) {
            char full[VFS_PATH];
            if (resolve(arg, full)) { if (vfs_remove(full)) say("OK\n"); else say("ERR cannot remove %s\n", full); }
        } else if (!strcmp(line, "BYE")) { say("OK\n"); break; }
        else if (line[0]) say("ERR unknown command %s\n", line);
        con_flush();
    }
    free(buf);
    con_mirror(old);
    con_puts("USB file transfer is off.\n");
    return 0;
}
