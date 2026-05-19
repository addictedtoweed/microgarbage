/* ============================================================
 *  trashdrive.h — RAM-backed block device (BYOFS)
 *
 *  A "trash drive" is a chunk of RAM that pretends to be a disk:
 *  fixed-size sectors, read and write by sector index. You bring
 *  your own filesystem — FatFs, Petit FatFs, or any block-device
 *  consumer — and mount it on top.
 *
 *  Sector size is fixed at TRASH_SECTOR_SIZE (512 bytes) to match
 *  what FatFs / Petit FatFs expect.
 *
 *  This module doesn't depend on any filesystem library. It just
 *  provides read_sectors / write_sectors / query primitives. The
 *  caller wires those into whatever filesystem's disk callbacks.
 *
 *  ---------------------------------------------------------------
 *  Typical wiring with FatFs (excerpt from your project's
 *  diskio.c — NOT part of this module):
 *
 *      static TrashDrive g_ram_drive;
 *
 *      DSTATUS disk_initialize(BYTE pdrv) {
 *          if (pdrv == DRIVE_RAM) return 0;   // already up
 *          return STA_NOINIT;
 *      }
 *
 *      DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sec, UINT n) {
 *          if (pdrv != DRIVE_RAM) return RES_PARERR;
 *          return trash_read(&g_ram_drive, buf, sec, n) == TRASH_OK
 *                 ? RES_OK : RES_ERROR;
 *      }
 *      // ... and similar for disk_write, disk_ioctl
 *
 *  Your application code then uses FatFs normally:
 *
 *      FATFS fs;
 *      f_mount(&fs, "1:", 1);
 *      FIL f;
 *      f_open(&f, "1:/scripts/foo.lua", FA_WRITE | FA_CREATE_ALWAYS);
 *      f_write(&f, code, code_len, &bw);
 *      f_close(&f);
 *
 *  ---------------------------------------------------------------
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef TRASHDRIVE_H
#define TRASHDRIVE_H

#include <stddef.h>
#include <stdint.h>

/* All trash drives use this sector size. Matches the FatFs /
 * Petit FatFs convention; changing it would break compatibility. */
#define TRASH_SECTOR_SIZE 512

/* Minimum region size for a usable drive. Below this, FatFs can't
 * format a filesystem. The actual practical minimum depends on the
 * filesystem (FAT12 needs more headroom than Petit FatFs which can
 * work with very small volumes). 16 KB is generous enough for any
 * of them. */
#define TRASH_MIN_BYTES (16 * 1024)

/* ============================================================
 *  Result codes
 * ============================================================ */

typedef enum {
    TRASH_OK = 0,
    TRASH_ERR_INVALID_ARG,     /* null pointer, etc                  */
    TRASH_ERR_OUT_OF_RANGE,    /* sector index >= sector_count       */
} TrashResult;

/* ============================================================
 *  Drive handle
 *
 *  Caller declares this directly (no allocator pattern — the
 *  drive is just a few pointers and counts). Initialize via
 *  trash_init.
 * ============================================================ */

typedef struct {
    uint8_t *region;          /* base of the RAM region           */
    size_t   region_bytes;    /* total size, in bytes             */
    size_t   sector_count;    /* region_bytes / TRASH_SECTOR_SIZE */
} TrashDrive;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Initialize a trash drive over the given RAM region.
 *
 *   td:           drive handle (caller-owned, will be filled in)
 *   region:       pointer to RAM region (must remain valid for life)
 *   region_bytes: size of the region in bytes; must be >= TRASH_MIN_BYTES
 *                 and a multiple of TRASH_SECTOR_SIZE
 *
 * The region's contents are NOT touched by init — neither cleared
 * nor formatted. The first time you mount a filesystem on the
 * drive, the filesystem library should be told to format it
 * (e.g., FatFs's f_mkfs).
 *
 * Returns TRASH_OK on success, TRASH_ERR_INVALID_ARG if the region
 * is null, too small, or not sector-aligned.
 *
 * Note: this is intentionally a "thin" init — the caller is in
 * full control of memory placement (typically a chunk of memory-
 * mapped PSRAM). If you want the contents zeroed on init, call
 * memset before or after. */
TrashResult trash_init(TrashDrive *td, void *region, size_t region_bytes);

/* Reset the drive — zero the region. Subsequent reads return 0
 * until something writes. */
void trash_clear(TrashDrive *td);

/* ============================================================
 *  Block I/O
 *
 *  These mirror the contract of FatFs's disk_read / disk_write:
 *  read or write `count` consecutive sectors starting at `sector`.
 *
 *  Returns TRASH_OK on success, TRASH_ERR_OUT_OF_RANGE if the
 *  request would go past the end of the drive.
 * ============================================================ */

TrashResult trash_read(const TrashDrive *td,
                        void *buf,
                        size_t sector,
                        size_t count);

TrashResult trash_write(TrashDrive *td,
                         const void *buf,
                         size_t sector,
                         size_t count);

/* ============================================================
 *  Introspection
 * ============================================================ */

/* Return sector count and sector size — useful for filling out
 * FatFs's disk_ioctl GET_SECTOR_COUNT and GET_SECTOR_SIZE. */
size_t trash_sector_count(const TrashDrive *td);
size_t trash_sector_size(const TrashDrive *td);
size_t trash_total_bytes(const TrashDrive *td);

#endif /* TRASHDRIVE_H */
