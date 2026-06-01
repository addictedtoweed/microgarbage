/* ============================================================
 *  l2_init.c — own the shared L2Alloc on the 3 MB PSRAM slice.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "l2_init.h"

#include "l2_alloc.h"

#include <errno.h>
#include <stddef.h>

static L2Alloc *g_l2;

int mgapi_l2_init(void *region, size_t region_size) {
    if (g_l2) return -EALREADY;
    g_l2 = l2_create(region, region_size);
    return g_l2 ? 0 : -ENOMEM;
}

void mgapi_l2_shutdown(void) {
    if (g_l2) {
        l2_destroy(g_l2);
        g_l2 = NULL;
    }
}

void *mgapi_l2_host_alloc(size_t size, size_t align) {
    return g_l2 ? l2_alloc(g_l2, size, align) : NULL;
}

void mgapi_l2_host_free(void *p) {
    if (g_l2) l2_free(g_l2, p);
}

int mgapi_l2_stats(MgapiL2Stats *out) {
    if (!out) return -EINVAL;
    if (!g_l2) return -EAGAIN;
    L2Stats s;
    l2_stats(g_l2, &s);
    out->used_bytes         = s.used_bytes;
    out->free_bytes         = s.free_bytes;
    out->alloc_count        = s.alloc_count;
    out->free_block_count   = s.free_block_count;
    out->largest_free_bytes = l2_largest_free(g_l2);
    return 0;
}
