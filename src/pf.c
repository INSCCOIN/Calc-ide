/* pf.c - the page file: named objects in 768 KB of on-board flash.
 *
 * Adapted from the STORE of CalcOS (src/store.c, MIT licence). The layout:
 *
 *   page 0        directory: a header and up to 72 entries
 *   pages 1..191  object data, each object a run of whole 4 KB pages
 *
 * Allocation is first fit. Replacing an object writes the new copy into
 * pages the old copy is not using and only then rewrites the directory, so
 * a power cut in the middle leaves the old object intact. Reading costs no
 * RAM: pf_find() returns a pointer straight into memory-mapped flash.
 *
 * Flash wears out after roughly 100,000 erases of a sector, so this is for
 * saving things, not for variables that change every frame.
 */
#include "pf.h"
#include "hal.h"
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#define PF_MAGIC   "CIDEPF1"
#define DIR_PAGE   0
#define PAGE_MAX   4096
#define PAGES_MAX  256

typedef struct {
    char     magic[8];
    uint32_t version;
    uint32_t npages;
    uint32_t nent;
    uint32_t reserved[5];
} DirHdr;                       /* 32 bytes */

typedef struct {
    char     name[PF_NAMELEN];  /* NUL padded; empty means a free slot */
    uint16_t page;
    uint16_t pages;
    uint32_t len;
    uint32_t kind;
    uint32_t reserved;
} DirEnt;                       /* 56 bytes */

static uint8_t dirbuf[PAGE_MAX];
static int dir_loaded, dir_valid;

static int psize(void) { return hal_pf_page_size(); }
static int pcount(void) { int n = hal_pf_pages(); return n > PAGES_MAX ? PAGES_MAX : n; }
static DirHdr *hdr(void) { return (DirHdr *)dirbuf; }
static DirEnt *ents(void) { return (DirEnt *)(dirbuf + sizeof(DirHdr)); }

static int max_entries(void)
{
    int n = (psize() - (int)sizeof(DirHdr)) / (int)sizeof(DirEnt);
    return n > PF_MAXENT ? PF_MAXENT : n;
}

static void empty_dir(void)
{
    memset(dirbuf, 0, sizeof dirbuf);
    memcpy(hdr()->magic, PF_MAGIC, 8);
    hdr()->version = 1;
    hdr()->npages = (uint32_t)pcount();
    hdr()->nent = 0;
}

static int load_dir(void)
{
    const void *p;
    if (dir_loaded) return dir_valid;
    dir_loaded = 1;
    dir_valid = 0;
    if (pcount() < 2 || psize() > PAGE_MAX) return 0;
    p = hal_pf_read(DIR_PAGE);
    if (!p) return 0;
    memcpy(dirbuf, p, (size_t)psize());
    if (memcmp(hdr()->magic, PF_MAGIC, 8) != 0 || hdr()->version != 1
        || hdr()->nent > (uint32_t)max_entries()) empty_dir();   /* unformatted reads as 0xFF */
    dir_valid = 1;
    return 1;
}

static int commit_dir(void) { return hal_pf_write(DIR_PAGE, dirbuf, psize()); }

static void mark_used(uint8_t *used, const DirEnt *e)
{
    int i;
    for (i = 0; i < e->pages; i++)
        if (e->page + i < pcount()) used[e->page + i] = 1;
}

static int find_run(int want, const DirEnt *keep_busy)
{
    static uint8_t used[PAGES_MAX];
    int i, run = 0, n = pcount();
    uint32_t k;
    if (want <= 0) return -1;
    memset(used, 0, (size_t)n);
    used[DIR_PAGE] = 1;
    for (k = 0; k < hdr()->nent; k++) if (ents()[k].name[0]) mark_used(used, &ents()[k]);
    if (keep_busy) mark_used(used, keep_busy);
    for (i = 0; i < n; i++) {
        if (used[i]) { run = 0; continue; }
        if (++run == want) return i - want + 1;
    }
    return -1;
}

static DirEnt *find_ent(const char *name)
{
    uint32_t i;
    for (i = 0; i < hdr()->nent; i++)
        if (ents()[i].name[0] && strncmp(ents()[i].name, name, PF_NAMELEN) == 0) return &ents()[i];
    return 0;
}

int pf_available(void) { return pcount() > 1; }
int pf_total_pages(void) { return pcount(); }

