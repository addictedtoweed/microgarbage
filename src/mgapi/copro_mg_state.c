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

#include <stdio.h>
#include <string.h>

/* The payload area in the cart window (offset 0 .. CW_OFF_JOY_BASE).
 * We bump-allocate within it each frame; reset on every build_frame
 * entry. ~28 KB is plenty for the OAM (~544 B) + CGRAM (~512 B) +
 * any incidental CHR/tilemap uploads accumulated across the frame. */
#define PAYLOAD_AREA_START  0x0000u
#define PAYLOAD_AREA_END    CW_OFF_HDMA_TABLES   /* 0x7000 — payload ends
                                                  * where HDMA tables begin.
                                                  * v2.18 BISECT: reverted from
                                                  * CW_OFF_NMI_CODE to isolate
                                                  * whether mode7_3d's crash and
                                                  * audio_mixer's flicker were
                                                  * payload-size regressions. */

/* SNES PPU B-bus addresses we emit. */
#define BBUS_OAMDATA   0x04   /* $2104 — write index then bytes        */
#define BBUS_VMDATAL   0x18   /* $2118/2119 — write VMADDR first       */
#define BBUS_CGDATA    0x22   /* $2122 — write CGADD index first       */

/* DMAP values (the $43x0 byte) we use. */
#define DMAP_1B_1R     0x00   /* 1 byte to 1 reg, increment src        */
#define DMAP_2B_2R     0x01   /* 2 bytes to 2 regs ($18+$19)           */

/* The singleton state. */
static MgState s_state;

/* Bump pointer + DMA slot count for the cart-window payload + DMA
 * list. The "checkpoint" pair is what build_frame resets to instead
 * of zero -- mg_chr_upload (and any other "persistent" stage that
 * uploads bytes once and expects them to survive across frames)
 * advances the checkpoint so its bytes + slot don't get overwritten
 * by the shadow-state emissions on the next commit. */
static uint32_t s_payload_used;
static unsigned s_slot_used;
static uint32_t s_payload_checkpoint;   /* persistent direct staging end */
static unsigned s_slot_checkpoint;      /* persistent direct slot count  */

/* How many slots mg_state's shadow walker populated last frame. The
 * build_frame walker clears EXACTLY this many slots at the start of
 * the next walk before re-populating — slots beyond this index stay
 * untouched so a guest that uses the legacy SYS_COPRO_STAGE_DMA_SLOT
 * for higher-index slots keeps its entries across frames. */
static unsigned s_prev_mg_slots;

/* v2.05: host-side sub-frame chaining.
 *
 * For demos like the FMV player that need more per-frame DMA than
 * fits in one SNES NMI's vblank budget (~9 KB), the runtime splits
 * the slot list into multiple "sub-frames." The cart window's 64-byte
 * slot-list area gets rotated between sub-frames per NMI tick (port-7
 * mailbox read advances to the next sub-frame). The kernel doesn't
 * know about this — it still walks 8 slots per NMI, just sees a
 * different slot list each time. frame_consumed bumps only when the
 * last sub-frame has been processed.
 *
 * Slots accumulate in s_staged here (via stage_dma) instead of
 * writing to cart_window directly. The split + first-sub-frame-write
 * happens at the end of build_frame; subsequent sub-frame writes
 * happen in mg_state_advance_subframe (called from cart_window's
 * port-7 read handler). */
#define MG_MAX_STAGED_SLOTS   48u   /* 8 slots × up to 6 sub-frames + margin */
/* MG_MAX_SUBFRAMES bumped from 4 → 8 (v2.11). The 4 cap silently
 * dropped cgram + bg-tilemap shadow DMAs on the first commit after
 * mg_ppu_clean_slate when a demo also chunked CHR into 3 slots —
 * SF0 spent on the 65536-B clean_slate VRAM-clear budget, SF1-3 on
 * the three CHR chunks, leaving no room for SF4 (cgram + tilemap).
 * Multi-commit demos hid this via s_cgram_reupload_frames /
 * s_bg_reupload_frames widening dirty for 3 / 60 follow-ups; single-
 * commit demos (demo_fmv_still) failed to display anything.
 * 8 covers 2× the worst-case + headroom; ~80 B extra BSS. */
#define MG_MAX_SUBFRAMES       8u   /* hard cap */
/* Per-NMI DMA byte budget. Per the FMV design:
 *   (vblank_lines + force_blanked_lines) × 1364 cycles / 8 = bytes/NMI
 *   = (38 + 16) × 1364 / 8 = 9207 with force_blank(8, 8).
 * Round down a touch so callers' uploads don't bump right against
 * the wall. Sub-frame packing keeps total bytes ≤ this per group. */
#define MG_SUBFRAME_BYTE_BUDGET 9200u

typedef struct {
    uint8_t  bbus;
    uint8_t  dmap;
    uint16_t prep;
    uint16_t src;    /* offset into cart-window payload area */
    uint16_t size;
} StagedSlot;

typedef struct {
    uint8_t     slot_count;
    StagedSlot  slots[8];
} SubFrame;

static StagedSlot s_staged[MG_MAX_STAGED_SLOTS];
static unsigned   s_staged_count;
static unsigned   s_staged_checkpoint;   /* persistent prefix end */

static SubFrame s_subframes[MG_MAX_SUBFRAMES];
static unsigned s_subframe_count;
static unsigned s_subframe_index;

/* Clean-slate VRAM-clear arming state. Set by
 * mg_state_arm_clean_slate_vram_clear (called from h_ppu_clean_slate)
 * and consumed by mg_state_build_frame the frame after the kernel
 * acks the staged frame containing the clear. See
 * mg_state_arm_clean_slate_vram_clear's comment for the full protocol. */
static uint32_t s_clean_slate_drop_at_consumed;
static bool     s_clean_slate_pending;

