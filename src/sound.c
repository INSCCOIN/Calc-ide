/* sound.c - a small synthesizer for the PicoCalc's speakers, and WAV files.
 *
 * play() takes a tracker string. Up to four channels, separated by '|',
 * play at the same time; each is mixed in software at 16 kHz and sent to
 * the speakers through the PWM.
 *
 *   C4 D#4 Eb4      a note: letter, # or b, octave 0-8 (default: O)
 *   /1 /2 /4 /8 /16 /32 and '.'   length and dotted length (default: L)
 *   R               a rest
 *   K SN HH OH CP TH TL CY       drums: kick, snare, closed and open
 *                   hi-hat, clap, high and low tom, crash cymbal
 *   T120            tempo, quarter notes per minute (the first T in the
 *                   string sets the starting tempo of every channel)
 *   L8  O5          default length and octave
 *   ISQ IPLS ITRI ISAW ISIN   instrument: square, pulse, triangle, saw, sine
 *   V0..V9          channel volume (default 7)
 *   [ ... ]x3       repeat (nesting allowed)
 *   @/path.song     play a song file (see play_file)
 *
 * A4 is 440 Hz. The break key stops playback.
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

static char err[80];
const char *play_error(void) { return err; }

#define SR 16000            /* synth sample rate */
#define MAXCH 4
#define MAXTEXT 32768       /* per channel, after repeats are expanded */

enum { W_SQ, W_PLS, W_TRI, W_SAW, W_SIN };
enum { D_NONE, D_KICK, D_SNARE, D_HAT, D_OHAT, D_CLAP, D_TOMH, D_TOML, D_CRASH };

static const int8_t sine64[64] = {
    0, 12, 25, 37, 49, 60, 71, 81, 90, 98, 106, 112, 117, 122, 125, 126,
    127, 126, 125, 122, 117, 112, 106, 98, 90, 81, 71, 60, 49, 37, 25, 12,
    0, -12, -25, -37, -49, -60, -71, -81, -90, -98, -106, -112, -117, -122, -125, -126,
    -127, -126, -125, -122, -117, -112, -106, -98, -90, -81, -71, -60, -49, -37, -25, -12
};

typedef struct {
    const char *p;          /* where parsing continues */
    int bpm, deflen, defoct, wave, vol;
    /* the current event */
    int32_t left;           /* samples remaining */
    int32_t len;            /* total samples */
    int32_t n;              /* samples played */
    uint32_t phase, inc;
    int drum;               /* D_* or D_NONE for a note; -1 for a rest */
    int32_t amp, amp2;      /* drum envelopes, 0..32767 */
    int32_t k1, k2;         /* their per-sample decay (16.16) */
    uint16_t lfsr;
    int prev;               /* for the hi-hat's high-pass */
    int done;
} Chan;

/* ------------------------------------------------------------------ */
/* text: song files and [ ]xN repeats                                  */
/* ------------------------------------------------------------------ */

/* expand the innermost [ ... ]xN until none are left */
static char *expand(const char *in)
{
    size_t cap = strlen(in) + 1;
    char *s = (char *)malloc(cap);
    if (!s) return 0;
    strcpy(s, in);
    for (;;) {
        char *open = 0, *q, *close, *out;
        int times = 2;
        size_t blen, nlen;
        for (q = s; *q; q++) {
            if (*q == '[') open = q;
            else if (*q == ']' && open) break;
        }
        if (!*q || !open) break;
        close = q;
        q = close + 1;
        if (*q == 'x' || *q == 'X' || *q == '*') {
            times = atoi(q + 1);
            q++;
            while (isdigit((unsigned char)*q)) q++;
        }
        if (times < 1) times = 1;
        if (times > 64) times = 64;
        blen = (size_t)(close - open - 1);
        nlen = (size_t)(open - s) + blen * (size_t)times + (size_t)times + strlen(q) + 1;
        if (nlen > MAXTEXT) { free(s); snprintf(err, sizeof err, "song too long after repeats"); return 0; }
        out = (char *)malloc(nlen);
        if (!out) { free(s); return 0; }
        {
            char *w = out;
            int t;
            memcpy(w, s, (size_t)(open - s));
            w += open - s;
            for (t = 0; t < times; t++) {
                memcpy(w, open + 1, blen);
                w += blen;
                *w++ = ' ';
            }
            strcpy(w, q);
        }
        free(s);
        s = out;
    }
    if (strchr(s, '[') || strchr(s, ']')) { free(s); snprintf(err, sizeof err, "unmatched [ or ]"); return 0; }
    return s;
}

