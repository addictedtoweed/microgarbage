/* ============================================================
 *  psram_pool.h — the 8 MB "PSRAM" block, carved three ways.
 *
 *  This module owns the single big slab that, on the MCU, is the
 *  QSPI PSRAM and, on Windows, is one malloc'd buffer of the same
 *  size. It hands out three named subregions to the rest of
 *  mgapi:
 *
 *      1 MB   /cart/ trashfs volume  (read-mostly bulk assets)
 *      4 MB   audio pool             (block-allocator, see audio_pool.h)
 *      3 MB   L2 alloc pool          (guest-visible alloc_l2/free_l2)
 *
 *  Other modules (audio, trashfs, l2_alloc) consume the slices but
 *  never own the backing memory. This separation is what lets the
 *  MCU port replace just this file: same Carve layout, different
 *  base address.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_PSRAM_POOL_H
#define MGAPI_PSRAM_POOL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The three carve sizes. Sum must equal the total; a static-assert
 * in the .c file enforces it. If you change the split, change the
 * memory entry `[[mgapi-cart-runtime]]` too — that file documents
 * the design choice. */
#define MGAPI_PSRAM_CART_TRASHFS_BYTES  (1u * 1024u * 1024u)
#define MGAPI_PSRAM_AUDIO_BYTES         (4u * 1024u * 1024u)
#define MGAPI_PSRAM_L2_BYTES            (3u * 1024u * 1024u)
#define MGAPI_PSRAM_TOTAL_BYTES         (MGAPI_PSRAM_CART_TRASHFS_BYTES + \
                                         MGAPI_PSRAM_AUDIO_BYTES + \
                                         MGAPI_PSRAM_L2_BYTES)

/* Layout the init returns. Pointers stay valid until psram_pool_shutdown.
 * Sizes are the constants above, repeated so a consumer doesn't need to
 * include this header to know what it has. */
typedef struct {
    uint8_t *cart_trashfs;
    size_t   cart_trashfs_size;
    uint8_t *audio;
    size_t   audio_size;
    uint8_t *l2;
    size_t   l2_size;
} PsramLayout;

/* Allocate (Windows) or claim (MCU) the 8 MB block and fill `out`
 * with the three subregion pointers. Returns 0 on success, negative
 * errno on failure (-ENOMEM if the block can't be obtained,
 * -EALREADY if already initialized).
 */
int  psram_pool_init(PsramLayout *out);

/* Release the block. Idempotent. After this, every subregion
 * pointer the caller still holds is dangling. */
void psram_pool_shutdown(void);

/* Diagnostic: a copy of the layout, valid only between init and
 * shutdown. Used by the dev test harness to confirm the carve. */
const PsramLayout *psram_pool_layout(void);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_PSRAM_POOL_H */