/* v2.20: clean_slate full-frame force-blank window, gated on
 * cart_window_frame_consumed(). While consumed has not caught up to
 * s_clean_slate_force_blank_until_consumed, emit_inidisp_table emits
 * a 224-line $80 table instead of the normal letterbox shape — the
 * PPU stays force-blanked for the entire frame (vblank + active
 * display) so the 64 KB VRAM clear at slot 0 and the demo's initial
 * CGRAM/CHR/tilemap/OAM all land regardless of the kernel's slot-walk
 * timing.
 *
 * Why consumed-based instead of a build_frame counter: the 64 KB
 * clear takes ~32 ms (~2 NTSC frames) of CPU-paused DMA. During that
 * span, NMI is held off and consumed doesn't advance. Host build_frame
 * keeps being called (~60 Hz) but each call corresponds to a guest
 * commit, NOT to an actually-processed frame. A naive per-build_frame
 * decrement raced ahead of the real NMI cycle and lifted force-blank
 * before VRAM finished updating, producing the "font ghost" artifact.
 * Gating on consumed advance means the window lasts however long the
 * SNES actually needs.
 *
 * Threshold = 4 consumed advances: covers the clear (1 NMI) + 3
 * follow-up NMIs for demos that stage CHR over multiple frames
 * (FMV-style chunked uploads). Visible cost ≈ 67 ms of black between
 * demos. */
static uint32_t s_clean_slate_force_blank_until_consumed;
static bool     s_clean_slate_force_blank_active;

/* CGRAM re-upload counter, decremented by mg_state_build_frame. While
 * positive, build_frame forces cgram_dirty=full so the CGRAM DMA is
 * re-staged even if the guest hasn't touched the palette. Needed
 * because the 24ms VRAM-clear DMA at slot 0 on the clean_slate frame
 * appears to leave the SNES bus in a state that makes the slot-2
 * CGRAM DMA on the same frame land short (CGRAM[0] stays at boot 0);
 * re-staging on the next 2-3 frames (when slot 0 has been dropped and
 * CGRAM moves earlier in the slot list) recovers cleanly. */
static unsigned s_cgram_reupload_frames;

/* Same idea, for BG tilemaps. Reintroduced in v1.84 after the audio_mixer
 * "text rows 2/5/12/14/17 missing while rows 7/9 visible" pattern came
 * back the moment we actually ran the BG1SC-fixed v1.83 DLL through the
 * real kernel runtime (previous test runs accidentally fell through to
 * the smoke ROM's 65816 menu and looked correct for the wrong reason).
 *
 * The root cause is still timing: clean_slate's 64KB VRAM-clear DMA at
 * slot 0 takes ~24 ms — about 1.5 NTSC frames — which means the slot-5
 * BG-tilemap DMA on the first frame fires while the kernel is well past
 * vblank. v1.76's INIDISP=$80 force-blank wrapper around the slot walk
 * is supposed to let those writes land regardless of vcounter, but in
 * practice bsnes-plus only honors part of the range. Three frames of
 * re-emission cover the recovery window — by frame 2 the persistent
 * slot 0 has been dropped, vblank is plenty, and the BG-tilemap DMA
 * lands cleanly. */
static unsigned s_bg_reupload_frames;

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

    /* CGRAM defaults: all zero (black). Mark the WHOLE range dirty so
     * the next commit DMAs all 512 zero bytes into the PPU's CGRAM,
     * clearing leftover palette entries from the previous demo. Without
     * this the new demo's mg_palette_set_rgb calls only dirty the
     * specific entries it touches; CGRAM[1..N] for any N the new demo
     * doesn't explicitly set keeps the old demo's values (visible
     * symptom: red strip at top after palette.elf → letterbox.elf →
     * sprite.elf, where leftover CGRAM[1] from letterbox.elf shows
     * through BG1 rendering tile-0 from leftover VRAM). */
    memset(s_state.cgram_shadow, 0, sizeof(s_state.cgram_shadow));
    s_state.cgram_dirty_lo = 0;
    s_state.cgram_dirty_hi = sizeof(s_state.cgram_shadow);

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

    /* Mode 7 identity matrix + zero center + wrap behavior. */
    s_state.m7a   = 256;   /* 1.0 in 8.8 */
    s_state.m7b   = 0;
    s_state.m7c   = 0;
    s_state.m7d   = 256;
    s_state.m7cx  = 0;
    s_state.m7cy  = 0;
    s_state.m7sel = 0;

    /* Reset payload + slot bump pointers AND clear all 8 cart_window
     * slots to empty. Without this, slot data from the previous demo's
     * last commit persists in cart_window even after mg_state shadow
     * reset; the next demo's mg_chr_upload allocates from the stale
     * s_slot_used value (e.g., slot 1 if the previous demo had 1 slot
     * used), so its CHR goes to slot 1 not slot 0 — and the previous
     * demo's slot 0 CGRAM-red DMA keeps firing every NMI, ahead of the
     * new demo's CGRAM-zero DMA at a higher slot. Visible symptom:
     * palette.elf → letterbox.elf shows CGRAM[0] = red instead of the
     * black letterbox.elf wants. Clearing slots here ensures the next
     * demo starts with all-empty cart_window slots regardless of what
     * the previous demo's bump pointers happened to be. */
    s_payload_used = 0;
    s_slot_used    = 0;
    {
        static const CartDmaSlot empty_slot = {0};
        for (unsigned i = 0; i < 8; i++) {
            cart_window_set_dma_slot(i, &empty_slot);
        }
    }
    s_prev_mg_slots = 0;

    /* Persistent-staging checkpoint -- starts at zero so the first
     * mg_chr_upload (or similar) allocates from payload offset 0.
     * Each direct stage advances it; build_frame rewinds to here
     * instead of zero so persistent uploads survive. Reset on VM
     * unload / new game spawn so the next demo starts clean. */
    s_payload_checkpoint = 0;
    s_slot_checkpoint    = 0;

    /* v2.25: the staged-DMA queue (s_staged + s_subframes) carries
     * persistent slot descriptors across build_frame calls within a
     * single demo's run. Reset on demo teardown so the next demo's
     * build_frame doesn't rewind into the previous demo's leftover
     * descriptors — those would still carry the old bbus/dmap/prep
     * values but the src offsets would now point at the new demo's
     * payload bytes, dispatching wrong content to wrong PPU registers.
     * Visible symptom: demo_audio_mixer's CGRAM[1] came up black on
     * second run after another demo; demo_sprite showed garbage CHR
     * tiles after similar transitions.
     *
     * s_pending_* (FMV transient queue) and s_hdma_tables_used live
     * later in the file; they're either reset per-frame by build_frame
     * (HDMA tables) or only touched by demos that aren't currently
     * affected (pending transients = FMV). Skip them here to keep this
     * function ordering correct; revisit if FMV cross-demo runs
     * regress. */
    s_staged_count = 0;
    s_staged_checkpoint = 0;
    s_subframe_count = 0;
    s_subframe_index = 0;

    /* Drop any in-flight clean-slate VRAM-clear arming from a
     * previous demo. If this reset is being called by h_ppu_clean_slate
     * itself, the handler will re-arm via mg_state_arm_clean_slate_*
     * after we return. */
    s_clean_slate_pending = false;
    s_clean_slate_drop_at_consumed = 0;
    s_cgram_reupload_frames = 0;
    s_bg_reupload_frames    = 0;
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

