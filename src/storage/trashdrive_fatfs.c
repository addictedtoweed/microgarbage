/* ============================================================
 *  trashdrive_fatfs.c — Implementation of FatFs disk_* functions
 *  on top of TrashDrive. See trashdrive_fatfs.h for the public
 *  contract.
 *
 *  This file IS the diskio layer that FatFs links against. FatFs
 *  ships an example diskio.c in source/, but we do NOT use that
 *  one — we use this file instead. Don't compile both.
 *
 *  Build dependencies (in addition to standard -Iinclude):
 *    -Ithird_party/fatfs/source        for ff.h, diskio.h
 *    -Ithird_party/fatfs               for ffconf.h
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "storage/trashdrive_fatfs.h"
#include "storage/trashdrive.h"

/* FatFs headers. Live under third_party/fatfs/. */
#include "ff.h"
#include "diskio.h"

/* ============================================================
 *  Registry
 *
 *  One TrashDrive pointer per physical drive number. NULL means
 *  "no drive registered for this slot". Static so the array is
 *  zero-initialized at program start.
 * ============================================================ */

static TrashDrive *g_drives[TRASH_FATFS_MAX_VOLUMES];

bool trash_fatfs_register(uint8_t pdrv, TrashDrive *drive) {
    if (pdrv >= TRASH_FATFS_MAX_VOLUMES) {
        return false;
    }
    /* drive == NULL is a valid unregister request. */
    g_drives[pdrv] = drive;
    return true;
}

TrashDrive *trash_fatfs_get(uint8_t pdrv) {
    if (pdrv >= TRASH_FATFS_MAX_VOLUMES) return NULL;
    return g_drives[pdrv];
}

/* ============================================================
 *  FatFs disk_* implementation
 *
 *  All five functions are called by FatFs's internal logic. We
 *  dispatch by pdrv to the registered TrashDrive, mapping
 *  TrashDrive's status/error codes to FatFs's.
 * ============================================================ */

/* disk_status — query whether the drive is initialized and
 * writable. Called by FatFs at the start of many operations.
 *
 * Returns a bitmask of STA_NOINIT (not initialized), STA_NODISK
 * (no media present), STA_PROTECT (write-protected). 0 means
 * "ready to use".
 *
 * Since trashdrive is initialized synchronously at registration
 * (you can't have a registered drive that's not ready), the only
 * status we can return is "ready" or "no disk". */
DSTATUS disk_status(BYTE pdrv) {
    if (pdrv >= TRASH_FATFS_MAX_VOLUMES) return STA_NODISK;
    if (g_drives[pdrv] == NULL)          return STA_NODISK;
    return 0;
}

/* disk_initialize — initialize the drive.
 *
 * In our world this is a no-op: the TrashDrive was already
 * initialized by the caller (via trash_init) before they
 * registered it. We just confirm it's there and return its
 * status. */
DSTATUS disk_initialize(BYTE pdrv) {
    return disk_status(pdrv);
}

/* disk_read — read `count` sectors starting at `sector` into
 * `buf`. Each sector is 512 bytes (matches trashdrive). */
DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sector, UINT count) {
    if (pdrv >= TRASH_FATFS_MAX_VOLUMES) return RES_PARERR;
    TrashDrive *d = g_drives[pdrv];
    if (!d) return RES_NOTRDY;
    if (!buf) return RES_PARERR;

    TrashResult r = trash_read(d, buf, (size_t)sector, (size_t)count);
    switch (r) {
        case TRASH_OK:                 return RES_OK;
        case TRASH_ERR_OUT_OF_RANGE:   return RES_PARERR;
        case TRASH_ERR_INVALID_ARG:    return RES_PARERR;
        default:                       return RES_ERROR;
    }
}

#if FF_FS_READONLY == 0

/* disk_write — write `count` sectors from `buf` starting at
 * `sector`. Only compiled when FatFs is built read-write. */
