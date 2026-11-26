/* sound.c - melodies and WAV files through the PicoCalc's speakers.
 *
 * Tracker strings, space or comma separated, case-insensitive:
 *   C4 D#4 Eb4      a note: letter, optional # or b, octave 0-8 (default 4)
 *   C4/8  C4/4.     length: /1 /2 /4 /8 /16 /32, '.' makes it dotted
 *   R  R/2          a rest
 *   T140            tempo in quarter notes per minute (default 120)
 *   L8              default length for following notes (default 4)
 *   O5              default octave for notes without one
 * A4 is 440 Hz. Any key in break_key stops playback.
 */
#include "sound.h"
#include "hal.h"
#include "console.h"
#include "config.h"
#include "vfs.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <math.h>

static char err[64];
const char *play_error(void) { return err; }

/* wait ms, keeping an eye on the break key; 1 if broken */
static int wait_ms(int ms)
{
    uint32_t end = hal_millis() + (uint32_t)ms;
    while ((int32_t)(end - hal_millis()) > 0) {
        if (con_poll_break()) return 1;
        hal_sleep_ms(2);
    }
    return 0;
}

int play_notes(const char *s)
{
    int bpm = 120, deflen = 4, defoct = 4;
    err[0] = 0;
    while (*s) {
        char tok[16];
        int n = 0, len, dotted = 0, ms, semis, oct;
        const char *p;
        while (*s == ' ' || *s == ',' || *s == '\t' || *s == '\n') s++;
        if (!*s) break;
        while (*s && *s != ' ' && *s != ',' && *s != '\t' && *s != '\n' && n < 15) tok[n++] = (char)toupper((unsigned char)*s++);
        tok[n] = 0;
        p = tok;
        if (*p == 'T' || *p == 'L' || *p == 'O') {
            int v = atoi(p + 1);
            if (*p == 'T' && v >= 20 && v <= 600) bpm = v;
            else if (*p == 'L' && (v == 1 || v == 2 || v == 4 || v == 8 || v == 16 || v == 32)) deflen = v;
            else if (*p == 'O' && v >= 0 && v <= 8 && isdigit((unsigned char)p[1])) defoct = v;
            else { snprintf(err, sizeof err, "bad setting '%s'", tok); return -1; }
            continue;
        }
        semis = -1;
        if (*p == 'R') p++;
        else {
            static const int base[7] = {9, 11, 0, 2, 4, 5, 7};   /* A B C D E F G */
            if (*p < 'A' || *p > 'G') { snprintf(err, sizeof err, "bad note '%s'", tok); return -1; }
            semis = base[*p - 'A'];
            p++;
            if (*p == '#') { semis++; p++; }
            else if (*p == 'B' && (isdigit((unsigned char)p[1]) || p[1] == '/' || !p[1])) { semis--; p++; }
        }
        oct = defoct;
        if (isdigit((unsigned char)*p)) oct = *p++ - '0';
        len = deflen;
        if (*p == '/') { len = atoi(p + 1); p++; while (isdigit((unsigned char)*p)) p++; }
        if (*p == '.') { dotted = 1; p++; }
        if (*p || len <= 0 || len > 64) { snprintf(err, sizeof err, "bad note '%s'", tok); return -1; }
        ms = 240000 / bpm / len;
        if (dotted) ms += ms / 2;
        if (semis < 0 || g_conf.mute) {
            if (wait_ms(ms)) { hal_tone_off(); return 1; }
        } else {
            /* semitones from A4 */
            int k = (oct - 4) * 12 + semis - 9;
            int hz = (int)(440.0 * pow(2.0, k / 12.0) + 0.5);
            int gap = ms > 40 ? 12 : 0;      /* a short gap keeps repeated notes apart */
            hal_tone(hz, 0);
            if (wait_ms(ms - gap)) { hal_tone_off(); return 1; }
            hal_tone_off();
            if (gap && wait_ms(gap)) return 1;
        }
    }
    hal_tone_off();
    return 0;
}

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

int play_wav(const char *path)
{
    char full[VFS_PATH];
    uint8_t h[12], ck[8], fmt[16];
    uint8_t *buf = 0;
    vfile *f;
    uint32_t rate = 0, left = 0, frame;
    int chans = 0, bits = 0, found = 0, rc = -1, n, i, stop = 0;
    uint64_t next;
    err[0] = 0;
    if (!vfs_resolve(path, full) || !(f = vfs_open(full, "r"))) { snprintf(err, sizeof err, "cannot open %s", path); return -1; }
    if (vfs_read(f, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { snprintf(err, sizeof err, "not a WAV file"); goto out; }
    /* walk the chunks to "fmt " and "data" */
    while (vfs_read(f, ck, 8) == 8) {
        uint32_t sz = le32(ck + 4);
        if (!memcmp(ck, "fmt ", 4)) {
            if (sz < 16 || vfs_read(f, fmt, 16) != 16) break;
            if (le16(fmt) != 1) { snprintf(err, sizeof err, "only PCM WAV files are supported"); goto out; }
            chans = le16(fmt + 2);
            rate = le32(fmt + 4);
            bits = le16(fmt + 14);
            if (sz > 16) vfs_seek(f, (int32_t)(sz - 16 + (sz & 1)), 1);
        } else if (!memcmp(ck, "data", 4)) {
            left = sz;
            found = 1;
            break;
        } else vfs_seek(f, (int32_t)(sz + (sz & 1)), 1);
    }
    if (!found || !rate) { snprintf(err, sizeof err, "no audio data"); goto out; }
    if ((bits != 8 && bits != 16) || chans < 1 || chans > 2 || rate < 2000 || rate > 48000) {
        snprintf(err, sizeof err, "unsupported: %d-bit, %d channels, %u Hz", bits, chans, (unsigned)rate);
        goto out;
    }
    frame = (uint32_t)(chans * bits / 8);
    buf = (uint8_t *)malloc(2048);
    if (!buf) { snprintf(err, sizeof err, "out of memory"); goto out; }
    if (!g_conf.mute) hal_pcm_start((int)rate);
    next = hal_micros();
    while (left >= frame && !stop) {
        uint32_t want = left > 2048 ? 2048 : left;
        want -= want % frame;
        n = vfs_read(f, buf, (int)want);
        if (n <= 0) break;
        left -= (uint32_t)n;
        for (i = 0; i + (int)frame <= n; i += (int)frame) {
            int v;
            if (bits == 8) v = chans == 2 ? (buf[i] + buf[i + 1]) / 2 : buf[i];
            else {
                int s = (int16_t)le16(buf + i);
                if (chans == 2) s = (s + (int16_t)le16(buf + i + 2)) / 2;
                v = (s >> 8) + 128;
            }
            v = 128 + (v - 128) * g_conf.volume / 100;
            next += 1000000u / rate;
            while (hal_micros() < next) { }
            if (!g_conf.mute) hal_pcm_write((uint8_t)v);
        }
        if (con_poll_break()) stop = 1;
    }
    rc = stop ? 1 : 0;
out:
    if (buf) { hal_pcm_stop(); free(buf); }
    vfs_close(f);
    return rc;
}
