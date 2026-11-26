/* vfs_fatfs.c - the SD card through FatFs (FAT12/16/32 and exFAT, long names). */
#include "vfs.h"
#include "ff.h"
#include "picocalc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

struct vfile { FIL fil; };

static FATFS fs;
static int mounted;
static FRESULT last;

static const char *fr_text(FRESULT r)
{
    switch (r) {
    case FR_OK: return "ok";
    case FR_DISK_ERR: return "disk error";
    case FR_NOT_READY: return "card not ready";
    case FR_NO_FILE: return "file not found";
    case FR_NO_PATH: return "path not found";
    case FR_INVALID_NAME: return "invalid name";
    case FR_DENIED: return "access denied (directory not empty, or card full?)";
    case FR_EXIST: return "already exists";
    case FR_WRITE_PROTECTED: return "write protected";
    case FR_NO_FILESYSTEM: return "no FAT file system";
    case FR_TOO_MANY_OPEN_FILES: return "too many open files";
    case FR_INVALID_OBJECT: return "invalid file";
    default: return "file system error";
    }
}

const char *vfs_error(void) { return fr_text(last); }

int vfs_mount(void)
{
    if (mounted) return 1;
    sd_deinit();
    if (!sd_init()) { last = FR_NOT_READY; return 0; }
    last = f_mount(&fs, "", 1);
    mounted = last == FR_OK;
    return mounted;
}

int vfs_unmount(void)
{
    f_mount(0, "", 0);
    mounted = 0;
    sd_deinit();
    return 1;
}

int vfs_mounted(void) { return mounted; }
int vfs_sync(void) { return sd_sync(); }

vfile *vfs_open(const char *path, const char *mode)
{
    BYTE m = 0;
    vfile *f;
    if (!mounted) { last = FR_NOT_READY; return 0; }
    if (mode[0] == 'r') m = FA_READ | (strchr(mode, '+') ? FA_WRITE : 0);
    else if (mode[0] == 'w') m = FA_CREATE_ALWAYS | FA_WRITE | (strchr(mode, '+') ? FA_READ : 0);
    else if (mode[0] == 'a') m = FA_OPEN_APPEND | FA_WRITE | (strchr(mode, '+') ? FA_READ : 0);
    else return 0;
    f = (vfile *)malloc(sizeof *f);
    if (!f) { last = FR_NOT_ENOUGH_CORE; return 0; }
    last = f_open(&f->fil, path, m);
    if (last != FR_OK) { free(f); return 0; }
    return f;
}

int vfs_read(vfile *f, void *buf, int n)
{
    UINT got = 0;
    last = f_read(&f->fil, buf, (UINT)n, &got);
    return last == FR_OK ? (int)got : -1;
}

int vfs_write(vfile *f, const void *buf, int n)
{
    UINT put = 0;
    last = f_write(&f->fil, buf, (UINT)n, &put);
    return last == FR_OK ? (int)put : -1;
}

int vfs_seek(vfile *f, int32_t off, int whence)
{
    FSIZE_t base = 0;
    if (whence == 1) base = f_tell(&f->fil);
    else if (whence == 2) base = f_size(&f->fil);
    if ((int64_t)base + off < 0) return -1;
    last = f_lseek(&f->fil, (FSIZE_t)((int64_t)base + off));
    return last == FR_OK ? 0 : -1;
}

int32_t vfs_tell(vfile *f) { return (int32_t)f_tell(&f->fil); }
int32_t vfs_size(vfile *f) { return (int32_t)f_size(&f->fil); }
int vfs_eof(vfile *f) { return f_eof(&f->fil); }
int vfs_flush(vfile *f) { last = f_sync(&f->fil); return last == FR_OK ? 0 : -1; }

int vfs_close(vfile *f)
{
    last = f_close(&f->fil);
    free(f);
    return last == FR_OK ? 0 : -1;
}

int vfs_stat(const char *path, int *isdir, uint32_t *size)
{
    FILINFO fi;
    if (!mounted) return 0;
    if (!strcmp(path, "/") || !path[0]) { if (isdir) *isdir = 1; if (size) *size = 0; return 1; }
    last = f_stat(path, &fi);
    if (last != FR_OK) return 0;
    if (isdir) *isdir = (fi.fattrib & AM_DIR) != 0;
    if (size) *size = (uint32_t)fi.fsize;
    return 1;
}

int vfs_mkdir(const char *path) { last = f_mkdir(path); return last == FR_OK; }
int vfs_remove(const char *path) { last = f_unlink(path); return last == FR_OK; }
int vfs_rename(const char *from, const char *to)
{
    /* FatFs will not replace an existing file */
    FILINFO fi;
    if (f_stat(to, &fi) == FR_OK && !(fi.fattrib & AM_DIR)) f_unlink(to);
    last = f_rename(from, to);
    return last == FR_OK;
}

void *vfs_opendir(const char *path)
{
    DIR *d;
    if (!mounted) { last = FR_NOT_READY; return 0; }
    d = (DIR *)malloc(sizeof *d);
    if (!d) return 0;
    last = f_opendir(d, path);
    if (last != FR_OK) { free(d); return 0; }
    return d;
}

int vfs_readdir(void *dp, vdirent *e)
{
    FILINFO fi;
    for (;;) {
        last = f_readdir((DIR *)dp, &fi);
        if (last != FR_OK || !fi.fname[0]) return 0;
        if (fi.fattrib & (AM_HID | AM_SYS)) continue;
        if (fi.fname[0] == '.') continue;
        snprintf(e->name, sizeof e->name, "%s", fi.fname);
        e->isdir = (fi.fattrib & AM_DIR) != 0;
        e->size = (uint32_t)fi.fsize;
        return 1;
    }
}

void vfs_closedir(void *dp)
{
    f_closedir((DIR *)dp);
    free(dp);
}

int vfs_space(uint32_t *total_kb, uint32_t *free_kb)
{
    FATFS *pfs;
    DWORD fre;
    if (!mounted) return 0;
    last = f_getfree("", &fre, &pfs);
    if (last != FR_OK) return 0;
    *total_kb = (uint32_t)(((uint64_t)(pfs->n_fatent - 2) * pfs->csize) / 2);
    *free_kb = (uint32_t)(((uint64_t)fre * pfs->csize) / 2);
    return 1;
}

const char *vfs_fstype(void)
{
    if (!mounted) return "-";
    switch (fs.fs_type) {
    case FS_FAT12: return "FAT12";
    case FS_FAT16: return "FAT16";
    case FS_FAT32: return "FAT32";
    case FS_EXFAT: return "exFAT";
    default: return "?";
    }
}

int vfs_label(char *buf, int n)
{
    char lab[24];
    if (!mounted || f_getlabel("", lab, 0) != FR_OK) { buf[0] = 0; return 0; }
    snprintf(buf, (size_t)n, "%s", lab);
    return 1;
}
