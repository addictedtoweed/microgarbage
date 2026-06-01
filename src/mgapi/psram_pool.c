/* ============================================================
 *  psram_pool.c — Windows implementation: one big malloc, three slices.
 *
 *  The MCU port replaces this file with one that points at the
 *  PSRAM section symbol (e.g., __psram_start) and never malloc's
 *  anything. The header + Carve constants stay identical.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "psram_pool.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Compile-time sanity: the carve must sum to the declared total. */
_Static_assert(MGAPI_PSRAM_CART_TRASHFS_BYTES +
               MGAPI_PSRAM_AUDIO_BYTES +
               MGAPI_PSRAM_L2_BYTES == MGAPI_PSRAM_TOTAL_BYTES,
               "psram carve must sum to MGAPI_PSRAM_TOTAL_BYTES");

static uint8_t    *g_block;        /* the whole 8 MB slab */
static PsramLayout g_layout;
static int         g_initialized;

int psram_pool_init(PsramLayout *out) {
    if (g_initialized) return -EALREADY;

    /* On Windows: one malloc. On the MCU: g_block = (uint8_t *)&__psram_start
     * and the malloc/free below are stubbed. The carve math is identical. */
    g_block = (uint8_t *)malloc(MGAPI_PSRAM_TOTAL_BYTES);
    if (!g_block) return -ENOMEM;

    /* Zero the whole slab so trashfs sees clean blocks, audio pool's
     * bitmap walks land on known state, and L2 alloc has predictable
     * free-list contents on first use. The cost is one big memset per
     * lifetime — negligible. */
    memset(g_block, 0, MGAPI_PSRAM_TOTAL_BYTES);

    g_layout.cart_trashfs       = g_block;
    g_layout.cart_trashfs_size  = MGAPI_PSRAM_CART_TRASHFS_BYTES;
    g_layout.audio              = g_block + MGAPI_PSRAM_CART_TRASHFS_BYTES;
    g_layout.audio_size         = MGAPI_PSRAM_AUDIO_BYTES;
    g_layout.l2                 = g_layout.audio + MGAPI_PSRAM_AUDIO_BYTES;
    g_layout.l2_size            = MGAPI_PSRAM_L2_BYTES;

    if (out) *out = g_layout;
    g_initialized = 1;
    return 0;
}

void psram_pool_shutdown(void) {
    if (!g_initialized) return;
    free(g_block);
    g_block = NULL;
    memset(&g_layout, 0, sizeof g_layout);
    g_initialized = 0;
}

const PsramLayout *psram_pool_layout(void) {
    return g_initialized ? &g_layout : NULL;
}
