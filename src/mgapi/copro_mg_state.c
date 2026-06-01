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

/* How many slots mg_state's shadow walker populated last frame. The
 * build_frame walker clears EXACTLY this many slots at the start of
 * the next walk before re-populating — slots beyond this index stay
 * untouched so a guest that uses the legacy SYS_COPRO_STAGE_DMA_SLOT
 * for higher-index slots keeps its entries across frames. */
static unsigned s_prev_mg_slots;

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

    s_state.force_blank_top    = 0;
    s_state.force_blank_bottom = 0;

    for (unsigned i = 0; i < 7; i++) {
        s_state.hdma[i].enabled   = false;
        s_state.hdma[i].bbad      = 0;
        s_state.hdma[i].dmap      = 0;
        s_state.hdma[i].table_off = 0;
    }
}

/* HDMA tables bump-allocator. Lives in CW_OFF_HDMA_TABLES..
 * CW_OFF_HDMA_TABLES+CW_HDMA_TABLES_BYTES, separate from the payload
 * area (which gets reset every frame; HDMA tables stay across frames
 * unless re-uploaded). Reset by mg_state_reset only. */
static uint16_t s_hdma_tables_used;

uint16_t mg_state_stage_hdma_table(const void *src, uint16_t len) {
    if (len == 0 || !src) return 0;
    if ((uint32_t)s_hdma_tables_used + len > CW_HDMA_TABLES_BYTES) {
        return UINT16_MAX;
    }
    uint16_t off = (uint16_t)(CW_OFF_HDMA_TABLES + s_hdma_tables_used);
    cart_window_load_blob(off, src, len);
    s_hdma_tables_used = (uint16_t)(s_hdma_tables_used + len);
    return off;
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

/* Public version of stage_dma for handlers like mg_chr_upload that
 * need to queue arbitrary DMAs outside the shadow flow. Returns
 * MG_R_ERR_DMA_* on overflow so the handler can propagate to the
 * guest's MgResult. */
uint8_t mg_state_slots_remaining(void) {
    return (uint8_t)(8u - s_slot_used);
}

/* Vblank byte budget (NTSC, joypad auto-read off): ~6479 baseline +
 * ~117 per force-blanked scanline. We round down to keep callers safe
 * even when the runtime's measurement of actual vblank length varies
 * by a few cycles. */
#define MG_BYTE_BUDGET_BASE   6479u
#define MG_BYTES_PER_FBLANK   117u

uint16_t mg_state_bytes_remaining(void) {
    uint32_t cap = MG_BYTE_BUDGET_BASE
                 + (uint32_t)s_state.force_blank_top    * MG_BYTES_PER_FBLANK
                 + (uint32_t)s_state.force_blank_bottom * MG_BYTES_PER_FBLANK;
    if (s_payload_used >= cap) return 0;
    return (uint16_t)(cap - s_payload_used);
}

int mg_state_queue_dma(const void *src, uint32_t size,
                       uint8_t bbus, uint8_t dmap, uint16_t prep) {
    if (s_slot_used >= 8) return -1;  /* MG_ERR_DMA_SLOTS */
    if (s_payload_used + size > (PAYLOAD_AREA_END - PAYLOAD_AREA_START)) {
        return -2;                    /* MG_ERR_DMA_BYTES */
    }
    if (!stage_dma(src, size, bbus, dmap, prep)) return -1;
    return 0;
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

/* Compute OBSEL ($2101) from sprite config. Layout:
 *   bits 5-7: sprite size code (the MgSpriteSizes enum value)
 *   bits 3-4: name gap NN — secondary CHR offset = (NN+1) * $1000 words
 *             above primary; we use chr_base1 - chr_base0 to derive
 *   bits 0-2: name base — primary CHR base / $2000 words
 */
static uint8_t compute_obsel(const MgSpriteConfig *cfg) {
    uint16_t base0 = cfg->chr_base0_word;
    uint16_t base1 = cfg->chr_base1_word;
    uint8_t  nb = (uint8_t)((base0 >> 13) & 0x07);   /* / $2000 words */
    uint8_t  gap = 0;
    if (base1 > base0) {
        uint16_t diff = (uint16_t)(base1 - base0);   /* word delta    */
        /* NN+1 = diff / $1000 words. */
        unsigned nn_plus_1 = (unsigned)(diff >> 12);
        if (nn_plus_1 > 0) gap = (uint8_t)((nn_plus_1 - 1) & 0x03);
    }
    return (uint8_t)((cfg->sizes_code << 5) | (gap << 3) | nb);
}

/* Compute BGxSC ($2107-$210A) from a layer's tilemap_word + size_code:
 *   bits 2-7: tilemap base / $0400 bytes = / $0200 words = word >> 9
 *   bits 0-1: size_code (MgBgSize) */
static uint8_t compute_bgxsc(const MgBgLayerState *bg) {
    return (uint8_t)(((bg->tilemap_word >> 9) << 2) | (bg->size_code & 3));
}

/* Compute the CHR-page index for BG12NBA / BG34NBA: chr_word >> 11
 * gives the 4 KB-page index (since 4 KB / 2 bytes-per-word = 2048
 * words). */
static uint8_t chr_page(const MgBgLayerState *bg) {
    return (uint8_t)((bg->chr_word >> 11) & 0x0F);
}

static void emit_ppu_batch(void) {
    const MgState *s = &s_state;
    PpuBatch b = {0};

    b.bgmode  = s->bgmode;
    b.obsel   = compute_obsel(&s->spr);
    b.bg1sc   = compute_bgxsc(&s->bg[0]);
    b.bg2sc   = compute_bgxsc(&s->bg[1]);
    b.bg3sc   = compute_bgxsc(&s->bg[2]);
    b.bg4sc   = compute_bgxsc(&s->bg[3]);
    b.bg12nba = (uint8_t)((chr_page(&s->bg[1]) << 4) | chr_page(&s->bg[0]));
    b.bg34nba = (uint8_t)((chr_page(&s->bg[3]) << 4) | chr_page(&s->bg[2]));

    /* TM / TS: per-layer main/sub bits + sprites always enabled. */
    uint8_t tm = 0, ts = 0;
    for (unsigned i = 0; i < MG_BG_LAYERS; i++) {
        if (s->bg[i].enabled_main) tm |= (uint8_t)(1u << i);
        if (s->bg[i].enabled_sub)  ts |= (uint8_t)(1u << i);
    }
    tm |= 0x10;   /* bit 4: sprite layer always on */
    b.tm = tm;
    b.ts = ts;

    /* MOSAIC stub — handler doesn't track yet. */
    b.mosaic = 0;

    /* Scrolls. The kernel writes them low-byte then high-byte to the
     * write-twice PPU register. */
    b.bg1hofs = (uint16_t)s->bg[0].hofs;
    b.bg1vofs = (uint16_t)s->bg[0].vofs;
    b.bg2hofs = (uint16_t)s->bg[1].hofs;
    b.bg2vofs = (uint16_t)s->bg[1].vofs;
    b.bg3hofs = (uint16_t)s->bg[2].hofs;
    b.bg3vofs = (uint16_t)s->bg[2].vofs;
    b.bg4hofs = (uint16_t)s->bg[3].hofs;
    b.bg4vofs = (uint16_t)s->bg[3].vofs;

    cart_window_set_ppu_batch(&b);
}

/* Build the INIDISP HDMA table for the current force_blank_top /
 * bottom values, write it into the cart window at CW_OFF_INIDISP_HDMA.
 * The kernel-reserved HDMA channel 7 reads from there each scanline.
 *
 * Mode-0 repeat segment format: [0x80 | line_count][value]
 * Terminator: 0x00
 * line_count is bits 0-6 of the count byte; 1..127 valid (0 means
 * "128 lines" with special handling we avoid by splitting). */
static void emit_inidisp_table(void) {
    uint8_t buf[CW_INIDISP_HDMA_BYTES] = {0};
    uint8_t *p = buf;
    uint8_t  t = s_state.force_blank_top;
    uint8_t  b = s_state.force_blank_bottom;

    int visible = 224 - (int)t - (int)b;
    if (visible < 0) visible = 0;

    if (t) {
        *p++ = (uint8_t)(0x80u | t);   /* repeat for t lines */
        *p++ = 0x80;                   /* INIDISP = force-blank */
    }
    while (visible > 0) {
        int n = visible > 127 ? 127 : visible;
        *p++ = (uint8_t)(0x80u | (uint8_t)n);
        *p++ = 0x0F;                   /* INIDISP = visible, full brightness */
        visible -= n;
    }
    if (b) {
        *p++ = (uint8_t)(0x80u | b);
        *p++ = 0x80;
    }
    *p++ = 0x00;                       /* terminator */

    cart_window_load_blob(CW_OFF_INIDISP_HDMA, buf,
                          (uint32_t)(p - buf));
}

void mg_state_build_frame(void) {
    /* Reset per-frame bookkeeping. */
    s_payload_used = 0;
    s_slot_used    = 0;

    /* Clear ONLY the slot indices mg_* used last frame. Slots beyond
     * that are left alone so guests that mix SYS_COPRO_STAGE_DMA_SLOT
     * with the mg_* API keep their explicit entries across frames. */
    static const CartDmaSlot empty_slot = {0};
    for (unsigned i = 0; i < s_prev_mg_slots && i < 8; i++) {
        cart_window_set_dma_slot(i, &empty_slot);
    }

    /* Always emit the PPU register batch — it's only 32 bytes and the
     * kernel will read whatever is staged regardless. Static-snapshot
     * of all the things mg_bg_mode / mg_bg_setup / mg_bg_enable /
     * mg_bg_scroll / mg_sprite_sizes / mg_sprite_chr_base have
     * accumulated. */
    emit_ppu_batch();

    /* And the INIDISP HDMA table for the force-blank window. */
    emit_inidisp_table();

    /* HDMA control table for channels 0..6. The kernel walks this 56-
     * byte area at vblank, configures DMAP/BBAD/A1T/A1B for each
     * enabled channel, and computes HDMAEN. Channel 7 is reserved for
     * the INIDISP letterbox above; the kernel ORs its bit in. */
    {
        uint8_t cfg[CW_HDMA_CONFIG_BYTES] = {0};
        for (unsigned c = 0; c < 7; c++) {
            uint8_t *p = cfg + c * CW_HDMA_CFG_BYTES_EACH;
            p[0] = s_state.hdma[c].enabled ? 1 : 0;
            p[1] = s_state.hdma[c].bbad;
            p[2] = s_state.hdma[c].dmap;
            /* p[3] reserved */
            p[4] = (uint8_t)(s_state.hdma[c].table_off & 0xFF);
            p[5] = (uint8_t)(s_state.hdma[c].table_off >> 8);
            /* p[6..7] reserved */
        }
        cart_window_load_blob(CW_OFF_HDMA_CONFIG, cfg, sizeof(cfg));
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

    /* Remember how many slots we used so the next frame's clear can
     * be precise instead of stomping the whole list. */
    s_prev_mg_slots = s_slot_used;
}
