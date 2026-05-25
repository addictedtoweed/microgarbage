/* ============================================================
 *  trashdrive.h — RAM-backed block device (BYOFS)
 *
 *  A "trash drive" is a chunk of RAM that pretends to be a disk:
 *  fixed-size sectors, read and write by sector index. You bring
 *  your own block-oriented filesystem and mount it on top via that
 *  filesystem's disk-IO callbacks.
 *
 *  Sector size is fixed at TRASH_SECTOR_SIZE (512 bytes), the
 *  conventional disk sector size that block filesystems expect.
 *
 *  This module doesn't depend on any filesystem library. It just
 *  provides read_sectors / write_sectors / query primitives. The
 *  caller wires those into whatever filesystem's disk callbacks.
 *
 *  ---------------------------------------------------------------
 *  Typical wiring (excerpt from your project's disk-IO glue —
 *  NOT part of this module):
 *
 *      static TrashDrive g_ram_drive;
 *
 *      int disk_read(uint8_t *buf, uint32_t sec, uint32_t n) {
 *          return trash_read(&g_ram_drive, buf, sec, n) == TRASH_OK
 *                 ? 0 : -1;
 *      }
 *      // ... and similar for disk_write
 *
 *  NOTE: the in-repo trashfs filesystem does NOT use this — it
 *  operates on a memory region directly. trashdrive is here for
 *  when you bring an external block-device-consuming filesystem.
 *
 *  ---------------------------------------------------------------
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef TRASHDRIVE_H
#define TRASHDRIVE_H

#include <stddef.h>
#include <stdint.h>

/* All trash drives use this sector size — the conventional disk
 * sector size; changing it would break compatibility with block
 * filesystems that assume 512. */
#define TRASH_SECTOR_SIZE 512

/* Minimum region size for a usable drive. Below this, most block
 * filesystems can't format a volume. The actual practical minimum
 * depends on the filesystem; 16 KB is generous enough for typical
 * small FAT-family volumes. */
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
 * drive, the filesystem should be told to format it.
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
 *  These mirror the conventional disk_read / disk_write contract:
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

/* Return sector count and sector size — useful for filling out a
 * filesystem's disk_ioctl GET_SECTOR_COUNT / GET_SECTOR_SIZE. */
size_t trash_sector_count(const TrashDrive *td);
size_t trash_sector_size(const TrashDrive *td);
size_t trash_total_bytes(const TrashDrive *td);

#endif /* TRASHDRIVE_H */
