/* ============================================================
 *  mg_bg.h — BG layers for cart-side games.
 *
 *  Two write paths to the tilemap:
 *
 *    SHADOW (mg_bg_set_tile, mg_bg_get_tile, mg_bg_blit) — writes
 *    to a host-side shadow tilemap with no DMA cost. The runtime
 *    tracks dirty range per layer; mg_frame_commit emits one DMA
 *    per dirty range. Good for static scenes + HUDs.
 *
 *    DIRECT (mg_bg_upload) — bypasses shadow, queues ONE DMA slot,
 *    costs `n*2` bytes against budget. Caller is responsible for
 *    keeping shadow in sync if it matters. Good for stream-in scroll
 *    worlds where you know exactly which cells are new this frame.
 *
 *  Both paths wrap in row-major past the right edge — past (31, 5)
 *  continues at (0, 6) on a 32x32 tilemap; past (31, 31) wraps to
 *  (0, 0). Useful for the "instantaneous transport" effect.
 *
 *  Shadow capped at 4 layers × 32x32 = 8 KB total host DTCM. Larger
 *  tilemaps (64x32, 32x64, 64x64) must use the direct path only.
 *
 *  See docs/game-api.md for design rationale.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_BG_H
#define MG_BG_H

#include <stdint.h>
#include <stdbool.h>

#include "mg_panic.h"   /* MgResult */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MG_BG_MODE_0 = 0,  /* 4 BGs x 2bpp                                */
    MG_BG_MODE_1 = 1,  /* BG1+2 4bpp, BG3 2bpp — the workhorse        */
    MG_BG_MODE_2 = 2,  /* BG1+2 4bpp, offset-per-tile                 */
    MG_BG_MODE_3 = 3,  /* BG1 8bpp, BG2 4bpp                          */
    MG_BG_MODE_4 = 4,
    MG_BG_MODE_5 = 5,  /* hi-res 512x224                              */
    MG_BG_MODE_6 = 6,
    MG_BG_MODE_7 = 7,  /* matrix-transformed single 8bpp — see mg_mode7.h */
} MgBgMode;

typedef enum {
    MG_BG_LAYER_1 = 0,
    MG_BG_LAYER_2 = 1,
    MG_BG_LAYER_3 = 2,
    MG_BG_LAYER_4 = 3,
} MgBgLayer;

typedef enum {
    MG_BG_SIZE_32x32 = 0,
    MG_BG_SIZE_64x32 = 1,  /* horizontal scroll-world */
    MG_BG_SIZE_32x64 = 2,  /* vertical               */
    MG_BG_SIZE_64x64 = 3,
} MgBgSize;

/* MgBgTile is a transparent union: write fields by name, read
 * `.word` for the raw 16-bit SNES tilemap word the runtime stages.
 * Layout matches SNES native tilemap word exactly. */
typedef union {
    struct {
        uint16_t tile     : 10;  /* 0..1023                              */
        uint16_t palette  : 3;   /* 0..7                                  */
        uint16_t priority : 1;   /* layer hi vs lo                        */
        uint16_t hflip    : 1;
        uint16_t vflip    : 1;
    };
    uint16_t word;
} MgBgTile;

/* -------- Mode + per-layer setup -------- */

/* Picks the global PPU mode. Affects which layers exist and which
 * are 4bpp / 8bpp / 2bpp. Mode 7 is configured via mg_mode7_set()
 * (see mg_mode7.h) instead of mg_bg_setup. */
void mg_bg_mode  (MgBgMode mode);

/* Configure one layer's VRAM layout. `tilemap_word` is the VRAM word
 * address of the tilemap base (32x32 = 2 KB tilemap, 64x64 = 8 KB).
 * `chr_word` is the VRAM word base for tile 0 in this layer's CHR. */
void mg_bg_setup (MgBgLayer layer, uint16_t tilemap_word,
                  MgBgSize size, uint16_t chr_word);

/* Show / hide a layer on the main screen and/or subscreen (the
 * latter is for color-math compositing). PPU register write at
 * vblank, no DMA cost. */
void mg_bg_enable(MgBgLayer layer, bool main_screen, bool sub_screen);

/* -------- Tilemap writes (shadow path) -------- */

void mg_bg_set_tile(MgBgLayer layer, uint8_t x, uint8_t y, MgBgTile cell);
void mg_bg_get_tile(MgBgLayer layer, uint8_t x, uint8_t y, MgBgTile *out);

/* Bulk shadow blit. `n` cells copied row-major starting at (x, y),
 * wrapping past the right edge. */
void mg_bg_blit    (MgBgLayer layer, uint8_t x, uint8_t y,
                    const MgBgTile *cells, uint16_t n);

/* -------- Tilemap writes (direct path) -------- */

/* Bypass shadow; stage `cells` into the cart window and queue ONE
 * DMA slot targeting the layer's tilemap at (x, y) with row-major
 * wrap. Costs 1 slot + n*2 bytes against budget. */
MgResult mg_bg_upload(MgBgLayer layer, uint8_t x, uint8_t y,
                      const MgBgTile *cells, uint16_t n);

/* -------- Scroll + simple effects -------- */

/* Set BG H/V scroll. PPU register write (BGxHOFS / BGxVOFS), staged
 * in the runtime's register batch. No DMA cost. */
void mg_bg_scroll       (MgBgLayer layer, int16_t hx, int16_t vy);

/* Mosaic effect: 0 = off, 1..15 = block size in pixels. `layer_mask`
 * is a 4-bit mask (bit 0 = BG1, etc.). */
void mg_bg_mosaic       (uint8_t size, uint8_t layer_mask);

/* The layer's main-priority bit (OR'd with per-tile priority). Most
 * games leave this at 0 and use per-tile MgBgTile.priority. */
void mg_bg_main_priority(MgBgLayer layer, bool hi);

#ifdef __cplusplus
}
#endif

#endif /* MG_BG_H */
