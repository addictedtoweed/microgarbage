/* ============================================================
 *  trashdrive.c — implementation
 *  See storage/trashdrive.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "storage/trashdrive.h"
#include <string.h>

TrashResult trash_init(TrashDrive *td, void *region, size_t region_bytes) {
    if (!td || !region) return TRASH_ERR_INVALID_ARG;
    if (region_bytes < TRASH_MIN_BYTES) return TRASH_ERR_INVALID_ARG;
    if (region_bytes % TRASH_SECTOR_SIZE != 0) return TRASH_ERR_INVALID_ARG;

    td->region       = (uint8_t *)region;
    td->region_bytes = region_bytes;
    td->sector_count = region_bytes / TRASH_SECTOR_SIZE;
    /* Region contents are intentionally left untouched. The caller
     * decides whether to clear (call trash_clear) or let the
     * filesystem library format it. */
    return TRASH_OK;
}

void trash_clear(TrashDrive *td) {
    if (!td || !td->region) return;
    memset(td->region, 0, td->region_bytes);
}

TrashResult trash_read(const TrashDrive *td,
                        void *buf,
                        size_t sector,
                        size_t count) {
    if (!td || !buf) return TRASH_ERR_INVALID_ARG;
    if (sector >= td->sector_count) return TRASH_ERR_OUT_OF_RANGE;
    if (count == 0) return TRASH_OK;
    if (sector + count > td->sector_count) return TRASH_ERR_OUT_OF_RANGE;

    const uint8_t *src = td->region + sector * TRASH_SECTOR_SIZE;
    memcpy(buf, src, count * TRASH_SECTOR_SIZE);
    return TRASH_OK;
}

TrashResult trash_write(TrashDrive *td,
                         const void *buf,
                         size_t sector,
                         size_t count) {
    if (!td || !buf) return TRASH_ERR_INVALID_ARG;
    if (sector >= td->sector_count) return TRASH_ERR_OUT_OF_RANGE;
    if (count == 0) return TRASH_OK;
    if (sector + count > td->sector_count) return TRASH_ERR_OUT_OF_RANGE;

    uint8_t *dst = td->region + sector * TRASH_SECTOR_SIZE;
    memcpy(dst, buf, count * TRASH_SECTOR_SIZE);
    return TRASH_OK;
}

size_t trash_sector_count(const TrashDrive *td) {
    return td ? td->sector_count : 0;
}

size_t trash_sector_size(const TrashDrive *td) {
    (void)td;
    return TRASH_SECTOR_SIZE;
}

size_t trash_total_bytes(const TrashDrive *td) {
    return td ? td->region_bytes : 0;
}
