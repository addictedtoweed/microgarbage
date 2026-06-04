/* ============================================================
 *  mg_gfx.h — CHR upload + palette + color packing for cart-side games.
 *
 *  CHR upload is one-call-one-DMA-slot: the runtime stages bytes into
 *  the cart-window payload area and queues a single VRAM-write DMA.
 *
 *  Palette uses shadow CGRAM (identical model to OAM): per-color
 *  writes are free, commit emits one DMA per dirty CGRAM range.
 *  Both BGR555-word and 8-bit RGB input formats are supported; the
 *  runtime packs RGB into BGR555 on the way in.
 *
 *  Bitmap → bitplane conversion (mg_pack_chr_*) is a host syscall
 *  rather than a guest-side bit-shuffle; the M7 does it fast in
 *  native code.
 *
 *  See docs/game-api.md for design rationale.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_GFX_H
#define MG_GFX_H

#include <stdint.h>

#include "mg_panic.h"   /* MgResult */

#ifdef __cplusplus
extern "C" {
#endif

/* -------- PPU clean slate (opt-in) -------- */

/* Reset the SNES PPU to a known-zero state on the next vblank:
 * VRAM/CGRAM/OAM all wiped, BG layers disabled, M7 matrix identity at
 * center (0,0), HDMA channels disabled, force-blank cleared, sprites
 * hidden at y=240. After calling this, the next mg_frame_commit's DMA
 * list will include the VRAM-clear + CGRAM/OAM zero writes.
 *
 * Voluntary. A child VM that wants to inherit its parent's loaded
 * graphics (e.g., a paged-overlay UI that builds on a kernel-loaded
 * tilemap) skips the call. A standalone demo calls it as the first
 * line of _start. Costs 1 DMA slot + 2 bytes of payload for the VRAM-
 * fill source; the actual 64KB VRAM DMA fits comfortably in vblank.
 *
 * Note: the host-side mg_state shadow is reset automatically on every
 * VM unload (vm_init.c's unload hook). This call is specifically for
 * clearing the PPU state itself, which the host can't touch except by
 * staging a DMA. */
void mg_ppu_clean_slate(void);

/* -------- CHR upload -------- */

/* Stage `bytes` of pre-packed planar CHR into the cart-window and
 * queue a VRAM-write DMA for next vblank. `vram_word` is the
 * destination word address (SNES VRAM is 32K words = 64KB).
 * Costs one DMA slot + `bytes` against the byte budget. */
MgResult mg_chr_upload(uint16_t vram_word, const void *src, uint16_t bytes);

/* -------- Palette (shadow CGRAM, free per-color writes) -------- */

/* Single-entry BGR555-word write. */
void mg_palette_set    (uint8_t idx, uint16_t bgr555);

/* Same, taking 8-bit RGB and packing for you. */
void mg_palette_set_rgb(uint8_t idx, uint8_t r, uint8_t g, uint8_t b);

/* Bulk: copy `count` BGR555 entries into shadow CGRAM starting at
 * `start`. Cheaper than a per-entry loop; still hits shadow only. */
void mg_palette_load      (uint8_t start, const uint16_t *bgr555_src, uint16_t count);

/* Bulk RGB24 form: `count` entries of 3 bytes each (R, G, B). The
 * runtime packs to BGR555 on the way into shadow. Useful when colors
 * come from an online picker or RGB asset format. */
void mg_palette_load_rgb24(uint8_t start, const uint8_t *rgb24_src, uint16_t count);

/* Snapshot all 512 bytes of shadow CGRAM into `out`. Used by a
 * cooperating parent VM before spawning a graphics-heavy child. */
MgResult mg_palette_snapshot(uint16_t *out_512_bytes);

/* Push a previously-snapshotted buffer back to shadow CGRAM. Costs
 * one DMA slot + 512 bytes (whole CGRAM is dirty after restore). */
MgResult mg_palette_restore (const uint16_t *in_512_bytes);

/* -------- Color packing helper (pure C, no ecall) -------- */

/* Pack 8-bit RGB into BGR555 with the high bit cleared. Cheap inline
 * since the 5-bit quantization is just `>> 3`. */
static inline uint16_t mg_bgr555(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((b >> 3) << 10) |
                      ((g >> 3) <<  5) |
                       (r >> 3));
}

/* -------- Bitmap → bitplane (host-native) -------- */

/* Pack a linear 4bpp bitmap (one byte per pixel, low nibble = color)
 * into SNES native 4bpp planar tiles (32 bytes per tile, four planes
 * interleaved by row). `src_linear` is tile_count * 64 bytes in size,
 * `dst` receives tile_count * 32 bytes. */
MgResult mg_pack_chr_4bpp(void *dst, const void *src_linear, uint16_t tile_count);

/* Same for 8bpp: 64 source bytes / tile → 64 packed bytes / tile,
 * eight planes interleaved. */
MgResult mg_pack_chr_8bpp(void *dst, const void *src_linear, uint16_t tile_count);

#ifdef __cplusplus
}
#endif

#endif /* MG_GFX_H */
