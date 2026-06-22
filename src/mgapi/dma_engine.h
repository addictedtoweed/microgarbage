/* ============================================================
 *  dma_engine.h — virtualized DMA engine (region→region transfers)
 *
 *  Models the STM32 H745 DMA controller as explicit, named
 *  region→region transfers with src/dst MEMORY-SPACE shims, so the
 *  same call sites retarget from a desktop memcpy to the MCU's real
 *  DMA (HAL_DMA_Start) by swapping the MgDmaOps backend — no call-site
 *  changes. The point on the desktop is determinism + a single named
 *  seam: the host-side FMV pipeline produces complete frames into a
 *  PSRAM ring and the SNES-facing consumer does ONE PSRAM→cart-window
 *  transfer at FRAME_DONE, instead of building lazily at promotion.
 *
 *  Concurrency: an engine instance is single-context (one producer or
 *  one consumer thread). The memcpy backend is synchronous (submit
 *  performs the copy; poll_done is always true; drain is a no-op).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_DMA_ENGINE_H
#define MGAPI_DMA_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Memory space a transfer touches. On the desktop every space is host
 * RAM and `base` is a real pointer; on the H745 the space selects
 * DTCM / AXI-SRAM / QSPI-PSRAM / a peripheral FIFO and an MCU backend
 * keys cache maintenance + the right DMA stream off it. */
typedef enum {
    MG_DMA_MEM_HOST = 0,   /* generic host RAM / decode scratch        */
    MG_DMA_MEM_PSRAM,      /* L2 staging (QSPI PSRAM on the MCU)        */
    MG_DMA_MEM_DTCM,       /* fast tightly-coupled SRAM                 */
    MG_DMA_MEM_CARTWIN,    /* the SNES cart window (DTCM on the MCU)    */
} MgDmaSpace;

typedef struct {
    MgDmaSpace space;
    void      *base;       /* region base pointer (host address)        */
    uint32_t   size;       /* region size in bytes (for bounds clamp)   */
} MgDmaRegion;

/* Transfer flags. */
enum {
    /* Repeat the src bytes (src_off..src.size) to fill the whole dst —
     * the SNES fixed-source fill trick (e.g. a 2-byte zero source
     * clearing 64 KB of VRAM). Without it, a plain bounded copy. */
    MG_DMA_FIXED_SRC = 1u << 0,
};

typedef struct {
    MgDmaRegion src; uint32_t src_off;
    MgDmaRegion dst; uint32_t dst_off;
    uint32_t    len;
    uint8_t     flags;
} MgDmaXfer;

/* Pluggable transfer mechanism. submit performs/enqueues one transfer;
 * poll_done reports completion; drain blocks until idle. Desktop memcpy
 * backend is synchronous. */
typedef struct {
    void (*submit)(void *ctx, const MgDmaXfer *x);
    bool (*poll_done)(void *ctx);
    void (*drain)(void *ctx);
    void *ctx;
} MgDmaOps;

typedef struct MgDmaEngine MgDmaEngine;

/* Create an engine bound to a mechanism (ops is copied). NULL on bad
 * args or allocation failure. */
MgDmaEngine *mg_dma_create(const MgDmaOps *ops);
void         mg_dma_destroy(MgDmaEngine *eng);

/* Submit one transfer. Bounds-clamped to the smaller of len and the
 * region capacities from their offsets; returns the byte count that
 * will move (0 if nothing fits). */
uint32_t mg_dma_submit(MgDmaEngine *eng, const MgDmaXfer *x);

/* Convenience: a plain bounded region→region copy. */
uint32_t mg_dma_copy(MgDmaEngine *eng,
                     MgDmaRegion dst, uint32_t dst_off,
                     MgDmaRegion src, uint32_t src_off,
                     uint32_t len);

/* Block until all submitted transfers complete (no-op for memcpy). */
void mg_dma_drain(MgDmaEngine *eng);

/* Fill `out_ops` with the built-in synchronous memcpy mechanism
 * (desktop, and the MCU CPU-copy fallback). ctx is unused. */
void mg_dma_ops_memcpy(MgDmaOps *out_ops);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MGAPI_DMA_ENGINE_H */
