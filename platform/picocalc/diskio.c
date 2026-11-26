/* diskio.c - FatFs media access on top of sd_spi.c. */
#include "ff.h"
#include "diskio.h"
#include "picocalc.h"

DSTATUS disk_status(BYTE pdrv) { return (pdrv == 0 && sd_ready()) ? 0 : STA_NOINIT; }

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0) return STA_NOINIT;
    return sd_init() ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !sd_ready()) return RES_NOTRDY;
    return sd_read_blocks((uint32_t)sector, buff, (int)count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0 || !sd_ready()) return RES_NOTRDY;
    return sd_write_blocks((uint32_t)sector, buff, (int)count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0 || !sd_ready()) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC: return sd_sync() ? RES_OK : RES_ERROR;
    case GET_SECTOR_COUNT: *(LBA_t *)buff = sd_sectors(); return sd_sectors() ? RES_OK : RES_ERROR;
    case GET_SECTOR_SIZE: *(WORD *)buff = 512; return RES_OK;
    case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
    default: return RES_PARERR;
    }
}

DWORD get_fattime(void)
{
    /* no real-time clock: 1 Jan 2026, 12:00 */
    return ((DWORD)(2026 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16) | ((DWORD)12 << 11);
}
