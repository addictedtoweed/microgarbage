/* ============================================================
 *  cart_frame_types.h — shared per-frame DMA staging types.
 *
 *  Extracted from copro_mg_state.c so both the shadow-state staging
 *  path (copro_mg_state.c) and the host FMV pipeline (the generalized
 *  stream arbiter's FMV_VIDEO producer + the complete-frame ring) share
 *  ONE definition of a staged DMA slot, a sub-frame, and a complete
 *  cart-window-ready frame. Pulls in cart_window.h for the PPU batch /
 *  Mode-7 batch / offset constants the protocol uses.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_CART_FRAME_TYPES_H
#define MGAPI_CART_FRAME_TYPES_H

#include <stdint.h>

#include "cart_window.h"   /* PpuBatch, Mode7Batch, CW_OFF_HDMA_TABLES,
                            * CW_INIDISP_HDMA_BYTES */

/* Max staged DMA slots across all sub-frames of one logical frame
 * (8 slots × up to 6 sub-frames + margin). */
#define MG_MAX_STAGED_SLOTS   48u

/* Hard cap on sub-frames per logical frame. Bumped 4 → 8 (v2.11): the 4
 * cap silently dropped cgram + bg-tilemap shadow DMAs on the first commit
 * after mg_ppu_clean_slate when a demo also chunked CHR into 3 slots. */
#define MG_MAX_SUBFRAMES       8u

/* Per-burst DMA byte budget. Kept ≤ the kernel chainer's per-burst window
 * (54 lines × 170 B = 9180) so the host never packs a sub-frame the
 * chainer would have to split across two bursts (which would cost a 4th
 * burst and drop 20 fps → 15 fps). */
#define MG_SUBFRAME_BYTE_BUDGET 9180u

/* Cart-window per-frame payload area size (0x0000 .. CW_OFF_HDMA_TABLES).
 * Same value copro_mg_state.c calls HOST_PAYLOAD_BYTES; named distinctly
 * here so the two headers never collide. */
#define MG_FRAME_PAYLOAD_BYTES  ((uint32_t)CW_OFF_HDMA_TABLES)

/* One staged DMA descriptor (mirrors a cart-window DMA slot). */
typedef struct {
    uint8_t  bbus;
    uint8_t  dmap;
    uint16_t prep;
    uint16_t src;    /* offset into cart-window payload area */
    uint16_t size;
} StagedSlot;

/* One sub-frame: up to 8 slots the kernel chainer walks in one burst. */
typedef struct {
    uint8_t     slot_count;
    StagedSlot  slots[8];
} SubFrame;

/* A fully-built, cart-window-ready logical frame — the element type of the
 * FMV_VIDEO ring. The producer fills this several frames ahead; the
 * consumer copies `payload` into the cart window and publishes `ppu_batch`
 * + sub-frame 0 at FRAME_DONE. No mid-build at promotion. */
typedef struct {
    uint8_t    payload[MG_FRAME_PAYLOAD_BYTES]; /* bytes for cart window 0x0000.. */
    uint32_t   payload_used;
    SubFrame   subframes[MG_MAX_SUBFRAMES];
    unsigned   subframe_count;
    PpuBatch   ppu_batch;                        /* BG-base flip + scrolls */
    uint8_t    inidisp_hdma[CW_INIDISP_HDMA_BYTES];
    Mode7Batch mode7;                            /* parity; unused for FMV */
    /* Per-scanline H-blank VRAM siphon: the kernel writes `siphon_bytes`
     * (≤32) of CHR per visible scanline for `siphon_lines` lines, sourcing
     * from payload offset `siphon_src` into VRAM word `siphon_vram`. Delivers
     * the tile rows the bulk force-blank burst can't fit. siphon_bytes=0
     * disables it (the consumer then publishes a zeroed config). */
    uint8_t    siphon_bytes;
    uint8_t    siphon_lines;
    uint16_t   siphon_src;
    uint16_t   siphon_vram;
} MgCompleteFrame;

/* ---- shared PPU-base encoders (the gotcha-prone bits) ----
 * Used by both copro_mg_state.c (shadow path) and the FMV producer so the
 * BG-base encoding lives in exactly one place. */

/* BGxSC ($2107-$210A): bits 2-7 = tilemap base / 1024 WORDS (>>10), bits
 * 0-1 = size_code. (v1.78: must be word-units, not byte-units.) */
static inline uint8_t mg_bg_sc(uint16_t tilemap_word, uint8_t size_code) {
    return (uint8_t)(((tilemap_word >> 10) << 2) | (size_code & 3u));
}

/* BG CHR-base page for BG12NBA / BG34NBA: base / $1000 words (>>12). */
static inline uint8_t mg_chr_page(uint16_t chr_word) {
    return (uint8_t)((chr_word >> 12) & 0x0Fu);
}

#endif /* MGAPI_CART_FRAME_TYPES_H */
