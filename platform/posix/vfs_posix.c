/* vfs_posix.c - the host build's "SD card": a directory (./sdroot by
 * default, or $CALCIDE_ROOT). */
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

struct vfile { FILE *fp; };

static char root[512];
static int mounted;
static char lasterr[96];

static const char *host(const char *path, char *buf)
{
    snprintf(buf, 700, "%s%s", root, path);
    return buf;
}

static void seterr(void) { snprintf(lasterr, sizeof lasterr, "%s", strerror(errno)); }
const char *vfs_error(void) { return lasterr[0] ? lasterr : "error"; }

int vfs_mount(void)
{
    const char *r = getenv("CALCIDE_ROOT");
    struct stat st;
    snprintf(root, sizeof root, "%s", r && *r ? r : "sdroot");
    if (stat(root, &st) != 0) mkdir(root, 0755);
    mounted = stat(root, &st) == 0 && S_ISDIR(st.st_mode);
    return mounted;
}
int vfs_unmount(void) { mounted = 0; return 1; }
int vfs_mounted(void) { return mounted; }
int vfs_sync(void) { sync(); return 1; }

vfile *vfs_open(const char *path, const char *mode)
{
    char h[700], m[8];
    vfile *f;
    FILE *fp;
    if (!mounted) { snprintf(lasterr, sizeof lasterr, "no SD card"); return 0; }
    snprintf(m, sizeof m, "%sb", mode);
    if (strchr(mode, 'b')) snprintf(m, sizeof m, "%s", mode);
    fp = fopen(host(path, h), m);
    if (!fp) { seterr(); return 0; }
    {
        struct stat st;
        if (fstat(fileno(fp), &st) == 0 && S_ISDIR(st.st_mode)) { fclose(fp); snprintf(lasterr, sizeof lasterr, "is a directory"); return 0; }
    }
    f = (vfile *)malloc(sizeof *f);
    if (!f) { fclose(fp); return 0; }
    f->fp = fp;
    return f;
}

int vfs_read(vfile *f, void *buf, int n) { return (int)fread(buf, 1, (size_t)n, f->fp); }
int vfs_write(vfile *f, const void *buf, int n) { return (int)fwrite(buf, 1, (size_t)n, f->fp); }
int vfs_seek(vfile *f, int32_t off, int whence) { return fseek(f->fp, off, whence); }
int32_t vfs_tell(vfile *f) { return (int32_t)ftell(f->fp); }
int32_t vfs_size(vfile *f)
{
    struct stat st;
    fflush(f->fp);
    if (fstat(fileno(f->fp), &st) != 0) return -1;
    return (int32_t)st.st_size;
}
int vfs_eof(vfile *f) { return vfs_tell(f) >= vfs_size(f); }
int vfs_flush(vfile *f) { return fflush(f->fp); }
int vfs_close(vfile *f) { int r = fclose(f->fp); free(f); return r; }

int vfs_stat(const char *path, int *isdir, uint32_t *size)
{
    char h[700];
    struct stat st;
    if (!mounted || stat(host(path, h), &st) != 0) return 0;
    if (isdir) *isdir = S_ISDIR(st.st_mode);
    if (size) *size = (uint32_t)st.st_size;
    return 1;
}

int vfs_mkdir(const char *path) { char h[700]; if (mkdir(host(path, h), 0755) != 0) { seterr(); return 0; } return 1; }

int vfs_remove(const char *path)
{
    char h[700];
    struct stat st;
    if (stat(host(path, h), &st) != 0) { seterr(); return 0; }
    if (S_ISDIR(st.st_mode)) { if (rmdir(h) != 0) { seterr(); return 0; } return 1; }
    if (unlink(h) != 0) { seterr(); return 0; }
    return 1;
}

int vfs_rename(const char *from, const char *to)
{
    char a[700], b[700];
    if (rename(host(from, a), host(to, b)) != 0) { seterr(); return 0; }
    return 1;
}

void *vfs_opendir(const char *path)
{
    char h[700];
    DIR *d;
    if (!mounted) return 0;
    d = opendir(host(path, h));
    if (!d) { seterr(); return 0; }
    {
        /* remember the directory for stat() of entries */
        struct { DIR *d; char path[700]; } *w = malloc(sizeof *w);
        if (!w) { closedir(d); return 0; }
        w->d = d;
        snprintf(w->path, sizeof w->path, "%s", h);
        return w;
    }
}

int vfs_readdir(void *dp, vdirent *e)
{
    struct { DIR *d; char path[700]; } *w = dp;
    struct dirent *de;
    while ((de = readdir(w->d)) != 0) {
        char full[1400];
        struct stat st;
        if (de->d_name[0] == '.') continue;
        snprintf(e->name, sizeof e->name, "%.127s", de->d_name);
        snprintf(full, sizeof full, "%s/%s", w->path, de->d_name);
        if (stat(full, &st) != 0) continue;
        e->isdir = S_ISDIR(st.st_mode);
        e->size = (uint32_t)st.st_size;
        return 1;
    }
    return 0;
}

void vfs_closedir(void *dp)
{
    struct { DIR *d; char path[700]; } *w = dp;
    closedir(w->d);
    free(w);
}

int vfs_space(uint32_t *total_kb, uint32_t *free_kb)
{
    struct statvfs s;
    if (!mounted || statvfs(root, &s) != 0) return 0;
    *total_kb = (uint32_t)((unsigned long long)s.f_blocks * s.f_frsize / 1024);
    *free_kb = (uint32_t)((unsigned long long)s.f_bavail * s.f_frsize / 1024);
    return 1;
}

const char *vfs_fstype(void) { return "hostfs"; }
int vfs_label(char *buf, int n) { snprintf(buf, (size_t)n, "%s", root); return 1; }
