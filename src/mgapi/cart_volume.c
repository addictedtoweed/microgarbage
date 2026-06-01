/* ============================================================
 *  cart_volume.c — format + mount the /cart/ trashfs volume.
 *
 *  trashfs is memory-region based — no block-device layer, no
 *  malloc. We just hand it the PSRAM slice and call
 *  trashfs_format + trashfs_mount. The volume struct lives here as
 *  a singleton; stage 3 hands its address to vm_host_fs_mount_trashfs
 *  under the name "cart".
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "cart_volume.h"

#include "storage/trashfs.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

static TrashfsVolume g_vol;
static int           g_mounted;

int mgapi_cart_volume_init(void *region, size_t region_size) {
    if (g_mounted) return -EALREADY;
    if (!region || region_size == 0) return -EINVAL;

    /* Format the region: now=0 because the RTC isn't wired here, so
     * all timestamps start at 0 (matches the shell host's behavior;
     * see docs/trashfs-format.md). flags=0 selects the default
     * geometry. */
    TrashfsResult r = trashfs_format((uint8_t *)region,
                                     (uint32_t)region_size, 0, 0);
    if (r != TRASHFS_OK) return -EIO;

    r = trashfs_mount(&g_vol, (uint8_t *)region, (uint32_t)region_size);
    if (r != TRASHFS_OK) {
        memset(&g_vol, 0, sizeof g_vol);
        return -EIO;
    }
    g_mounted = 1;
    return 0;
}

void mgapi_cart_volume_shutdown(void) {
    /* No explicit umount API; volumes are pure references to caller-
     * owned memory. Drop our state. */
    memset(&g_vol, 0, sizeof g_vol);
    g_mounted = 0;
}

void *mgapi_cart_volume_handle(void) {
    return g_mounted ? &g_vol : NULL;
}

int mgapi_cart_volume_stats(CartVolumeStats *out) {
    if (!g_mounted) return -EAGAIN;
    if (!out) return -EINVAL;
    out->total_blocks = trashfs_total_blocks(&g_vol);
    out->free_blocks  = trashfs_free_blocks(&g_vol);
    out->inode_count  = trashfs_inode_count(&g_vol);
    out->free_inodes  = trashfs_free_inodes(&g_vol);
    return 0;
}
