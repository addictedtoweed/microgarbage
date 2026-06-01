/* ============================================================
 *  copro_mg_state.c — shadow PPU state + frame builder.
 *  See copro_mg_state.h for the contract.
 *
 *  The build_frame walk does the per-frame work:
 *    1. For each dirty OAM/CGRAM/BG-tilemap range, allocate space
 *       in the cart window's payload area (bump pointer from 0).
 *    2. Copy the dirty bytes into that allocated slice.
 *    3. Write a CartDmaSlot descriptor into the cart window's DMA
 *       list with bbus / dmap / prep set for the destination, and
 *       src / size pointing at the staged bytes.
 *    4. Reset the dirty range.
 *    5. If sprite config (OBSEL) or BG mode/scroll changed, do the
 *       equivalent for the PPU register batch (a future TODO; for
 *       this slice we just track the values, the kernel.s side will
 *       read them out of the status mailbox area).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "copro_mg_state.h"

#include "cart_window.h"

#include <string.h>

/* The payload area in the cart window (offset 0 .. CW_OFF_JOY_BASE).
 * We bump-allocate within it each frame; reset on every build_frame
 * entry. ~28 KB is plenty for the OAM (~544 B) + CGRAM (~512 B) +
 * any incidental CHR/tilemap uploads accumulated across the frame. */
#define PAYLOAD_AREA_START  0x0000u
#define PAYLOAD_AREA_END    CW_OFF_JOY_BASE   /* 0x7000 */

/* SNES PPU B-bus addresses we emit. */
#define BBUS_OAMDATA   0x04   /* $2104 — write index then bytes        */
#define BBUS_VMDATAL   0x18   /* $2118/2119 — write VMADDR first       */
#define BBUS_CGDATA    0x22   /* $2122 — write CGADD index first       */

/* DMAP values (the $43x0 byte) we use. */
#define DMAP_1B_1R     0x00   /* 1 byte to 1 reg, increment src        */
#define DMAP_2B_2R     0x01   /* 2 bytes to 2 regs ($18+$19)           */

/* The singleton state. */
static MgState s_state;

/* Frame-time bump pointer + DMA slot count. Reset at the start of
 * mg_state_build_frame. */
static uint32_t s_payload_used;
static unsigned s_slot_used;

/* ----------------------------------------------------------------
 *  Lifecycle
 * ---------------------------------------------------------------- */

void mg_state_init(void) {
    memset(&s_state, 0, sizeof(s_state));
    mg_state_reset();
}

void mg_state_shutdown(void) {
    /* Nothing to release; state is static. */
}

void mg_state_reset(void) {
    /* OAM defaults: all sprites y=240 (offscreen). The cart-side
     * kernel will see "everything hidden" on first commit. */
    memset(s_state.oam_shadow, 0, sizeof(s_state.oam_shadow));
    for (unsigned i = 0; i < 128; i++) {
        s_state.oam_shadow[i * 4 + 1] = 240;
    }
    s_state.oam_dirty_lo   = 0;
    s_state.oam_dirty_hi   = MG_OAM_BYTES;   /* publish "everything hidden" */

    /* CGRAM defaults: all zero (black). */
    memset(s_state.cgram_shadow, 0, sizeof(s_state.cgram_shadow));
    s_state.cgram_dirty_lo = 0;
    s_state.cgram_dirty_hi = 0;

    /* BG layers default to disabled + zero tilemap. */
    for (unsigned i = 0; i < MG_BG_LAYERS; i++) {
        MgBgLayerState *bg = &s_state.bg[i];
        memset(bg->shadow, 0, sizeof(bg->shadow));
        bg->tilemap_word  = 0;
        bg->chr_word      = 0;
        bg->size_code     = 0;
        bg->enabled_main  = false;
        bg->enabled_sub   = false;
        bg->hofs = bg->vofs = 0;
        bg->dirty_lo = bg->dirty_hi = 0;
    }

    /* Sprite config defaults: smallest size pair, CHR base 0. */
    s_state.spr.sizes_code     = 0;
    s_state.spr.chr_base0_word = 0;
    s_state.spr.chr_base1_word = 0;

    s_state.bgmode = 0;
}

MgState *mg_state(void) {
    return &s_state;
}

/* ----------------------------------------------------------------
 *  Dirty-range helpers
 *
 *  Lo..hi is half-open: hi == lo means "no dirty bytes". Successive
 *  marks widen the range monotonically; the build_frame walk DMAs
 *  the full envelope per shadow buffer per frame.
 * ---------------------------------------------------------------- */

static inline void widen_range(uint16_t *lo, uint16_t *hi,
                               uint16_t new_lo, uint16_t new_hi) {
    if (*hi == *lo) {
        /* Clean — adopt the new range outright. */
        *lo = new_lo;
        *hi = new_hi;
        return;
    }
    if (new_lo < *lo) *lo = new_lo;
    if (new_hi > *hi) *hi = new_hi;
}

void mg_state_dirty_oam(uint16_t lo, uint16_t hi) {
    if (lo >= hi || hi > MG_OAM_BYTES) return;
    widen_range(&s_state.oam_dirty_lo, &s_state.oam_dirty_hi, lo, hi);
}

void mg_state_dirty_cgram(uint16_t lo, uint16_t hi) {
    if (lo >= hi || hi > MG_CGRAM_BYTES) return;
    widen_range(&s_state.cgram_dirty_lo, &s_state.cgram_dirty_hi, lo, hi);
}

void mg_state_dirty_bg(uint8_t layer, uint16_t lo, uint16_t hi) {
    if (layer >= MG_BG_LAYERS) return;
    if (lo >= hi || hi > MG_BG_TILEMAP_BYTES) return;
    widen_range(&s_state.bg[layer].dirty_lo,
                &s_state.bg[layer].dirty_hi, lo, hi);
}