/* Drop all per-frame HDMA-table staging — used by the early-return
 * path in h_frame_commit when the SNES kernel hasn't yet acked the
 * previous frame. Without this, mg_hdma_upload_table calls from
 * subsequent iterations of a tight commit-cancelled loop accumulate
 * into the pool and overflow CW_HDMA_TABLES_BYTES on iteration N+1
 * even though each iteration's tables would fit on their own.
 *
 * Safe because the stored hdma[].table_off pointers stay unchanged
 * across the next iteration's re-uploads — the new tables overwrite
 * the old ones at the same offsets the kernel reads from. */
void mg_state_drop_hdma_tables(void) {
    s_hdma_tables_used = 0;
}

/* Forward decl — alloc_payload's full definition is further down in
 * the file (it logically belongs with the stage_dma machinery), but
 * we need it here for mg_state_stage_vram_clear. */
static uint32_t alloc_payload(uint32_t bytes);

/* Full-VRAM clear via SNES fixed-source DMA trick. See header comment
 * for the protocol; here we set up the slot fields directly because:
 *   - we need DMAP bit 4 (fixed source) — stage_dma doesn't expose this
 *   - we want slot.size = $0000, which SNES DAS interprets as 65536-
 *     byte transfer (= 32K word writes = full VRAM). stage_dma's size==0
 *     guard would short-circuit, so we bypass it.
 *
 * The 2-byte zero source sits in the regular payload area; the fixed-
 * source mode means the SNES reads the SAME 2 bytes 65536 times, never
 * advancing past them. Cheap (2 bytes of payload, 1 slot). */
bool mg_state_stage_vram_clear(void) {
    if (s_slot_used >= 8) return false;

    static const uint8_t zeros[2] = {0, 0};
    uint32_t off = alloc_payload(2);
    if (off == UINT32_MAX) return false;
    cart_window_load_blob(off, zeros, 2);

    CartDmaSlot slot = {
        .bbus = BBUS_VMDATAL,
        .dmap = DMAP_2B_2R | 0x10,  /* bit 4: fixed source */
        .src  = (uint16_t)off,
        .size = 0,                   /* SNES interprets DAS=0 as 65536 */
        .prep = 0,                   /* VMADDR start = 0 */
    };
    cart_window_set_dma_slot(s_slot_used++, &slot);
    return true;
}

/* "Arm" a clean-slate VRAM clear: stage the VRAM-fill slot AT slot 0,
 * promote it to persistent (so it survives build_frame's checkpoint
 * rewind on the first NMI), and record the frame_consumed value at
 * which we should drop it. h_ppu_clean_slate calls this immediately
 * after mg_state_reset; the demo's subsequent mg_chr_upload then
 * lands at slot 1 instead of slot 0, so the kernel processes the
 * VRAM clear FIRST (wiping leftover tilemap/CHR from any previous
 * demo) and THEN the CHR upload (writing the new demo's tile 0).
 *
 * The drop happens in mg_state_build_frame after cart_window's
 * frame_consumed counter advances past s_clean_slate_drop_at_consumed
 * — i.e., the kernel has finished walking the slot list at least once,
 * so the 24ms VRAM-clear DMA has run. After dropping, slot 0 is empty
 * (kernel skips bbus=0) and the persistent CHR upload at slot 1 stays
 * put. We don't bother reclaiming the 2 bytes of payload the clear's
 * zero source consumed — they're just sitting there harmlessly.
 *
 * One visible cost: the 24ms VRAM clear DMA pauses CPU and exceeds a
 * single vblank, so the first display frame after clean_slate shows
 * a brief flash of scrambled VRAM. Acceptable as a one-shot init cost;
 * a future enhancement could stage an INIDISP=$80 force-blank slot at
 * slot 0 ahead of the clear (slot 1 would become VRAM clear; slot 2
 * the CHR upload) to hide the flash, but that uses one more slot.
 *
 * Returns true on success, false if slot 0 is somehow already in use
 * (shouldn't happen — caller is responsible for mg_state_reset first). */
