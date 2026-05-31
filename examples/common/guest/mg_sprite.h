/* ============================================================
 *  mg_sprite.h — OAM sprites for cart-side games.
 *
 *  Model: per-sprite writes go to a host-side shadow OAM with no
 *  DMA cost. mg_frame_commit() emits ONE DMA descriptor covering
 *  the dirty range (low watermark to high watermark) per frame,
 *  regardless of how many sprite calls happened.
 *
 *  Slots are a flat 128-entry resource shared across all VMs.
 *  "Whoever writes the slot wins." mg_sprite_alloc/free is an
 *  advisory cooperative allocator (auto-released on VM exit); direct
 *  slot access still works for the "I own all sprites" case.
 *
 *  See docs/game-api.md for full design rationale.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_SPRITE_H
#define MG_SPRITE_H

#include <stdint.h>

#include "mg_panic.h"   /* MgResult */

#ifdef __cplusplus
extern "C" {
#endif

/* The OAM-shaped sprite descriptor. Position fields stay as native
 * ints (hot-path math); attrs pack into one byte matching the SNES
 * OAM byte-3 layout exactly. The runtime extracts x's high bit into
 * high-OAM and y's offscreen convention (y >= 240) for hidden. */
typedef struct {
    int16_t  x;                 /* -256..255, runtime packs sign bit  */
    uint8_t  y;                 /* 0..255; y >= 240 = offscreen        */
    uint16_t tile;              /* 0..511 (9-bit; uint16 for alignment) */
    uint8_t  palette    : 3;    /* 0..7 (resolves to CGRAM palette 8+)  */
    uint8_t  priority   : 2;    /* 0..3 (layers vs BG planes)           */
    uint8_t  hflip      : 1;
    uint8_t  vflip      : 1;
    uint8_t  size_large : 1;    /* picks OBSEL small vs large dims      */
} MgSprite;

/* OBSEL sprite-size pair selector. Each enum names the small / large
 * pixel dimensions; the names with x in them (6, 7) are the
 * asymmetric pairs F-Zero used. */
typedef enum {
    MG_SPR_SIZES_8_16        = 0,
    MG_SPR_SIZES_8_32        = 1,
    MG_SPR_SIZES_8_64        = 2,
    MG_SPR_SIZES_16_32       = 3,
    MG_SPR_SIZES_16_64       = 4,
    MG_SPR_SIZES_32_64       = 5,
    MG_SPR_SIZES_16x32_32x64 = 6,
    MG_SPR_SIZES_16x32_32x32 = 7,
} MgSpriteSizes;

/* -------- Per-slot operations -------- */

/* Write the whole sprite to shadow OAM. No DMA cost. */
void mg_sprite_set (uint8_t slot, const MgSprite *s);

/* Read the shadow OAM for a slot. Useful for "x += vx" style updates
 * without re-publishing the full struct. Reading a slot that was
 * never set returns the defaults (y=240, everything else zero). */
void mg_sprite_get (uint8_t slot, MgSprite *out);

/* Update only x/y of an existing slot. Cheaper for the hot path
 * than building a full MgSprite each frame. */
void mg_sprite_move(uint8_t slot, int16_t x, uint8_t y);

/* Set y = 240 (offscreen). Idiomatic SNES "hide". */
void mg_sprite_hide(uint8_t slot);

/* Hide every slot — sets shadow OAM to all-y=240, resets dirty
 * watermarks so a frame that clears + sets 32 sprites only DMAs the
 * touched range. */
void mg_sprites_clear_all(void);

/* -------- Global OAM config (call at init) -------- */

/* Pick the OBSEL small / large pair for the whole OAM. */
void mg_sprite_sizes(MgSpriteSizes pair);

/* Set the two CHR base addresses inside VRAM that the 9-bit tile
 * number indexes into. Standard SNES OBSEL convention; pass word
 * addresses (byte_address / 2). */
void mg_sprite_chr_base(uint16_t base0_word, uint16_t base1_word);

/* -------- Cooperative slot allocator (optional) -------- */

/* Reserve a contiguous range of slots. Returns the first slot
 * (0..127) or -1 if too few free slots remain. Auto-released on VM
 * exit. Direct slot access via mg_sprite_set still works without
 * going through the allocator — this is cooperation, not
 * enforcement. */
int  mg_sprite_alloc(uint8_t count);

/* Voluntary release. Optional; VM exit covers it. */
void mg_sprite_free (uint8_t first, uint8_t count);

#ifdef __cplusplus
}
#endif

#endif /* MG_SPRITE_H */