/* ----------------------------------------------------------------
 *  Frame builder: shadow → cart window payload + DMA slot list
 * ---------------------------------------------------------------- */

/* Allocate `bytes` of contiguous space in the cart window payload
 * area. Returns the offset on success, or UINT32_MAX if no room. */
static uint32_t alloc_payload(uint32_t bytes) {
    if (s_payload_used + bytes > PAYLOAD_AREA_END - PAYLOAD_AREA_START) {
        return UINT32_MAX;
    }
    uint32_t off = PAYLOAD_AREA_START + s_payload_used;
    s_payload_used += bytes;
    return off;
}

/* Stage `src_bytes` of bytes into the payload area and queue a DMA
 * slot pointing at them. Returns true on success. */
static bool stage_dma(const void *src, uint32_t size,
                      uint8_t bbus, uint8_t dmap, uint16_t prep) {
    if (s_slot_used >= 8) return false;
    if (size == 0) return true;     /* nothing to do, success */

    uint32_t off = alloc_payload(size);
    if (off == UINT32_MAX) return false;

    cart_window_load_blob(off, src, size);

    CartDmaSlot slot = {
        .bbus = bbus,
        .dmap = dmap,
        .src  = (uint16_t)off,
        .size = (uint16_t)size,
        .prep = prep,
    };
    cart_window_set_dma_slot(s_slot_used++, &slot);
    return true;
}

/* Compose the OAM `prep` word: low byte = OAMADDL, high byte =
 * OAMADDH. Index is in 16-bit OAM words (256 internal addresses for
 * 512-byte main + 32 high). For our shadow which lives in linear
 * byte order, we DMA from byte 0 by writing OAMADDL=0, OAMADDH=0
 * (priority bit cleared) and letting the SNES auto-increment. */
static uint16_t oam_prep_word(uint16_t lo) {
    /* For now: always DMA from byte 0 of OAM regardless of dirty
     * range. The whole-shadow DMA is 544 bytes; one slot covers it.
     * Refinement to actually offset into OAM is a TODO when dirty-
     * range optimization matters (today the slot count is the
     * binding constraint, not the byte budget). */
    (void)lo;
    return 0x0000;   /* OAMADDL=0, OAMADDH=0 (low table, no priority) */
}

/* CGRAM prep word: CGADD value to write before DMA starts. CGADD is
 * a single byte — the starting palette entry. */
static uint16_t cgram_prep_word(uint16_t lo_bytes) {
    /* CGRAM is byte-addressed in our shadow but word-indexed for
     * CGADD (one CGADD step = 2 bytes). */
    uint8_t cgadd_idx = (uint8_t)(lo_bytes >> 1);
    return (uint16_t)cgadd_idx;
}

void mg_state_build_frame(void) {
    /* Reset per-frame bookkeeping. */
    s_payload_used = 0;
    s_slot_used    = 0;

    /* Clear any prior slots so a frame with no DMAs publishes a
     * cleanly-empty slot list. cart_window_set_dma_slot with bbus=0
     * means "empty slot" per the kernel-visible contract. */
    static const CartDmaSlot empty_slot = {0};
    for (unsigned i = 0; i < 8; i++) {
        cart_window_set_dma_slot(i, &empty_slot);
    }

    /* OAM. */
    if (s_state.oam_dirty_hi > s_state.oam_dirty_lo) {
        /* DMA the whole shadow (544 bytes) — simpler than partial
         * range and within slot budget for the common case. */
        (void)stage_dma(s_state.oam_shadow,
                        MG_OAM_BYTES,
                        BBUS_OAMDATA, DMAP_1B_1R,
                        oam_prep_word(s_state.oam_dirty_lo));
        s_state.oam_dirty_lo = s_state.oam_dirty_hi = 0;
    }

    /* CGRAM. */
    if (s_state.cgram_dirty_hi > s_state.cgram_dirty_lo) {
        uint16_t lo = s_state.cgram_dirty_lo;
        uint16_t hi = s_state.cgram_dirty_hi;
        const uint8_t *src = (const uint8_t *)s_state.cgram_shadow + lo;
        (void)stage_dma(src, (uint16_t)(hi - lo),
                        BBUS_CGDATA, DMAP_1B_1R,
                        cgram_prep_word(lo));
        s_state.cgram_dirty_lo = s_state.cgram_dirty_hi = 0;
    }

    /* BG tilemaps. Each layer's tilemap_word is the VRAM word
     * address; we DMA at VMAIN auto-increment + VMADDR = tilemap_word
     * + (dirty_lo / 2). The 2-byte-2-reg DMAP writes tile words. */
    for (unsigned i = 0; i < MG_BG_LAYERS; i++) {
        MgBgLayerState *bg = &s_state.bg[i];
        if (bg->dirty_hi <= bg->dirty_lo) continue;
        uint16_t lo = bg->dirty_lo;
        uint16_t hi = bg->dirty_hi;
        const uint8_t *src = bg->shadow + lo;
        /* prep word for VRAM: pack VMAIN (high byte) + VMADDR low.
         * For now: VMAIN = 0x80 (increment by 1 word after high write)
         * and the source carries the VMADDR offset; the kernel-side
         * sequence writes VMAIN, VMADDL, VMADDH from the prep + src
         * components. The exact prep format is a kernel-side
         * convention we'll lock when the BG path actually displays
         * something on real hardware. */
        (void)stage_dma(src, (uint16_t)(hi - lo),
                        BBUS_VMDATAL, DMAP_2B_2R,
                        (uint16_t)(bg->tilemap_word + (lo >> 1)));
        bg->dirty_lo = bg->dirty_hi = 0;
    }
}
