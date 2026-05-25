/* ============================================================
 *  ppu.h — SNES PPU rasterizer (renders state -> a framebuffer).
 *
 *  NOT a cycle-accurate emulator. It takes a snapshot of PPU state —
 *  the same VRAM / CGRAM / OAM / register payload the coprocessor would
 *  DMA to a real SNES during vblank — and rasterizes it to a 256x224
 *  RGBA8888 framebuffer (the format present.h wants). No 65816, no
 *  timing: the cart builds a static per-frame snapshot and this replays
 *  it. Per-scanline HDMA register changes are layered on later (the
 *  render loop is already scanline-structured for it).
 *
 *  THE SNAPSHOT CONTRACT (this struct) is a *decoded* view of the PPU:
 *  fields map 1:1 to the $21xx registers, but pre-decoded (scroll as a
 *  16-bit value, base addresses as VRAM word addresses, etc.) so the
 *  renderer doesn't re-derive them per pixel. The host fills it from the
 *  coprocessor's register writes via a thin adapter. Memory blocks
 *  (vram/cgram/oam) are byte/word-identical to the hardware.
 *
 *  Implemented now: Mode 0 + Mode 1 backgrounds, scroll, tile flip,
 *  priority compositing, backdrop, master brightness. Sprites, color
 *  math/windows, HDMA, and Mode 7 are follow-on increments.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VIDEO_PPU_H
#define VIDEO_PPU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PPU_SCREEN_W   256
#define PPU_SCREEN_H   224

#define PPU_VRAM_WORDS 0x8000   /* 64 KB, word-addressed (32768 words) */
#define PPU_CGRAM_LEN  256      /* BGR555 palette entries              */
#define PPU_OAM_LEN    544      /* 512-byte table + 32-byte high table */

/* BG screen-size codes (BGnSC bits 0-1). */
typedef enum {
    PPU_SC_32x32 = 0,
    PPU_SC_64x32 = 1,
    PPU_SC_32x64 = 2,
    PPU_SC_64x64 = 3,
} PpuScreenSize;

/* One background layer's decoded configuration. */
typedef struct {
    uint16_t      tilemap_word;  /* VRAM word address of the tilemap base   */
    uint16_t      char_word;     /* VRAM word address of the tile/char base */
    PpuScreenSize size;          /* tilemap arrangement                     */
    uint16_t      hofs, vofs;    /* scroll offsets in pixels (10-bit useful)*/
    bool          on_main;       /* enabled on the main screen (TM bit)     */
    bool          on_sub;        /* enabled on the sub screen (TS bit)      */
} PpuBg;

/* Decoded PPU snapshot — the per-frame contract the renderer consumes. */
typedef struct {
    uint8_t  mode;          /* BG mode 0..7 (0 and 1 rendered for now)      */
    bool     bg3_priority;  /* Mode 1 BG3-priority bit ($2105 bit 3)        */
    bool     forced_blank;  /* INIDISP bit 7 — screen forced black          */
    uint8_t  brightness;    /* INIDISP bits 0-3, 0..15 (15 = full)          */

    PpuBg    bg[4];         /* BG1..BG4 (BG3/BG4 used by Mode 0)            */

    /* Hardware memory, identical to the real chip. */
    uint16_t vram[PPU_VRAM_WORDS];
    uint16_t cgram[PPU_CGRAM_LEN];   /* BGR555: 0bbbbbgggggrrrrr            */
    uint8_t  oam[PPU_OAM_LEN];       /* (sprites: later increment)          */
} PpuState;

/* Tilemap entry bit layout (16-bit little-endian word in VRAM). */
#define PPU_TILE_NUM(e)   ((uint16_t)((e) & 0x03FFu))   /* bits 0-9   */
#define PPU_TILE_PAL(e)   ((uint8_t)(((e) >> 10) & 0x7u))/* bits 10-12 */
#define PPU_TILE_PRIO(e)  (((e) >> 13) & 1u)            /* bit 13     */
#define PPU_TILE_HFLIP(e) (((e) >> 14) & 1u)            /* bit 14     */
#define PPU_TILE_VFLIP(e) (((e) >> 15) & 1u)            /* bit 15     */

/* Render the snapshot to `fb` (PPU_SCREEN_W * PPU_SCREEN_H pixels, the
 * present.h RGBA byte order). `fb` must hold at least that many pixels. */
void ppu_render(const PpuState *ppu, uint32_t *fb);

/* Convenience: zero a state to a sane blank (mode 0, brightness full,
 * everything off, backdrop = CGRAM[0] = black). */
void ppu_state_clear(PpuState *ppu);

/* Expand a 15-bit BGR555 color to the present.h RGBA pixel format. */
uint32_t ppu_bgr555_to_rgba(uint16_t bgr555);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_PPU_H */
