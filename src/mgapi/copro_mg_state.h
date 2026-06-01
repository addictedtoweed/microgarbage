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

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_COPRO_MG_STATE_H */
