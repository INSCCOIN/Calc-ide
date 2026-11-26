/* vfs_common.c - current directory, path resolution and whole-file I/O. */
#include "vfs.h"
#include <string.h>
#include <stdlib.h>

char vfs_cwd[VFS_PATH] = "/";

/* Join with the current directory and fold "." and ".." components. */
int vfs_resolve(const char *in, char *out)
{
    char tmp[VFS_PATH * 2];
    char *parts[48];
    int np = 0, i;
    char *p, *s;
    if (!in) return 0;
    if (strncmp(in, "sd:", 3) == 0 || strncmp(in, "SD:", 3) == 0) in += 3;
    if (in[0] == '/' || in[0] == '\\') {
        if (strlen(in) >= sizeof tmp) return 0;
        strcpy(tmp, in);
    } else {
        if (strlen(vfs_cwd) + strlen(in) + 2 >= sizeof tmp) return 0;
        strcpy(tmp, vfs_cwd);
        strcat(tmp, "/");
        strcat(tmp, in);
    }
    for (p = tmp; *p; p++) if (*p == '\\') *p = '/';
    s = tmp;
    while (*s) {
        while (*s == '/') s++;
        if (!*s) break;
        p = s;
        while (*s && *s != '/') s++;
        if (*s) *s++ = 0;
        if (strcmp(p, ".") == 0) continue;
        if (strcmp(p, "..") == 0) { if (np > 0) np--; continue; }
        if (np >= 48) return 0;
        parts[np++] = p;
    }
    out[0] = 0;
    if (np == 0) { strcpy(out, "/"); return 1; }
    {
        size_t len = 0;
        for (i = 0; i < np; i++) {
            size_t l = strlen(parts[i]);
            if (len + l + 2 > VFS_PATH) return 0;
            out[len++] = '/';
            memcpy(out + len, parts[i], l);
            len += l;
        }
        out[len] = 0;
    }
    return 1;
}

const char *vfs_basename(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

char *vfs_load(const char *path, int *len, int extra)
{
    char full[VFS_PATH];
    vfile *f;
    int32_t sz;
    char *buf;
    int got;
    if (!vfs_resolve(path, full)) return 0;
    f = vfs_open(full, "r");
    if (!f) return 0;
    sz = vfs_size(f);
    if (sz < 0) { vfs_close(f); return 0; }
    buf = (char *)malloc((size_t)sz + 1 + (extra > 0 ? extra : 0));
    if (!buf) { vfs_close(f); return 0; }
    got = sz ? vfs_read(f, buf, (int)sz) : 0;
    vfs_close(f);
    if (got != sz) { free(buf); return 0; }
    buf[sz] = 0;
    if (len) *len = (int)sz;
    return buf;
}

int vfs_save(const char *path, const void *data, int len)
{
    char full[VFS_PATH];
    vfile *f;
    int put;
    if (!vfs_resolve(path, full)) return 0;
    f = vfs_open(full, "w");
    if (!f) return 0;
    put = len ? vfs_write(f, data, len) : 0;
    if (vfs_close(f) != 0) return 0;
    return put == len;
}