bool mg_state_arm_clean_slate_vram_clear(void) {
    if (s_slot_used != 0) return false;   /* expected called right after reset */
    if (!mg_state_stage_vram_clear()) return false;
    /* Promote the slot + payload to persistent so build_frame's rewind
     * leaves them alone on the first commit. */
    s_payload_checkpoint = s_payload_used;
    s_slot_checkpoint    = s_slot_used;
    s_staged_checkpoint  = s_staged_count;
    /* Drop the slot once the kernel has acked the next frame. */
    s_clean_slate_drop_at_consumed = cart_window_frame_consumed() + 1;
    s_clean_slate_pending = true;
    /* Force CGRAM re-upload for the next 3 frames (= the clean_slate
     * frame + the two right after) so the palette lands even if the
     * long VRAM-clear DMA disrupts the slot-2 CGRAM dispatch on the
     * first frame. */
    s_cgram_reupload_frames = 3;
    /* v1.87: bumped from 3 → 60 (= 1 second at 60 Hz). The 3-frame
     * reupload window in v1.84 didn't actually deliver the BG tilemap;
     * audio_mixer kept showing the same row-7/row-9-only scatter
     * pattern across many test runs. Each successful commit re-stages
     * 2 KB; over 60 commits that's 120 KB of DMA traffic (~ 4% of
     * vblank budget), which is the cheapest brute-force way to make
     * sure whatever frame finally lands the full tilemap exists in
     * the window. We'll back this down once the underlying timing
     * is fully understood (force-blank semantics across NMI overrun,
     * mid-active-display NMI, etc.). */
    s_bg_reupload_frames    = 60;

    /* v2.20: hold the screen at full force-blank until consumed has
     * advanced by 4 — covers the ~32 ms VRAM clear (1 NMI) plus 3
     * follow-up NMIs of demo setup. See the comment on
     * s_clean_slate_force_blank_until_consumed for the why. */
    s_clean_slate_force_blank_until_consumed = cart_window_frame_consumed() + 4;
    s_clean_slate_force_blank_active = true;
    return true;
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
    if (s_staged_count >= MG_MAX_STAGED_SLOTS) return false;
    if (size == 0) return true;     /* nothing to do, success */

    uint32_t off = alloc_payload(size);
    if (off == UINT32_MAX) return false;

    cart_window_load_blob(off, src, size);

    s_staged[s_staged_count++] = (StagedSlot){
        .bbus = bbus,
        .dmap = dmap,
        .prep = prep,
        .src  = (uint16_t)off,
        .size = (uint16_t)size,
    };
    /* Keep s_slot_used as a parallel byte-counter for legacy budget
     * APIs (mg_state_slots_remaining). One slot of any size counts
     * once toward the 8-slot soft limit the older API exposes; with
     * sub-frame chaining the real cap is MG_MAX_STAGED_SLOTS. */
    s_slot_used++;
    return true;
}

/* ---- sub-frame queue management ---- */

/* Stuff the eight cart-window slot entries from one sub-frame, padding
 * remaining slot indices with empty (bbus=0) so the kernel skips them
 * cleanly. */
static void write_subframe_to_cart_window(const SubFrame *sf) {
    static const CartDmaSlot empty_slot = {0};
    for (unsigned i = 0; i < 8; i++) {
        if (i < sf->slot_count) {
            CartDmaSlot s = {
                .bbus = sf->slots[i].bbus,
                .dmap = sf->slots[i].dmap,
                .src  = sf->slots[i].src,
                .size = sf->slots[i].size,
                .prep = sf->slots[i].prep,
            };
            cart_window_set_dma_slot(i, &s);
        } else {
            cart_window_set_dma_slot(i, &empty_slot);
        }
    }
}

/* Greedy-pack s_staged[] into sub-frames of ≤8 slots and ≤BUDGET bytes
 * each. Writes the FIRST sub-frame to the cart window's slot list so
 * the next NMI can process it. Subsequent sub-frames sit in
 * s_subframes[] waiting for mg_state_advance_subframe. */
static void flush_subframes(void) {
    s_subframe_count = 0;
    s_subframe_index = 0;
    if (s_staged_count == 0) {
        /* No work — clear cart-window slots so any leftover from a
         * previous frame's tail entries doesn't fire. */
        static const CartDmaSlot empty_slot = {0};
        for (unsigned i = 0; i < 8; i++) {
            cart_window_set_dma_slot(i, &empty_slot);
        }
        return;
    }

    SubFrame *cur = &s_subframes[0];
    cur->slot_count = 0;
    uint32_t cur_bytes = 0;
    s_subframe_count = 1;

    for (unsigned i = 0; i < s_staged_count; i++) {
        const StagedSlot *s = &s_staged[i];
        /* SNES DMA: size=0 means 65536. clean_slate's VRAM-clear is
         * the canonical user; count it at its real transfer size for
         * budget purposes so it lands in its own sub-frame. */
        uint32_t slot_bytes = (s->size == 0) ? 65536u : s->size;
        bool need_new_subframe =
            (cur->slot_count >= 8) ||
            (cur->slot_count > 0 &&
             cur_bytes + slot_bytes > MG_SUBFRAME_BYTE_BUDGET);
        if (need_new_subframe) {
            if (s_subframe_count >= MG_MAX_SUBFRAMES) {
                /* Out of sub-frame slots — drop the remaining
                 * stage entries. Should be rare; bump the cap if it
                 * fires (stderr to surface for tuning). */
                fprintf(stderr,
                    "mgapi: subframe overflow (staged=%u, dropped from %u)\n",
                    s_staged_count, i);
                break;
            }
            cur = &s_subframes[s_subframe_count++];
            cur->slot_count = 0;
            cur_bytes = 0;
        }
        cur->slots[cur->slot_count++] = *s;
        cur_bytes += slot_bytes;
    }

    write_subframe_to_cart_window(&s_subframes[0]);
}

/* Advance to the next sub-frame's slot list. Called by cart_window
 * when the kernel reads port 7 (which it does once per main-loop
 * iteration, i.e. between NMIs). Returns true if another sub-frame
 * was loaded into the cart window (caller keeps FRAME_RDY=1), false
 * if the queue is empty (caller bumps frame_consumed and clears
 * FRAME_RDY). */
bool mg_state_advance_subframe(void) {
    s_subframe_index++;
    if (s_subframe_index >= s_subframe_count) {
        /* Logical frame fully delivered. */
        s_subframe_index = 0;
        s_subframe_count = 0;
        return false;
    }
    write_subframe_to_cart_window(&s_subframes[s_subframe_index]);
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
    /* Advance the persistent checkpoint -- this is a "direct" upload
     * (mg_chr_upload, etc.) that the guest expects to survive across
     * frames. build_frame's reset rewinds to here instead of zero so
     * the bytes + slot stay intact. */
    s_payload_checkpoint = s_payload_used;
    s_slot_checkpoint    = s_slot_used;
    s_staged_checkpoint  = s_staged_count;
    return 0;
}

