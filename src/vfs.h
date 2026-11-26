/* vfs.h - the file system seen by the shell, editor, compiler and VM.
 *
 * Paths are absolute ("/dir/file.c") once resolved; vfs_resolve() turns a
 * relative path into one using the shell's current directory. Firmware uses
 * FatFs on the SD card; the host build maps "/" onto a directory.
 */
#ifndef VFS_H
#define VFS_H

#include <stdint.h>

#define VFS_PATH 160

typedef struct vfile vfile;

typedef struct {
    char     name[96];
    int      isdir;
    uint32_t size;
} vdirent;

int    vfs_mount(void);
int    vfs_unmount(void);
int    vfs_mounted(void);
int    vfs_sync(void);

vfile *vfs_open(const char *path, const char *mode);   /* "r", "w", "a", "r+", "w+" ... */
int    vfs_read(vfile *f, void *buf, int n);
int    vfs_write(vfile *f, const void *buf, int n);
int    vfs_seek(vfile *f, int32_t off, int whence);
int32_t vfs_tell(vfile *f);
int32_t vfs_size(vfile *f);
int    vfs_eof(vfile *f);
int    vfs_flush(vfile *f);
int    vfs_close(vfile *f);

int    vfs_stat(const char *path, int *isdir, uint32_t *size);
int    vfs_mkdir(const char *path);
int    vfs_remove(const char *path);        /* file or empty directory */
int    vfs_rename(const char *from, const char *to);

void  *vfs_opendir(const char *path);
int    vfs_readdir(void *d, vdirent *e);    /* 1 = entry, 0 = end */
void   vfs_closedir(void *d);

/* sizes in KB */
int    vfs_space(uint32_t *total_kb, uint32_t *free_kb);
const char *vfs_fstype(void);
int    vfs_label(char *buf, int n);
const char *vfs_error(void);                /* text of the last failure */

/* path handling (common code, src/vfs_common.c) */
extern char vfs_cwd[VFS_PATH];
int    vfs_resolve(const char *in, char *out);  /* 0 if too long */
const char *vfs_basename(const char *p);

/* whole-file helpers */
char  *vfs_load(const char *path, int *len, int extra);  /* malloc'd, NUL terminated */
int    vfs_save(const char *path, const void *data, int len);

#endif