int pf_format(void)
{
    int i, n = pcount();
    if (!pf_available()) return 0;
    for (i = n - 1; i >= 0; i--) hal_pf_erase(i);
    empty_dir();
    dir_loaded = dir_valid = 1;
    return commit_dir();
}

int pf_count(void)
{
    uint32_t i;
    int n = 0;
    if (!load_dir()) return 0;
    for (i = 0; i < hdr()->nent; i++) if (ents()[i].name[0]) n++;
    return n;
}

int pf_entry(int idx, const char **name, int *len, int *kind, int *page, int *pages)
{
    uint32_t i;
    int n = 0;
    if (!load_dir()) return 0;
    for (i = 0; i < hdr()->nent; i++) {
        if (!ents()[i].name[0]) continue;
        if (n++ != idx) continue;
        if (name) *name = ents()[i].name;
        if (len) *len = (int)ents()[i].len;
        if (kind) *kind = (int)ents()[i].kind;
        if (page) *page = ents()[i].page;
        if (pages) *pages = ents()[i].pages;
        return 1;
    }
    return 0;
}

const void *pf_find(const char *name, int *len, int *kind)
{
    DirEnt *e;
    if (!load_dir()) return 0;
    e = find_ent(name);
    if (!e) return 0;
    if (len) *len = (int)e->len;
    if (kind) *kind = (int)e->kind;
    return hal_pf_read(e->page);
}

int pf_free_pages(void)
{
    static uint8_t used[PAGES_MAX];
    int i, n = pcount(), f = 0;
    uint32_t k;
    if (!load_dir()) return 0;
    memset(used, 0, (size_t)n);
    used[DIR_PAGE] = 1;
    for (k = 0; k < hdr()->nent; k++) if (ents()[k].name[0]) mark_used(used, &ents()[k]);
    for (i = 0; i < n; i++) if (!used[i]) f++;
    return f;
}

int pf_put(const char *name, const void *data, int len, int kind)
{
    int ps = psize(), want, start, i;
    DirEnt *old, *slot = 0;
    DirEnt saved;
    uint32_t k;
    if (!load_dir() || !name || !*name || len < 0) return PF_ERR;
    if ((int)strlen(name) >= PF_NAMELEN) return PF_ENAME;
    want = (len + ps - 1) / ps;
    if (want == 0) want = 1;
    old = find_ent(name);
    if (old) saved = *old;
    start = find_run(want, old);
    if (start < 0) return PF_EFULL;
    for (i = 0; i < want; i++) {
        int off = i * ps, n = len - off;
        if (n > ps) n = ps;
        if (n < 0) n = 0;
        if (!hal_pf_write(start + i, (const char *)data + off, n)) return PF_EIO;
    }
    if (old) slot = old;
    else {
        for (k = 0; k < hdr()->nent; k++) if (!ents()[k].name[0]) { slot = &ents()[k]; break; }
        if (!slot) {
            if ((int)hdr()->nent >= max_entries()) return PF_EFULL;
            slot = &ents()[hdr()->nent];
            hdr()->nent++;
        }
    }
    memset(slot, 0, sizeof *slot);
    snprintf(slot->name, PF_NAMELEN, "%s", name);
    slot->page = (uint16_t)start;
    slot->pages = (uint16_t)want;
    slot->len = (uint32_t)len;
    slot->kind = (uint32_t)kind;
    if (!commit_dir()) {
        if (old) *slot = saved;
        return PF_EIO;
    }
    return PF_OK;
}

int pf_delete(const char *name)
{
    DirEnt *e;
    if (!load_dir()) return PF_ERR;
    e = find_ent(name);
    if (!e) return PF_ENOENT;
    memset(e, 0, sizeof *e);
    return commit_dir() ? PF_OK : PF_EIO;
}

const char *pf_kind_name(int kind)
{
    switch (kind) {
    case PF_RAW: return "data";
    case PF_TEXT: return "text";
    case PF_PROG: return "program";
    case PF_VAR: return "var";
    default: return "?";
    }
}

const char *pf_strerror(int e)
{
    switch (e) {
    case PF_OK: return "ok";
    case PF_EFULL: return "page file full";
    case PF_ENOENT: return "no such object";
    case PF_EIO: return "flash write failed";
    case PF_ENAME: return "name too long (max 39)";
    default: return "page file not available";
    }
}
