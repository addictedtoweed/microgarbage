/* ============================================================
 *  trashdrive_fatfs.h — Bridge between trashdrive and FatFs
 *
 *  Lets you mount a FatFs volume on top of a `TrashDrive` (the
 *  RAM-backed block device in this library), giving you a full
 *  FAT12/16/32 filesystem inside a memory region you control.
 *
 *  Why this exists:
 *  ----------------
 *  trashdrive is a 512-byte-sector block device with no built-in
 *  filesystem; FatFs is a filesystem with no built-in block
 *  device. Wire them together and you get a real FAT volume
 *  with `f_open`, `f_read`, `f_write`, `f_mkdir`, `f_unlink`,
 *  and the rest of FatFs's API — backed by a chunk of RAM
 *  (PSRAM, external SRAM, a static buffer, whatever you like).
 *
 *  Typical use cases:
 *  - A scratchpad volume for transient files (logs, captures,
 *    work-in-progress data) without touching SD card flash.
 *  - A staging area: write to RAM first, then `f_copy` to SD
 *    when ready, eliminating SD wear during edit/save cycles.
 *  - Unit-testable filesystem code: the test harness uses a
 *    RAM volume; production code uses the same FatFs API but
 *    a different drive number for the real SD card.
 *
 *  How it works:
 *  -------------
 *  FatFs talks to block devices through a tiny C interface
 *  declared in `diskio.h` (disk_initialize, disk_read,
 *  disk_write, disk_ioctl, disk_status). This module provides
 *  those functions, dispatching by FatFs's "physical drive
 *  number" (pdrv) to whichever TrashDrive was registered for
 *  that slot.
 *
 *  Multiple trashdrives can coexist as long as FatFs is built
 *  with `FF_VOLUMES` >= N. This library's stock ffconf.h sets
 *  FF_VOLUMES = 1; bump it if you want more than one RAM volume.
 *
 *  Quick start:
 *  ------------
 *
 *      #include "storage/trashdrive.h"
 *      #include "storage/trashdrive_fatfs.h"
 *      #include "ff.h"
 *
 *      static uint8_t  g_pool[64 * 1024];
 *      static TrashDrive g_drive;
 *      static FATFS    g_fs;
 *
 *      // 1. Set up the block device.
 *      trash_init(&g_drive, g_pool, sizeof(g_pool));
 *
 *      // 2. Register it with FatFs as drive 0.
 *      trash_fatfs_register(0, &g_drive);
 *
 *      // 3. Format the volume (once — formatted volumes persist
 *      //    until the RAM is cleared).
 *      BYTE work[FF_MAX_SS];
 *      f_mkfs("0:", NULL, work, sizeof(work));
 *
 *      // 4. Mount and use.
 *      f_mount(&g_fs, "0:", 1);
 *
 *      FIL f;
 *      f_open(&f, "0:/hello.txt", FA_WRITE | FA_CREATE_ALWAYS);
 *      UINT bw;
 *      f_write(&f, "hello, world\n", 13, &bw);
 *      f_close(&f);
 *
 *  Notes on FatFs config (third_party/fatfs/ffconf.h):
 *  ----------------------------------------------------
 *  Our stock config has:
 *    FF_FS_READONLY = 0     (we want writes)
 *    FF_USE_MKFS    = 1     (host can format the volume)
 *    FF_USE_LFN     = 0     (8.3 filenames; saves flash + RAM)
 *    FF_FS_NORTC    = 1     (no real-time clock; files get a stub timestamp)
 *    FF_VOLUMES     = 1     (one mount point; bump for more)
 *    FF_TINY        = 0     (full-feature, better performance)
 *    FF_FS_REENTRANT = 0    (single-threaded host)
 *
 *  See third_party/fatfs/ffconf.h for the full set with comments.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_TRASHDRIVE_FATFS_H
#define MICROGARBAGE_TRASHDRIVE_FATFS_H

#include "storage/trashdrive.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of trashdrive-backed volumes we can register.
 * Must be at least FF_VOLUMES from ffconf.h; we don't include
 * ff.h here to avoid forcing every consumer to pull in FatFs. */
#ifndef TRASH_FATFS_MAX_VOLUMES
#define TRASH_FATFS_MAX_VOLUMES 4
#endif

/* Register a TrashDrive as FatFs's physical drive `pdrv`.
 *
 * Call this BEFORE any FatFs operation on that drive (f_mount,
 * f_mkfs, etc.). After registration, FatFs's disk_* functions
 * will route reads/writes for `pdrv` to this TrashDrive.
 *
 * Pass NULL as `drive` to unregister a previously-registered
 * slot.
 *
 * Returns true on success, false if:
 *   - pdrv >= TRASH_FATFS_MAX_VOLUMES
 *   - the trashdrive's region is too small for FatFs (FatFs
 *     wants at least a few KB; very small regions will fail
 *     at f_mkfs even if registration succeeds)
 *
 * The shim does not take ownership of the TrashDrive; the
 * caller is responsible for keeping it alive as long as it's
 * registered and for tearing it down when done.
 */
bool trash_fatfs_register(uint8_t pdrv, TrashDrive *drive);

/* Look up which TrashDrive (if any) is registered for `pdrv`.
 * Returns NULL if nothing is registered for that slot or pdrv
 * is out of range. Mainly useful for tests and diagnostics. */
TrashDrive *trash_fatfs_get(uint8_t pdrv);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_TRASHDRIVE_FATFS_H */