/* A song file: '#' comments; "N: notes" lines add to channel N (1-4);
 * other lines go to channel 1. Channels are joined with '|'. */
static char *load_song(const char *path)
{
    int len, ch, i;
    char *txt = vfs_load(path, &len, 0), *line, *nl;
    char *chan[MAXCH] = {0};
    size_t clen[MAXCH] = {0}, total = 0;
    char *out;
    if (!txt) { snprintf(err, sizeof err, "cannot open %s", path); return 0; }
    for (line = txt; line && *line; line = nl) {
        char *t;
        nl = strchr(line, '\n');
        if (nl) *nl++ = 0;
        /* '#' starts a comment at the start of a line or after a space;
         * straight after a note letter it is a sharp */
        for (t = line; *t; t++)
            if (*t == '#' && (t == line || t[-1] == ' ' || t[-1] == '\t')) { *t = 0; break; }
        while (*line == ' ' || *line == '\t' || *line == '\r') line++;
        if (!*line) continue;
        ch = 0;
        if (line[0] >= '1' && line[0] <= '4' && line[1] == ':') { ch = line[0] - '1'; line += 2; }
        {
            size_t l = strlen(line);
            char *nb = (char *)realloc(chan[ch], clen[ch] + l + 2);
            if (!nb) break;
            chan[ch] = nb;
            memcpy(chan[ch] + clen[ch], line, l);
            clen[ch] += l;
            chan[ch][clen[ch]++] = ' ';
            chan[ch][clen[ch]] = 0;
        }
    }
    free(txt);
    for (i = 0; i < MAXCH; i++) total += clen[i] + 2;
    out = (char *)malloc(total + 1);
    if (out) {
        out[0] = 0;
        for (i = 0; i < MAXCH; i++) {
            if (!clen[i]) continue;
            if (out[0]) strcat(out, "|");
            strcat(out, chan[i]);
        }
    }
    for (i = 0; i < MAXCH; i++) free(chan[i]);
    return out;
}

/* ------------------------------------------------------------------ */
/* parsing one event                                                   */
/* ------------------------------------------------------------------ */

static int32_t decay_k(int ms)
{
    /* per-sample factor that falls to ~1/1000 after ms */
    double k = exp(-6.9 / (SR * ms / 1000.0));
    return (int32_t)(k * 65536.0);
}