/* Pending transient queue — copied from the guest now, staged into
 * the cart window LATER (inside build_frame, AFTER the shadow walker)
 * so the transient bytes land AFTER the shadow DMAs in payload. If
 * we stage_dma now, build_frame's payload-pointer rewind would
 * clobber the bytes when the shadow walker writes from the checkpoint
 * onward. Bookkeeping is reset every successful flush. */
#define MG_PENDING_TRANSIENT_BUF_BYTES (32u * 1024u)
#define MG_PENDING_TRANSIENT_MAX        8u
typedef struct {
    uint32_t buf_off;
    uint16_t size;
    uint16_t prep;
    uint8_t  bbus;
    uint8_t  dmap;
} PendingTransientDma;
static uint8_t  s_pending_buf[MG_PENDING_TRANSIENT_BUF_BYTES];
static PendingTransientDma s_pending[MG_PENDING_TRANSIENT_MAX];
static uint32_t s_pending_buf_used;
static uint32_t s_pending_count;

int mg_state_queue_dma_transient(const void *src, uint32_t size,
                                 uint8_t bbus, uint8_t dmap, uint16_t prep) {
    if (s_pending_count >= MG_PENDING_TRANSIENT_MAX) return -1;
    if (s_pending_buf_used + size > MG_PENDING_TRANSIENT_BUF_BYTES) return -2;

    /* Copy the bytes now — the guest's source pointer may be reused
     * before build_frame fires. */
    memcpy(s_pending_buf + s_pending_buf_used, src, size);
    s_pending[s_pending_count] = (PendingTransientDma){
        .buf_off = s_pending_buf_used,
        .size    = (uint16_t)size,
        .prep    = prep,
        .bbus    = bbus,
        .dmap    = dmap,
    };
    s_pending_count++;
    s_pending_buf_used += size;
    return 0;
}

void mg_state_drop_pending_transients(void) {
    /* See header comment for the failure mode this prevents
     * (h_frame_commit silent-drop + accumulated FMV transients =
     * "every other frame is garbage" symptom). */
    s_pending_count    = 0;
    s_pending_buf_used = 0;
}

/* Stage queued transient DMAs into the cart-window payload. Called
 * by build_frame after the shadow walker so the transient bytes land
 * past the shadow region. Resets the queue. */