DRESULT disk_write(BYTE pdrv, const BYTE *buf, LBA_t sector, UINT count) {
    if (pdrv >= TRASH_FATFS_MAX_VOLUMES) return RES_PARERR;
    TrashDrive *d = g_drives[pdrv];
    if (!d) return RES_NOTRDY;
    if (!buf) return RES_PARERR;

    TrashResult r = trash_write(d, buf, (size_t)sector, (size_t)count);
    switch (r) {
        case TRASH_OK:                 return RES_OK;
        case TRASH_ERR_OUT_OF_RANGE:   return RES_PARERR;
        case TRASH_ERR_INVALID_ARG:    return RES_PARERR;
        default:                       return RES_ERROR;
    }
}

#endif /* FF_FS_READONLY */

/* disk_ioctl — control operations. FatFs queries this for a
 * handful of properties.
 *
 *   CTRL_SYNC          flush cached writes (no-op; we have no cache)
 *   GET_SECTOR_COUNT   total number of sectors → returns LBA_t
 *   GET_SECTOR_SIZE    bytes per sector → returns WORD
 *                      (only needed when FF_MAX_SS != FF_MIN_SS;
 *                       our config has both at 512 so FatFs may
 *                       skip this call)
 *   GET_BLOCK_SIZE     erase block size in sectors → returns DWORD
 *                      (RAM device has no erase blocks; 1 is the
 *                       smallest legal value)
 *   CTRL_TRIM          discard sectors (no-op for RAM; saves no
 *                      space and there's nothing to "trim")
 */
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buf) {
    if (pdrv >= TRASH_FATFS_MAX_VOLUMES) return RES_PARERR;
    TrashDrive *d = g_drives[pdrv];
    if (!d) return RES_NOTRDY;

    switch (cmd) {
        case CTRL_SYNC:
            /* Nothing to flush. trash_write is synchronous. */
            return RES_OK;

        case GET_SECTOR_COUNT:
            if (!buf) return RES_PARERR;
            *(LBA_t *)buf = (LBA_t)trash_sector_count(d);
            return RES_OK;

        case GET_SECTOR_SIZE:
            if (!buf) return RES_PARERR;
            *(WORD *)buf = (WORD)trash_sector_size(d);
            return RES_OK;

        case GET_BLOCK_SIZE:
            /* "Erase block" in sectors. RAM-backed device has no
             * physical erase blocks; reporting 1 means FatFs
             * won't try to do erase-block-aligned operations. */
            if (!buf) return RES_PARERR;
            *(DWORD *)buf = 1;
            return RES_OK;

#if FF_USE_TRIM
        case CTRL_TRIM:
            /* No-op: RAM-backed, no space saved by discarding. */
            return RES_OK;
#endif

        default:
            return RES_PARERR;
    }
}

/* ============================================================
 *  get_fattime — FatFs needs to stamp files with a timestamp.
 *
 *  When FF_FS_NORTC is set in ffconf.h (which our config does),
 *  FatFs provides a stub itself based on FF_NORTC_YEAR/MON/MDAY.
 *  When FF_FS_NORTC is NOT set, the application must provide
 *  this function.
 *
 *  We don't compile this here when FF_FS_NORTC is set — FatFs's
 *  internal stub handles it. If a future config flips that off,
 *  uncomment the block below and supply real time-of-day.
 * ============================================================ */

#if 0  /* enable if FF_FS_NORTC == 0 in ffconf.h */
DWORD get_fattime(void) {
    /* Format:
     *   bits 31:25  year origin 1980 (so 2025 = 45)
     *   bits 24:21  month (1-12)
     *   bits 20:16  day of month (1-31)
     *   bits 15:11  hour (0-23)
     *   bits 10:5   minute (0-59)
     *   bits  4:0   second / 2 (0-29)
     *
     * Application should fetch time from RTC or system clock and
     * encode it here. Stub returns "2025-01-01 00:00:00":
     */
    return ((DWORD)(2025 - 1980) << 25) | ((DWORD)1 << 21) | ((DWORD)1 << 16);
}
#endif
