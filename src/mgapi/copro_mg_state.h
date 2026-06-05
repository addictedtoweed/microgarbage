/* ============================================================
 *  copro_mg_state.h — shadow PPU state for the SYS_MG_* handlers.
 *
 *  Implementation detail of mgapi.dll / libmgapi. NOT exposed in
 *  the public mgapi.h ABI.
 *
 *  The SYS_MG_* handlers in copro_mg_handlers.c update a per-process
 *  shadow of the SNES PPU state (OAM, CGRAM, BG tilemaps, OBSEL,
 *  PPU register batch). At mg_frame_commit() the state builder here
 *  walks the dirty ranges, stages the bytes into the cart window's
 *  payload area, and writes DMA descriptors into the cart_window's
 *  DMA slot list — the existing protocol the SNES kernel consumes.
 *
 *  Lifecycle: mg_state_init() at mgapi startup, mg_state_shutdown()
 *  at teardown. mg_state_build_frame() called by the modified
 *  SYS_COPRO_FRAME_COMMIT handler before it sets the frame-ready byte.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_COPRO_MG_STATE_H
#define MGAPI_COPRO_MG_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SNES PPU constants for our shadows. */
#define MG_OAM_BYTES        (512 + 32)   /* main + high OAM           */
#define MG_CGRAM_BYTES      512          /* 256 entries * 2 bytes     */
#define MG_BG_TILEMAP_BYTES 2048         /* 32x32 cells * 2 bytes     */
#define MG_BG_LAYERS        4

/* MgSprite mirror — kept in sync with examples/common/guest/mg_sprite.h.
 * Layout matches: int16 x, uint8 y, uint16 tile, packed attrs byte.
 * If you change one, change both. */
typedef struct {
    int16_t  x;
    uint8_t  y;
    uint16_t tile;
    uint8_t  palette    : 3;
    uint8_t  priority   : 2;
    uint8_t  hflip      : 1;
    uint8_t  vflip      : 1;
    uint8_t  size_large : 1;
} MgSpriteHost;
_Static_assert(sizeof(MgSpriteHost) == 8,
               "MgSpriteHost layout drifted from guest mg_sprite.h");

/* Sprite-side state derived from sprite_sizes / sprite_chr_base. */
typedef struct {
    uint8_t  sizes_code;          /* 0..7, the MgSpriteSizes enum value */
    uint16_t chr_base0_word;
    uint16_t chr_base1_word;
} MgSpriteConfig;

/* BG layer state derived from bg_setup / bg_scroll / bg_enable. */
typedef struct {
    uint16_t tilemap_word;        /* VRAM word base of the tilemap     */
    uint16_t chr_word;            /* VRAM word base of the layer's CHR */
    uint8_t  size_code;           /* MgBgSize: 0=32x32 ... 3=64x64     */
    bool     enabled_main;
    bool     enabled_sub;
    int16_t  hofs, vofs;
    uint8_t  shadow[MG_BG_TILEMAP_BYTES];   /* shadow tilemap (32x32) */
    uint16_t dirty_lo, dirty_hi; /* byte range; hi=lo means clean      */
} MgBgLayerState;

/* Snapshot of the whole shadow PPU state. */
typedef struct {
    /* OAM shadow + dirty range. */
    uint8_t  oam_shadow[MG_OAM_BYTES];
    uint16_t oam_dirty_lo, oam_dirty_hi;

    /* CGRAM shadow + dirty range. */
    uint16_t cgram_shadow[MG_CGRAM_BYTES / 2];
    uint16_t cgram_dirty_lo, cgram_dirty_hi;

    /* BG layer table. */
    MgBgLayerState bg[MG_BG_LAYERS];

    /* Sprite config — applies to all of OAM. */
    MgSpriteConfig spr;

    /* Mode / global PPU state. */
    uint8_t  bgmode;              /* 0..7                              */

    /* Force-blank window. Each scanline yields ~117 more DMA bytes per
     * frame. Default 0,0 = no forced blank, baseline budget. */
    uint8_t  force_blank_top;
    uint8_t  force_blank_bottom;

    /* Set by h_ppu_clean_slate; the next build_frame stages a VRAM-clear
     * DMA slot (fixed-source 64KB fill) and clears the flag. Without
     * this latch the clean-slate's direct slot allocation gets clobbered
     * by build_frame's s_slot_used = s_slot_checkpoint reset, so the
     * VRAM clear never fires and PPU state leaks between demos. */
    bool     pending_vram_clear;

    /* HDMA channel config — channels 0..6 (channel 7 is reserved for
     * INIDISP). The runtime publishes this to the cart window's
     * HDMA control table every frame so the kernel can set up the
     * SNES HDMA registers correctly. */
    struct {
        bool     enabled;
        uint8_t  bbad;          /* $21xx low byte                     */
        uint8_t  dmap;          /* SNES DMAP byte (transfer mode)     */
        uint16_t table_off;     /* offset within cart window           */
    } hdma[7];

    /* Mode 7 matrix + center + wrap mode. Identity defaults so a
     * call to mg_bg_mode(MG_BG_MODE_7) without explicit set is sane. */
    int16_t  m7a, m7b, m7c, m7d;   /* 8.8 fixed-point matrix            */
    int16_t  m7cx, m7cy;           /* 13-bit signed center              */
    uint8_t  m7sel;                /* wrap / fill / flip                */
} MgState;

