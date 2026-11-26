/* fatfs_host.c - runs vfs_fatfs.c + FatFs against a disk image, standing in
 * for the SD card, so the firmware's file layer is tested on the host.
 * usage: fatfs_host IMAGE   (the image is checked with fsck.vfat afterwards) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vfs.h"
#include "picocalc.h"

static FILE *img;
static uint32_t nsect;
int sd_init(void) { return img != 0; }
int sd_ready(void) { return img != 0; }
int sd_is_hc(void) { return 1; }
void sd_deinit(void) { }
void sd_reclock(void) { }
int sd_sync(void) { fflush(img); return 1; }
uint32_t sd_sectors(void) { return nsect; }
int sd_read_blocks(uint32_t lba, uint8_t *buf, int n)
{
    fseek(img, (long)lba * 512, SEEK_SET);
    return fread(buf, 512, (size_t)n, img) == (size_t)n;
}
int sd_write_blocks(uint32_t lba, const uint8_t *buf, int n)
{
    fseek(img, (long)lba * 512, SEEK_SET);
    return fwrite(buf, 512, (size_t)n, img) == (size_t)n;
}

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s (%s)\n", __LINE__, #c, vfs_error()); return 1; } } while (0)

int main(int argc, char **argv)
{
    char big[20000], back[20000];
    vfile *f;
    void *d;
    vdirent e;
    int i, n, isdir;
    uint32_t sz, tot, fr;
    img = fopen(argv[1], "r+b");
    fseek(img, 0, SEEK_END);
    nsect = (uint32_t)(ftell(img) / 512);
    CHECK(vfs_mount());
    printf("fs %s\n", vfs_fstype());
    CHECK(vfs_space(&tot, &fr) && tot > 0 && fr > 0);
    CHECK(vfs_mkdir("/calcide"));
    CHECK(vfs_mkdir("/calcide/examples"));
    CHECK(!vfs_mkdir("/calcide"));
    for (i = 0; i < (int)sizeof big; i++) big[i] = (char)(i * 7 + i / 13);
    CHECK(vfs_save("/calcide/examples/A long file name.c", big, sizeof big));
    CHECK(vfs_stat("/calcide/examples/A long file name.c", &isdir, &sz) && !isdir && sz == sizeof big);
    f = vfs_open("/calcide/examples/A long file name.c", "r");
    CHECK(f);
    CHECK(vfs_size(f) == (int)sizeof big);
    CHECK(vfs_read(f, back, 100) == 100);
    CHECK(vfs_seek(f, 1000, 0) == 0 && vfs_tell(f) == 1000);
    CHECK(vfs_read(f, back + 1000, sizeof big) == (int)sizeof big - 1000);
    CHECK(vfs_eof(f));
    vfs_close(f);
    CHECK(memcmp(big + 1000, back + 1000, sizeof big - 1000) == 0);
    f = vfs_open("/log.txt", "a");
    CHECK(f && vfs_write(f, "one\n", 4) == 4);
    vfs_close(f);
    f = vfs_open("/log.txt", "a");
    CHECK(f && vfs_write(f, "two\n", 4) == 4);
    vfs_close(f);
    {
        int len;
        char *t = vfs_load("/log.txt", &len, 0);
        CHECK(t && len == 8 && !strcmp(t, "one\ntwo\n"));
        free(t);
    }
    CHECK(vfs_rename("/log.txt", "/calcide/renamed.txt"));
    CHECK(!vfs_stat("/log.txt", 0, 0));
    CHECK(vfs_save("/calcide/x.txt", "x", 1));
    CHECK(vfs_rename("/calcide/x.txt", "/calcide/renamed.txt"));   /* replaces */
    n = 0;
    d = vfs_opendir("/calcide");
    CHECK(d);
    while (vfs_readdir(d, &e)) { printf("  %s%s %u\n", e.name, e.isdir ? "/" : "", (unsigned)e.size); n++; }
    vfs_closedir(d);
    CHECK(n == 2);
    CHECK(!vfs_remove("/calcide"));          /* not empty */
    CHECK(vfs_remove("/calcide/renamed.txt"));
    for (i = 0; i < 40; i++) {
        char name[64];
        snprintf(name, sizeof name, "/calcide/examples/file number %d.txt", i);
        CHECK(vfs_save(name, name, (int)strlen(name)));
    }
    n = 0;
    d = vfs_opendir("/calcide/examples");
    while (vfs_readdir(d, &e)) n++;
    vfs_closedir(d);
    CHECK(n == 41);
    CHECK(vfs_save("/CalcIDE.conf", "width = 64\n", 11));
    vfs_sync();
    vfs_unmount();
    CHECK(!vfs_mounted());
    CHECK(vfs_mount());
    CHECK(vfs_stat("/CalcIDE.conf", 0, &sz) && sz == 11);
    vfs_unmount();
    fclose(img);
    printf("fatfs ok\n");
    return 0;
}