/* the next note, rest or drum of a channel; 0 at the end, -1 on error */
static int next_event(Chan *c)
{
    for (;;) {
        char tok[16];
        int n = 0, len, dotted = 0, semis = -2, oct, drum = D_NONE;
        const char *p;
        while (*c->p == ' ' || *c->p == ',' || *c->p == '\t' || *c->p == '\n' || *c->p == '\r') c->p++;
        if (!*c->p || *c->p == '|') return 0;
        while (*c->p && *c->p != ' ' && *c->p != ',' && *c->p != '\t' && *c->p != '\n' && *c->p != '|' && n < 15)
            tok[n++] = (char)toupper((unsigned char)*c->p++);
        tok[n] = 0;
        p = tok;
        /* drums first: their names overlap tempo, octave and note letters */
        if (!strncmp(p, "SN", 2)) { drum = D_SNARE; p += 2; }
        else if (!strncmp(p, "HH", 2)) { drum = D_HAT; p += 2; }
        else if (!strncmp(p, "OH", 2)) { drum = D_OHAT; p += 2; }
        else if (!strncmp(p, "CP", 2)) { drum = D_CLAP; p += 2; }
        else if (!strncmp(p, "TH", 2)) { drum = D_TOMH; p += 2; }
        else if (!strncmp(p, "TL", 2)) { drum = D_TOML; p += 2; }
        else if (!strncmp(p, "CY", 2)) { drum = D_CRASH; p += 2; }
        else if (p[0] == 'K' && (!p[1] || p[1] == '/' || p[1] == '.')) { drum = D_KICK; p += 1; }
        else if (p[0] == 'I') {
            if (!strcmp(p, "ISQ")) c->wave = W_SQ;
            else if (!strcmp(p, "IPLS")) c->wave = W_PLS;
            else if (!strcmp(p, "ITRI")) c->wave = W_TRI;
            else if (!strcmp(p, "ISAW")) c->wave = W_SAW;
            else if (!strcmp(p, "ISIN")) c->wave = W_SIN;
            else { snprintf(err, sizeof err, "unknown instrument '%s'", tok); return -1; }
            continue;
        } else if ((p[0] == 'T' || p[0] == 'L' || p[0] == 'O' || p[0] == 'V') && isdigit((unsigned char)p[1])) {
            int v = atoi(p + 1);
            if (p[0] == 'T' && v >= 20 && v <= 600) c->bpm = v;
            else if (p[0] == 'L' && (v == 1 || v == 2 || v == 4 || v == 8 || v == 16 || v == 32)) c->deflen = v;
            else if (p[0] == 'O' && v >= 0 && v <= 8) c->defoct = v;
            else if (p[0] == 'V' && v >= 0 && v <= 9) c->vol = v;
            else { snprintf(err, sizeof err, "bad setting '%s'", tok); return -1; }
            continue;
        }
        if (drum == D_NONE) {
            if (*p == 'R') { semis = -1; p++; }
            else {
                static const int base[7] = {9, 11, 0, 2, 4, 5, 7};   /* A B C D E F G */
                if (*p < 'A' || *p > 'G') { snprintf(err, sizeof err, "bad note '%s'", tok); return -1; }
                semis = base[*p - 'A'];
                p++;
                if (*p == '#') { semis++; p++; }
                else if (*p == 'B' && (isdigit((unsigned char)p[1]) || p[1] == '/' || p[1] == '.' || !p[1])) { semis--; p++; }
            }
        }
        oct = c->defoct;
        if (drum == D_NONE && semis >= -1 && isdigit((unsigned char)*p)) oct = *p++ - '0';
        len = c->deflen;
        if (*p == '/') { len = atoi(p + 1); p++; while (isdigit((unsigned char)*p)) p++; }
        if (*p == '.') { dotted = 1; p++; }
        if (*p || len <= 0 || len > 64) { snprintf(err, sizeof err, "bad note '%s'", tok); return -1; }
        c->len = (int32_t)((int64_t)SR * 240 / c->bpm / len);
        if (dotted) c->len += c->len / 2;
        if (c->len < 1) c->len = 1;
        c->left = c->len;
        c->n = 0;
        c->drum = drum;
        if (drum == D_NONE && semis == -1) { c->drum = -1; return 1; }
        if (drum == D_NONE) {
            int k = (oct - 4) * 12 + semis - 9;
            double hz = 440.0 * pow(2.0, k / 12.0);
            c->inc = (uint32_t)(hz * 4294967296.0 / SR);
            return 1;
        }
        /* drum envelopes */
        c->amp = 32767;
        c->amp2 = 32767;
        c->phase = 0;
        switch (drum) {
        case D_KICK: c->k1 = decay_k(260); c->k2 = decay_k(40); break;
        case D_SNARE: c->k1 = decay_k(170); c->k2 = decay_k(70); break;
        case D_HAT: c->k1 = decay_k(45); break;
        case D_OHAT: c->k1 = decay_k(320); break;
        case D_CLAP: c->k1 = decay_k(120); break;
        case D_TOMH: case D_TOML: c->k1 = decay_k(300); c->k2 = decay_k(120); break;
        default: c->k1 = decay_k(1200); break;
        }
        return 1;
    }
}

