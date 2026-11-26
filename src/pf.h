/* pf.h - the page file: named objects in 768 KB of the RP2350's flash.
 *
 * Modelled on the STORE of CalcOS. Page 0 is a directory; every object
 * occupies a run of whole 4 KB pages; replacing an object writes the new
 * copy first and only then switches the directory, so a power cut never
 * loses the old one. Reading is free: objects are read in place through
 * the XIP window and cost no RAM.
 */
#ifndef PF_H
#define PF_H

#define PF_BYTES     (768 * 1024)
#define PF_NAMELEN   40
#define PF_MAXENT    72

enum { PF_RAW = 0, PF_TEXT = 1, PF_PROG = 2, PF_VAR = 3 };
enum { PF_OK = 0, PF_ERR = -1, PF_EFULL = -2, PF_ENOENT = -3, PF_EIO = -4, PF_ENAME = -5 };

int         pf_available(void);
int         pf_format(void);
int         pf_count(void);
int         pf_entry(int idx, const char **name, int *len, int *kind, int *page, int *pages);
const void *pf_find(const char *name, int *len, int *kind);
int         pf_put(const char *name, const void *data, int len, int kind);
int         pf_delete(const char *name);
int         pf_free_pages(void);
int         pf_total_pages(void);
const char *pf_kind_name(int kind);
const char *pf_strerror(int e);

#endif