/* -------- Lifecycle -------- */

void mg_state_init    (void);
void mg_state_shutdown(void);

/* Reset shadow state to defaults (called at mgapi reset or game
 * boot to recover from a previous game's residual state). */
void mg_state_reset   (void);

/* -------- Accessors used by the handlers -------- */

/* Returns the singleton state. Handlers mutate fields directly. */
MgState *mg_state(void);

/* Mark a byte range in OAM / CGRAM / a BG tilemap as dirty so the
 * frame builder DMAs only that range. The helpers handle the
 * empty-range case (lo == hi). */
void mg_state_dirty_oam   (uint16_t lo, uint16_t hi);
void mg_state_dirty_cgram (uint16_t lo, uint16_t hi);
void mg_state_dirty_bg    (uint8_t layer, uint16_t lo, uint16_t hi);

/* -------- Frame commit: shadow → cart window DMA slots -------- */

/* Walk the dirty ranges of every shadow buffer, stage the bytes into
 * the cart window's payload area, queue DMA slots in the cart window's
 * DMA list. Resets all dirty ranges. Called by the modified
 * SYS_COPRO_FRAME_COMMIT handler before it sets frame-ready. */
void mg_state_build_frame(void);

/* Queue an arbitrary DMA into the per-frame slot list. Used by
 * mg_chr_upload (and any other handler that needs a DMA outside the
 * shadow flow). Returns:
 *    0   = success (MG_OK)
 *   -1   = no free DMA slot (MG_ERR_DMA_SLOTS)
 *   -2   = no payload-area space (MG_ERR_DMA_BYTES)
 * Bytes are copied into the cart window's payload area immediately. */
int  mg_state_queue_dma(const void *src, uint32_t size,
                        uint8_t bbus, uint8_t dmap, uint16_t prep);

/* Budget introspection — both reflect bookkeeping AS OF the most
 * recent SYS_COPRO_FRAME_COMMIT (the slot count + payload bytes
 * accumulated since the previous commit). Cheap, never blocks. */
uint8_t  mg_state_slots_remaining (void);
uint16_t mg_state_bytes_remaining (void);

/* HDMA table staging — copies `len` bytes from `src` into the cart
 * window's HDMA tables area at a bump-allocated offset, returning
 * that offset on success (caller saves it into hdma[channel].table_
 * off). Returns UINT16_MAX if the area is full.
 *
 * Tables persist across frames in the cart window — no per-frame
 * wipe — but the bump pointer resets only when the runtime explicitly
 * does. mg_hdma_upload_table is a per-call append; the game is
 * responsible for not exceeding CW_HDMA_TABLES_BYTES. */
uint16_t mg_state_stage_hdma_table(const void *src, uint16_t len);

/* Drop the per-frame HDMA-table bump pointer back to 0. Used by
 * h_frame_commit's early-return path so a tight commit-cancelled loop
 * doesn't accumulate uploads across iterations and overflow the pool.
 * See implementation comment for the safety argument. */
void mg_state_drop_hdma_tables(void);

/* Stage a full-VRAM-clear DMA slot using the SNES fixed-source trick:
 * a 2-byte zero source + DMA with DAS=0 (= 65536 byte transfers) +
 * DMAP bit 4 set (fixed source, no increment) → fills all 32K VRAM
 * words with $0000 in a single DMA from the same 2-byte payload.
 * Used by mg_ppu_clean_slate to wipe leftover tilemap/CHR from a
 * previous demo's run. Returns false if no free slot or payload
 * space is available. */
bool mg_state_stage_vram_clear(void);

/* Stage the VRAM-clear slot AT slot 0, promote it to persistent, and
 * arm the build_frame self-drop after the kernel acks the next frame.
 * Caller must invoke this right after mg_state_reset and BEFORE any
 * mg_chr_upload / mg_palette_write so the CHR upload lands at slot 1
 * (after the VRAM clear) instead of overlapping it. Returns false on
 * unexpected non-empty slot 0; otherwise true. */
bool mg_state_arm_clean_slate_vram_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_COPRO_MG_STATE_H */