/* ------------------------------------------------------------------ */
/* one sample of one channel, -127..127 before volume                  */
/* ------------------------------------------------------------------ */

static int noise(Chan *c)
{
    unsigned bit = ((c->lfsr >> 0) ^ (c->lfsr >> 2) ^ (c->lfsr >> 3) ^ (c->lfsr >> 5)) & 1u;
    c->lfsr = (uint16_t)((c->lfsr >> 1) | (bit << 15));
    return (int)(c->lfsr & 0xFF) - 128;
}

static int voice(Chan *c)
{
    int v = 0, env;
    if (c->drum == -1) return 0;
    if (c->drum == D_NONE) {
        uint32_t ph = c->phase;
        switch (c->wave) {
        case W_SQ: v = (ph & 0x80000000u) ? 90 : -90; break;
        case W_PLS: v = (ph >> 30) == 0 ? 90 : -90; break;
        case W_SAW: v = (int)(int8_t)(ph >> 24) * 100 / 128; break;
        case W_TRI: { int t = (int)(ph >> 24); v = (t < 128 ? t * 2 - 128 : (255 - t) * 2 - 128); break; }
        default: v = sine64[ph >> 26]; break;
        }
        c->phase += c->inc;
        /* a short attack, and a release over the end of the note */
        env = 256;
        if (c->n < 48) env = c->n * 256 / 48;
        {
            int32_t rel = c->len / 5;
            if (rel > SR / 20) rel = SR / 20;
            if (rel > 0 && c->left < rel) env = env * c->left / rel;
        }
        return v * env / 256;
    }
    switch (c->drum) {
    case D_KICK: {
        /* a sine that drops from ~150 Hz to ~45 Hz */
        int32_t f = 45 + (int32_t)(110 * (int64_t)c->amp2 / 32767);
        c->phase += (uint32_t)((int64_t)f * 4294967296LL / SR);
        v = sine64[c->phase >> 26] * c->amp / 32767;
        c->amp2 = (int32_t)((int64_t)c->amp2 * c->k2 >> 16);
        break;
    }
    case D_SNARE: {
        int tone;
        c->phase += (uint32_t)(190LL * 4294967296LL / SR);
        tone = sine64[c->phase >> 26] * c->amp2 / 32767;
        v = (noise(c) * c->amp / 32767) + tone / 2;
        c->amp2 = (int32_t)((int64_t)c->amp2 * c->k2 >> 16);
        break;
    }
    case D_HAT: case D_OHAT: case D_CRASH: {
        int nz = noise(c), hp = nz - c->prev;          /* high-passed noise */
        c->prev = nz;
        v = (hp * 3 / 4) * c->amp / 32767;
        if (c->drum == D_CRASH) v = (hp / 2 + nz / 2) * c->amp / 32767;
        break;
    }
    case D_CLAP: {
        /* three quick bursts, then a tail */
        int32_t ms = c->n * 1000 / SR;
        int32_t a = c->amp;
        if (ms < 30 && (ms % 10) >= 6) a = a / 6;
        v = noise(c) * a / 32767;
        break;
    }
    default: {   /* toms: a sine sweeping down */
        int32_t lo = c->drum == D_TOMH ? 140 : 80;
        int32_t f = lo + (int32_t)(lo * (int64_t)c->amp2 / 32767);
        c->phase += (uint32_t)((int64_t)f * 4294967296LL / SR);
        v = sine64[c->phase >> 26] * c->amp / 32767;
        c->amp2 = (int32_t)((int64_t)c->amp2 * c->k2 >> 16);
        break;
    }
    }
    c->amp = (int32_t)((int64_t)c->amp * c->k1 >> 16);
    return v;
}

/* ------------------------------------------------------------------ */
/* playing                                                             */
/* ------------------------------------------------------------------ */

