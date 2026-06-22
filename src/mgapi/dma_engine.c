/* ============================================================
 *  dma_engine.c — virtualized DMA engine, desktop memcpy backend.
 *  See dma_engine.h. Public domain (CC0). No warranty.
 * ============================================================ */
#include "dma_engine.h"

#include <stdlib.h>
#include <string.h>

struct MgDmaEngine {
    MgDmaOps ops;
};

MgDmaEngine *mg_dma_create(const MgDmaOps *ops) {
    if (!ops || !ops->submit) return NULL;
    MgDmaEngine *e = (MgDmaEngine *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->ops = *ops;
    return e;
}

void mg_dma_destroy(MgDmaEngine *e) {
    free(e);
}

/* Clamp a transfer to what actually fits in both regions. For a fixed-
 * source fill the dst bounds the length (the src is repeated), so only
 * the dst capacity clamps; otherwise both src and dst availability do. */
static uint32_t clamp_len(const MgDmaXfer *x) {
    uint32_t d_avail = (x->dst_off < x->dst.size) ? (x->dst.size - x->dst_off) : 0u;
    uint32_t len = x->len;
    if (!(x->flags & MG_DMA_FIXED_SRC)) {
        uint32_t s_avail = (x->src_off < x->src.size) ? (x->src.size - x->src_off) : 0u;
        if (len > s_avail) len = s_avail;
    }
    if (len > d_avail) len = d_avail;
    return len;
}

uint32_t mg_dma_submit(MgDmaEngine *e, const MgDmaXfer *x) {
    if (!e || !x || !x->src.base || !x->dst.base) return 0;
    MgDmaXfer c = *x;
    c.len = clamp_len(x);
    if (c.len) e->ops.submit(e->ops.ctx, &c);
    return c.len;
}

uint32_t mg_dma_copy(MgDmaEngine *e,
                     MgDmaRegion dst, uint32_t dst_off,
                     MgDmaRegion src, uint32_t src_off,
                     uint32_t len) {
    MgDmaXfer x;
    x.src = src; x.src_off = src_off;
    x.dst = dst; x.dst_off = dst_off;
    x.len = len; x.flags = 0;
    return mg_dma_submit(e, &x);
}

void mg_dma_drain(MgDmaEngine *e) {
    if (e && e->ops.drain) e->ops.drain(e->ops.ctx);
}

/* ---- built-in synchronous memcpy backend ---- */

static void memcpy_submit(void *ctx, const MgDmaXfer *x) {
    (void)ctx;
    uint8_t       *d = (uint8_t *)x->dst.base + x->dst_off;
    const uint8_t *s = (const uint8_t *)x->src.base + x->src_off;
    if (x->flags & MG_DMA_FIXED_SRC) {
        uint32_t unit = (x->src_off < x->src.size) ? (x->src.size - x->src_off) : 1u;
        if (unit == 0u) unit = 1u;
        for (uint32_t i = 0; i < x->len; i++) d[i] = s[i % unit];
    } else {
        memcpy(d, s, x->len);
    }
}

static bool memcpy_done(void *ctx)  { (void)ctx; return true; }
static void memcpy_drain(void *ctx) { (void)ctx; }

void mg_dma_ops_memcpy(MgDmaOps *out) {
    if (!out) return;
    out->submit    = memcpy_submit;
    out->poll_done = memcpy_done;
    out->drain     = memcpy_drain;
    out->ctx       = NULL;
}