static void flush_pending_transients(void) {
    for (uint32_t i = 0; i < s_pending_count; i++) {
        const PendingTransientDma *p = &s_pending[i];
        (void)stage_dma(s_pending_buf + p->buf_off, p->size,
                         p->bbus, p->dmap, p->prep);
    }
    s_pending_count    = 0;
    s_pending_buf_used = 0;
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

/* Compute BGxSC ($2107-$210A) from a layer's tilemap_word + size_code.
 *
 * v1.78 fix: BG1SC bits 2-7 hold the tilemap base in 1024-WORD units
 * (= 2048 BYTE units), not 1024-byte. The original >> 9 derivation
 * was off by one shift and placed the staged tilemap 2KB below where
 * the PPU actually fetched it from. Only audio_mixer (which is the
 * first demo to ever use tilemap_word != 0 outside Mode 7) exposed
 * this — sprite.elf uses tilemap_word=0 so BG1SC=0 worked by accident,
 * and Mode 7 has its own non-BG1SC tilemap addressing.
 *
 * bsnes-plus mmio_w2107 confirms: screen_addr = (data & 0xfc) << 9,
 * which encodes ((bits 2-7) * 2048 bytes) = ((bits 2-7) * 1024 words).
 * So bits 2-7 = tilemap_word / 1024 = tilemap_word >> 10.
 *
 *   bits 2-7: tilemap base / 1024 words = word >> 10
 *   bits 0-1: size_code (MgBgSize) */
static uint8_t compute_bgxsc(const MgBgLayerState *bg) {
    return (uint8_t)(((bg->tilemap_word >> 10) << 2) | (bg->size_code & 3));
}

/* Compute the CHR-page index for BG12NBA / BG34NBA.
 *
 * The PPU encodes the BG CHR base as low-nibble * $1000 in VRAM word
 * coordinates: $0000 / $1000 / $2000 / ... / $7000. chr_word stores
 * the base in VRAM words (same units), so the conversion is
 *
 *     page_index = chr_word / $1000 = chr_word >> 12
 *
 * Earlier the shift was 11 (= divide by $0800), which doubled the
 * encoded page for every chr_word -- a guest asking for $1000 got
 * page 2 (= $2000) in BG12NBA. BG1 looked at $2000, the actual
 * upload at VRAM $1000 went unused, screen rendered backdrop. */
static uint8_t chr_page(const MgBgLayerState *bg) {
    return (uint8_t)((bg->chr_word >> 12) & 0x0F);
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

    /* TM / TS: per-layer main/sub bits + sprites only when at least
     * one sprite is actually placed onscreen. mg_state_reset parks
     * all 128 sprites at Y=240 (offscreen); any guest mg_sprite_set
     * (or similar) call that moves one to Y<240 flips OBJ on for
     * that frame. Demos that never touch sprites (mode7, mode7_3d)
     * get OBJ off, so a stray uninit byte in OAM or a sprite-CHR
     * read picking up garbage from another demo's VRAM doesn't
     * render a phantom block on the main screen. */
    uint8_t tm = 0, ts = 0;
    for (unsigned i = 0; i < MG_BG_LAYERS; i++) {
        if (s->bg[i].enabled_main) tm |= (uint8_t)(1u << i);
        if (s->bg[i].enabled_sub)  ts |= (uint8_t)(1u << i);
    }
    bool any_sprite_onscreen = false;
    for (unsigned i = 0; i < 128; i++) {
        if (s->oam_shadow[i * 4 + 1] < 224) {
            any_sprite_onscreen = true;
            break;
        }
    }
    if (any_sprite_onscreen) tm |= 0x10;
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
 * Uses DIRECT mode encoding (one value per scanline) because bsnes-
 * plus's HDMA doesn't honor repeat-mode bit 7 (it always advances the
 * source address per scanline). A real-hardware repeat table renders
 * incorrectly there — the channel reads past the intended data after
 * scanline 1. Direct mode works identically on both. Direct mode
 * format: [count_byte][value_0][value_1]...[value_count-1]. Count's
 * low 7 bits = chunk length (1..127); bit 7 = 0 (no repeat). $00
 * count terminates the channel for the frame.
 *
 * COMMON CASE (no letterbox: force_blank_top == 0 && force_blank_bottom
 * == 0) — emit just a 0x00 terminator. Channel 7 fires at scanline 0,
 * reads $00 as the count byte, marks the channel completed for the
 * frame, and never touches INIDISP. The kernel writes INIDISP=$0F at
 * boot and at every NMI entry, so the screen stays visible at full
 * brightness throughout the frame.
 *
 * LETTERBOX CASE — emit a direct chunk for the top force-blank lines
 * (value $80 = force-blank), one or two chunks for the visible middle
 * (value $0F = visible/full-brightness), and a final chunk for the
 * bottom force-blank lines. SNES caps each chunk at 127 lines; the
 * 224-scanline middle needs two chunks (127 + 97). Total table size
 * peaks around 230 bytes for an all-letterbox frame, comfortably
 * within CW_INIDISP_HDMA_BYTES = 256.
 *
 * Cost per scanline on the SNES DMA side: 8 master cycles per byte
 * read + 8 per write to INIDISP = 16 master cycles per scanline. For
 * 224 lines that's 3584 master cycles spread across the frame's
 * HBLANKs — negligible compared to per-scanline budgets. */
static void emit_inidisp_table(void) {
    uint8_t buf[CW_INIDISP_HDMA_BYTES] = {0};
    uint8_t *p = buf;
    uint8_t  t = s_state.force_blank_top;
    uint8_t  b = s_state.force_blank_bottom;

    /* v2.20: clean_slate full-frame force-blank override. Emit a
     * 224-line all-$80 table (terminator at end) while consumed has
     * not advanced to the threshold the arm step recorded. Gating on
     * consumed (not on a build_frame counter) means the window lasts
     * however long the SNES actually needs to process the clear +
     * initial setup, regardless of how many host commits the guest's
     * loop fires in the meantime. */
    if (s_clean_slate_force_blank_active &&
        cart_window_frame_consumed() < s_clean_slate_force_blank_until_consumed) {
        int lines = 224;
        while (lines > 0) {
            int n = lines > 127 ? 127 : lines;
            *p++ = (uint8_t)(0x80u | (uint8_t)n);
            for (int i = 0; i < n; i++) *p++ = 0x80;
            lines -= n;
        }
        *p++ = 0x00;                  /* terminator */
        /* v2.23: write the FULL CW_INIDISP_HDMA_BYTES region (not just
         * (p - buf)) so trailing bytes from a previous frame's larger
         * table are explicitly zeroed. Stale $80 chunks past the new
         * terminator caused letterbox-height top-band ghosting in any
         * demo following demo_letterbox — bsnes-plus's HDMA quirks
         * meant the terminator wasn't reliably stopping channel 7. */
        cart_window_load_blob(CW_OFF_INIDISP_HDMA, buf,
                              CW_INIDISP_HDMA_BYTES);
        return;
    }
    /* Threshold reached — disarm so subsequent emits go through the
     * normal letterbox path even if force_blank_until_consumed wraps. */
    s_clean_slate_force_blank_active = false;

    if (t == 0 && b == 0) {
        /* No letterbox: terminator-only. HDMA channel 7 completes
         * before writing INIDISP this frame; the kernel's boot/NMI
         * INIDISP=$0F write is the only thing the PPU sees. */
        /* buf is already zero-initialized; write the full region so
         * trailing bytes from a previous frame's larger table can't
         * confuse channel 7. See the v2.23 comment in the force-blank
         * branch above for the failure mode this prevents. */
        cart_window_load_blob(CW_OFF_INIDISP_HDMA, buf,
                              CW_INIDISP_HDMA_BYTES);
        return;
    }

    /* v2.16: TOP-only mode (t > 0, b == 0). HDMA channel 7 writes
     * $80 for the top T lines, then reads a terminator and goes
     * idle for the rest of the frame.
     *
     * Why: demos with very large per-NMI DMAs (FMV's chr2/chr3 = 9120
     * B each = ~17 scanlines of overrun past vblank into the next
     * frame) NEED the kernel-set INIDISP=$80 to stay in effect
     * throughout the overrun, otherwise the PPU silently drops VRAM
     * writes once HDMA flips INIDISP back to $0F at scanline 8.
     *
     * With TOP-only encoding, HDMA writes $80 for lines 0..T-1
     * (matching the kernel-set value), then terminates. INIDISP stays
     * at the last-written value ($80) until the kernel writes $0F at
     * @done after the DMA loop ends. Effective "force-blank top" is
     * however many scanlines pass before the kernel's @done — i.e.,
     * exactly as long as the DMA needs. No HDMA flip mid-DMA, no
     * dropped VRAM writes.
     *
     * Trade-off: there's no HDMA-driven bottom letterbox in this
     * mode. Demos that need symmetric letterbox should set both T
     * and B (falls through to the full encoder below). */
    if (t > 0 && b == 0) {
#define LB_CHUNK(n, val) do {                          \
        *p++ = (uint8_t)(0x80u | (uint8_t)(n));        \
        for (int _i = 0; _i < (n); _i++) *p++ = (val); \
    } while (0)
        while (t > 0) {
            uint8_t n = t > 127 ? 127 : t;
            LB_CHUNK(n, 0x80);
            t = (uint8_t)(t - n);
        }
        /* v2.31: explicit $0F transition at line t (after the $80
         * chunk, before the terminator). Without this, the HDMA
         * channel terminates with INIDISP at whatever the last $80
         * write left it (always $80 in this branch). The kernel's
         * @done write of $0F gets overridden by HDMA's $80 writes
         * at any line ≤ 7, so @done lands too early to stick. With
         * this explicit $0F at line t = force_blank_top, INIDISP
         * is guaranteed $0F from line t onwards regardless of
         * @done timing — and regardless of frame_ready (idle NMI
         * still gets a proper unblank from HDMA). */
        LB_CHUNK(1, 0x0F);
        *p++ = 0x00;                  /* terminator — ch7 done for rest of frame */
#undef LB_CHUNK
        /* v2.23: write the FULL CW_INIDISP_HDMA_BYTES region (not just
         * (p - buf)) so trailing bytes from a previous frame's larger
         * table are explicitly zeroed. Stale $80 chunks past the new
         * terminator caused letterbox-height top-band ghosting in any
         * demo following demo_letterbox — bsnes-plus's HDMA quirks
         * meant the terminator wasn't reliably stopping channel 7. */
        cart_window_load_blob(CW_OFF_INIDISP_HDMA, buf,
                              CW_INIDISP_HDMA_BYTES);
        return;
    }

    int visible = 224 - (int)t - (int)b;
    if (visible < 0) visible = 0;

    /* Hybrid encoding: count = 0x80 | N where N is the line count for
     * this chunk (1..127), followed by N data bytes (one per scanline).
     *
     * bsnes-plus's HDMA loop:
     *   per scanline: if do_transfer { transfer; src++ }; line_counter--;
     *                  do_transfer = line_counter & 0x80;
     *                  if (line_counter & 0x7F) == 0: refetch count.
     *
     * For count $80 | N, line_counter starts at $80 + N. After N
     * decrements it's at $80. Bit 7 still set → do_transfer = true for
     * all N transfers. Then (line_counter & 0x7F) == 0 → REFETCH
     * immediately, picking up the next chunk's count byte. So N
     * transfers fire on scanlines 0..N-1 and the next chunk takes over
     * on scanline N. Exactly what we want.
     *
     * Each scanline reads a separate data byte from the source. We
     * emit N copies of the desired INIDISP value ($80 force-blank or
     * $0F visible). Max chunk N = 127 (count $FF = $80 | $7F), so the
     * typical 208-line middle band splits into 127 + 81. Real SNES
     * note: on real hardware this encoding makes one transfer per
     * scanline that reads the same single repeated value (the first
     * data byte gets used for all N lines, the rest skipped — but the
     * source addr advance differs from bsnes-plus, so the next chunk's
     * count byte lands at a different offset). bsnes-plus only for
     * now; real-SNES compat would need a different encoding. */
#define LB_CHUNK(n, val) do {                          \
        *p++ = (uint8_t)(0x80u | (uint8_t)(n));        \
        for (int _i = 0; _i < (n); _i++) *p++ = (val); \
    } while (0)

    /* Top force-blank chunk(s). Cap at 127 per chunk. */
    while (t > 0) {
        uint8_t n = t > 127 ? 127 : t;
        LB_CHUNK(n, 0x80);
        t = (uint8_t)(t - n);
    }

    /* Visible middle. */
    while (visible > 0) {
        int n = visible > 127 ? 127 : visible;
        LB_CHUNK(n, 0x0F);
        visible -= n;
    }

    /* Bottom force-blank chunk(s). */
    while (b > 0) {
        uint8_t n = b > 127 ? 127 : b;
        LB_CHUNK(n, 0x80);
        b = (uint8_t)(b - n);
    }

    *p++ = 0x00;                       /* terminator */
#undef LB_CHUNK

    cart_window_load_blob(CW_OFF_INIDISP_HDMA, buf,
                          (uint32_t)(p - buf));
}