static int synth(const char *song)
{
    Chan ch[MAXCH];
    char *texts[MAXCH] = {0};
    int nch = 0, i, rc = 0, bpm0 = 120, active, realtime = hal_pcm_realtime();
    uint64_t next;
    uint32_t count = 0;
    const char *q = song, *t;
    /* the first tempo in the whole string starts every channel */
    for (t = song; *t; t++)
        if ((t == song || t[-1] == ' ' || t[-1] == '|' || t[-1] == ',' || t[-1] == '[') && (*t == 'T' || *t == 't')
            && isdigit((unsigned char)t[1])) { bpm0 = atoi(t + 1); break; }
    if (bpm0 < 20 || bpm0 > 600) bpm0 = 120;
    memset(ch, 0, sizeof ch);
    while (nch < MAXCH) {
        const char *bar = strchr(q, '|');
        size_t l = bar ? (size_t)(bar - q) : strlen(q);
        char *piece = (char *)malloc(l + 1);
        if (!piece) { rc = -1; snprintf(err, sizeof err, "out of memory"); goto out; }
        memcpy(piece, q, l);
        piece[l] = 0;
        texts[nch] = expand(piece);
        free(piece);
        if (!texts[nch]) { rc = -1; if (!err[0]) snprintf(err, sizeof err, "out of memory"); goto out; }
        ch[nch].p = texts[nch];
        ch[nch].bpm = bpm0;
        ch[nch].deflen = 4;
        ch[nch].defoct = 4;
        ch[nch].vol = 7;
        ch[nch].wave = W_SQ;
        ch[nch].lfsr = (uint16_t)(0xACE1u + nch * 977u);
        nch++;
        if (!bar) break;
        q = bar + 1;
    }
    /* check the whole song before making a sound */
    for (i = 0; i < nch; i++) {
        Chan probe = ch[i];
        int r;
        while ((r = next_event(&probe)) > 0) { }
        if (r < 0) { rc = -1; goto out; }
    }
    if (!g_conf.mute) hal_pcm_start(SR);
    next = hal_micros();
    for (;;) {
        int mix = 0;
        active = 0;
        for (i = 0; i < nch; i++) {
            Chan *c = &ch[i];
            if (c->done) continue;
            if (c->left <= 0) {
                int r = next_event(c);
                if (r <= 0) { c->done = 1; continue; }
            }
            active++;
            mix += voice(c) * c->vol / 9;
            c->left--;
            c->n++;
        }
        if (!active) break;
        /* scale by the number of channels so a full song rarely clips,
         * then the master volume */
        mix = mix * 2 / (nch + 1) * g_conf.volume / 100;
        /* a soft knee above 96, so peaks round off instead of clipping */
        if (mix > 96) mix = 96 + (mix - 96) * 31 / (mix - 96 + 31);
        else if (mix < -96) mix = -96 - (-96 - mix) * 31 / (-96 - mix + 31);
        if (mix > 127) mix = 127;
        if (mix < -127) mix = -127;
        if (realtime) {
            next += 1000000u / SR;
            while (hal_micros() < next) { }
        }
        if (!g_conf.mute) hal_pcm_write((uint8_t)(mix + 128));
        if ((++count & 511) == 0 && con_poll_break()) { rc = 1; break; }
    }
out:
    if (rc >= 0 && !g_conf.mute) hal_pcm_stop();
    for (i = 0; i < MAXCH; i++) free(texts[i]);
    return rc;
}

int play_notes(const char *s)
{
    int r;
    err[0] = 0;
    while (*s == ' ') s++;
    if (*s == '@') return play_file(s + 1);
    r = synth(s);
    hal_tone_off();
    return r;
}

int play_file(const char *path)
{
    char *song;
    int r;
    err[0] = 0;
    song = load_song(path);
    if (!song) { if (!err[0]) snprintf(err, sizeof err, "out of memory"); return -1; }
    r = synth(song);
    hal_tone_off();
    free(song);
    return r;
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
