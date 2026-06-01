/* ============================================================
 *  cart_volume.h — the /cart/ trashfs volume on the 1 MB PSRAM slice.
 *
 *  Internal to mgapi.dll / libmgapi.a. A separate mount from the
 *  existing /td0/ (which stays as the small AXI scratch volume on
 *  the MCU). The split mirrors the SNES cart paradigm: bulk
 *  read-mostly assets live in "ROM"-shaped storage; small writable
 *  scratch lives in "SRAM"-shaped storage.
 *
 *  Stage 2c brings the volume up but does NOT register it with the
 *  VM's fs mount table — that happens in stage 3 when the VM lands
 *  and vm_host_install_fs is called.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_CART_VOLUME_H
#define MGAPI_CART_VOLUME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Format + mount the volume on the given PSRAM region. region/size
 * are the `cart_trashfs` slice handed back by psram_pool_init.
 * Returns 0 on success, negative errno on failure. */
int  mgapi_cart_volume_init(void *region, size_t region_size);

/* Tear down. The PSRAM region is owned by psram_pool, not us, so we
 * just drop our reference. */
void mgapi_cart_volume_shutdown(void);

/* For stage-3 install_fs wiring: hand the caller the mounted volume
 * pointer so it can be registered as "cart" in the VM fs mount
 * table. Returns NULL if init hasn't run.
 *
 * The return type is `void *` so this header doesn't need to drag in
 * <storage/trashfs.h>. The caller casts to TrashfsVolume*. */
void *mgapi_cart_volume_handle(void);

/* Dev: volume stats for the host test (total/free blocks, inode
 * count). Returns 0 on success and fills `out`, or -EAGAIN if the
 * volume isn't mounted. */
typedef struct {
    uint32_t total_blocks;
    uint32_t free_blocks;
    uint32_t inode_count;
    uint32_t free_inodes;
} CartVolumeStats;
int mgapi_cart_volume_stats(CartVolumeStats *out);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_CART_VOLUME_H */