void mg_state_build_frame(void) {
    /* Drop the clean-slate VRAM-clear slot once the kernel has acked
     * the frame that contained it. mg_state_arm_clean_slate_vram_clear
     * armed s_clean_slate_drop_at_consumed = current_consumed + 1 right
     * after promoting slot 0 to persistent; once frame_consumed has
     * caught up, the clear has fired exactly once and we can free the
     * slot. We leave the 2 bytes of zero payload in place (no point
     * compacting; future allocations land safely past s_payload_used).
     *
     * After dropping, slot 0 is empty (bbus=0 → kernel skips); the
     * persistent CHR upload that demos stage immediately after
     * clean_slate sits at slot 1 and is unaffected. */
    if (s_clean_slate_pending &&
        cart_window_frame_consumed() >= s_clean_slate_drop_at_consumed) {
        static const CartDmaSlot empty_slot = {0};
        cart_window_set_dma_slot(0, &empty_slot);
        s_clean_slate_pending = false;
        s_clean_slate_drop_at_consumed = 0;
    }

    /* Force a full CGRAM re-upload while the post-clean-slate counter
     * is positive. The shadow has whatever palette the guest set up;
     * widening dirty to [0, 512) just makes the CGRAM emission below
     * pick up the whole shadow regardless of what the guest touched. */
    if (s_cgram_reupload_frames > 0) {
        s_state.cgram_dirty_lo = 0;
        s_state.cgram_dirty_hi = sizeof(s_state.cgram_shadow);
        s_cgram_reupload_frames--;
    }

    /* Same trick for BG tilemaps — widen each enabled layer's dirty
     * range to the full 2 KB so the emission below re-DMAs it. Three
     * frames cover the clean_slate VRAM-clear overrun window (see
     * s_bg_reupload_frames at top of file). Disabled layers stay
     * skipped — the emit_bg loop's enabled-check still gates each DMA. */
    if (s_bg_reupload_frames > 0) {
        for (unsigned i = 0; i < MG_BG_LAYERS; i++) {
            MgBgLayerState *bg = &s_state.bg[i];
            if (!bg->enabled_main && !bg->enabled_sub) continue;
            bg->dirty_lo = 0;
            bg->dirty_hi = MG_BG_TILEMAP_BYTES;
        }
        s_bg_reupload_frames--;
        if (s_bg_reupload_frames == 0) {
            /* One-shot stderr line so we can confirm the reupload window
             * actually ran to completion. If this never fires, the
             * counter never started; if it fires and the screen still
             * scatters, every frame's DMA is failing the same way and
             * the bug is downstream of staging. */
            fprintf(stderr,
                    "mgapi: bg_reupload window closed (60 commits)\n");
            fflush(stderr);
        }
    }


    /* Reset per-frame bookkeeping. We rewind to the persistent
     * checkpoint (set by mg_chr_upload and other direct-stage paths),
     * not to zero, so any persistent uploads from before this commit
     * survive the rebuild. Shadow emissions below allocate from the
     * checkpoint onward, leaving the persistent bytes + slot 0..N-1
     * untouched.
     *
     * Without this rewind to checkpoint, a demo that calls
     * mg_chr_upload once at startup loses its CHR bytes on the very
     * next mg_frame_commit -- the CGRAM / tilemap shadow walker
     * overwrites payload offset 0 (CHR's bytes) and slot 0 (CHR's
     * descriptor). The kernel then DMAs garbage into VRAM and BG1
     * renders empty tiles -- the bug behind John's "still blank"
     * screen even when dma=1 in the diag heartbeat. */
    s_payload_used = s_payload_checkpoint;
    s_slot_used    = s_slot_checkpoint;
    s_staged_count = s_staged_checkpoint;
    /* HDMA-tables pool is also reset per-frame. The contract on the
     * comment above ("HDMA tables stay across frames unless re-
     * uploaded") meant that the BYTES at a given offset persist, but
     * without resetting the bump pointer the pool fills up after
     * three frames and dynamic HDMA (per-frame matrix re-upload, the
     * Mode 7 perspective case) panics with MG_R_ERR_DMA_BYTES.
     *
     * Resetting per-frame means the next round of upload calls start
     * from offset 0 and overwrite the previous frame's bytes -- which
     * is exactly what dynamic-HDMA games want. Static HDMA still
     * works the same way: the game just re-uploads the identical
     * table every frame; cheap, since the bytes flow through the
     * cart-window payload only (no SNES-side DMA per upload).
     *
     * The kernel's HDMA-config descriptors carry table_off per
     * channel and stay stable until mg_hdma_setup is called again,
     * so consistent per-channel offset (which mg_hdma_upload_table
     * gives you naturally for consistent call order) keeps each
     * channel pointing at its own slice across frames. */
    s_hdma_tables_used = 0;

    /* Clear ONLY the slot indices the shadow walker populated last
     * frame (s_prev_mg_slots), and only those AT OR AFTER the
     * persistent checkpoint. Slots at indices 0..checkpoint-1 are
     * persistent uploads (mg_chr_upload, etc.) and must survive.
     * Slots beyond the shadow region are left alone so guests that
     * mix SYS_COPRO_STAGE_DMA_SLOT with the mg_* API keep their
     * explicit entries across frames. */
    static const CartDmaSlot empty_slot = {0};
    for (unsigned i = s_slot_checkpoint;
         i < s_prev_mg_slots && i < 8;
         i++) {
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

    /* Mode 7 batch — always emitted; the kernel always writes the M7
     * regs but their effect only shows when bgmode == 7. Cheap. */
    {
        Mode7Batch m7 = {0};
        m7.m7sel = s_state.m7sel;
        m7.m7a   = s_state.m7a;
        m7.m7b   = s_state.m7b;
        m7.m7c   = s_state.m7c;
        m7.m7d   = s_state.m7d;
        m7.m7x   = s_state.m7cx;
        m7.m7y   = s_state.m7cy;
        cart_window_set_mode7_batch(&m7);
    }

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

    /* CGRAM goes BEFORE OAM. With OAM at slot[1] and CGRAM at slot[2],
     * frame 1's CGRAM DMA fails to land in PPU on bsnes-plus (likely
     * the 544-byte OAM DMA leaves the SNES bus / OAMADDR in a state
     * that breaks the subsequent CGRAM dispatch). Putting CGRAM at a
     * lower slot index keeps it ahead of the OAM DMA where dispatch
     * is reliably proven to work. */
    if (s_state.cgram_dirty_hi > s_state.cgram_dirty_lo) {
        uint16_t lo = s_state.cgram_dirty_lo;
        uint16_t hi = s_state.cgram_dirty_hi;
        const uint8_t *src = (const uint8_t *)s_state.cgram_shadow + lo;
        (void)stage_dma(src, (uint16_t)(hi - lo),
                        BBUS_CGDATA, DMAP_1B_1R,
                        cgram_prep_word(lo));
        s_state.cgram_dirty_lo = s_state.cgram_dirty_hi = 0;
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

    /* BG tilemaps. Each layer's tilemap_word is the VRAM word
     * address; we DMA at VMAIN auto-increment + VMADDR = tilemap_word
     * + (dirty_lo / 2). The 2-byte-2-reg DMAP writes tile words. */
    for (unsigned i = 0; i < MG_BG_LAYERS; i++) {
        MgBgLayerState *bg = &s_state.bg[i];
        if (bg->dirty_hi <= bg->dirty_lo) continue;
        /* Skip disabled layers — h_ppu_clean_slate dirties all four
         * BG shadows so any enabled one gets its tilemap area zeroed
         * on the next commit, but we don't want to waste payload + a
         * slot DMAing to a tilemap_word the demo will never read.
         * Without this, 4 layers × 2KB = 8KB exceeds the per-vblank
         * DMA budget. The dirty bits stay set until the layer is
         * enabled (no-op until then). */
        if (!bg->enabled_main && !bg->enabled_sub) continue;
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

    /* Stage any pending transient uploads (mg_chr_upload_transient).
     * These land AFTER the shadow region in payload, so the shadow
     * walker's writes to offsets [checkpoint .. shadow_end] don't
     * collide with them. v2.04. */
    flush_pending_transients();

    /* v2.05: split s_staged[] into sub-frames and write the first
     * sub-frame's slots to the cart window. Subsequent sub-frames are
     * loaded by mg_state_advance_subframe (called from cart_window's
     * port-7 read handler). Until the queue drains, the host keeps
     * FRAME_RDY = 1 and the kernel processes one sub-frame per NMI. */
    flush_subframes();

    /* Remember how many slots we used so the next frame's clear can
     * be precise instead of stomping the whole list. */
    s_prev_mg_slots = s_slot_used;
}
