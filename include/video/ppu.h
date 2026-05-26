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

/* Registers an HDMA channel (or a future timer-IRQ write) can drive
 * per scanline. More are added as color math / Mode 7 land. */
typedef enum {
    PPU_REG_NONE = 0,
    PPU_REG_BG1_HOFS, PPU_REG_BG1_VOFS,
    PPU_REG_BG2_HOFS, PPU_REG_BG2_VOFS,
    PPU_REG_BG3_HOFS, PPU_REG_BG3_VOFS,
    PPU_REG_BG4_HOFS, PPU_REG_BG4_VOFS,
    PPU_REG_BRIGHTNESS,
    PPU_REG_M7A, PPU_REG_M7B, PPU_REG_M7C, PPU_REG_M7D,  /* Mode 7 matrix */
    PPU_REG_M7X, PPU_REG_M7Y,                            /* rotation center */
    PPU_REG_M7HOFS, PPU_REG_M7VOFS,                      /* Mode 7 scroll */
} PpuRegId;

#define PPU_HDMA_MAX 8

/* One per-scanline register-replay channel. `value` points at a
 * host-owned array of PPU_SCREEN_H entries — the value `target` takes
 * on each scanline. The host expands its WRAM HDMA tables into these
 * (this is the render-from-state stand-in for the HDMA byte stream). */
typedef struct {
    PpuRegId        target;
    const uint16_t *value;   /* [PPU_SCREEN_H], or NULL for an unused slot */
} PpuHdmaChannel;

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

    /* Sprites (OBJ). The host decodes OBSEL/TM/TS into these. */
    bool     obj_on_main;   /* OBJ enabled on the main screen (TM bit 4)   */
    bool     obj_on_sub;    /* OBJ enabled on the sub screen  (TS bit 4)   */
    uint8_t  obj_size_sel;  /* OBSEL bits 5-7: which small/large size pair */
    uint16_t obj_char_word; /* VRAM word base of OBJ tile 0 (page 0)       */
    uint16_t obj_gap_word;  /* word offset of OBJ page 1 (tiles 256-511)   */

    /* Per-scanline register replay (HDMA / future timer-IRQ writes). */
    PpuHdmaChannel hdma[PPU_HDMA_MAX];
    unsigned       hdma_count;

    /* Color math (subscreen add/subtract) — the translucency path.
     * The main-screen pixel is combined with a second operand: the
     * sub-screen pixel (layers flagged on_sub) when cm_use_subscreen,
     * else the fixed color. Math applies only where the winning main
     * layer's cm-enable bit is set. (CGADSUB / CGWSEL / COLDATA.) */
    bool     cm_bg[4];          /* per-BG color-math enable        */
    bool     cm_obj;            /* OBJ color-math enable           */
    bool     cm_backdrop;       /* backdrop color-math enable      */
    bool     cm_subtract;       /* subtract instead of add         */
    bool     cm_half;           /* halve the result                */
    bool     cm_use_subscreen;  /* 2nd operand: subscreen vs fixed */
    uint16_t cm_fixed_color;    /* fixed-color operand (BGR555)    */

    /* Mode 7 (affine BG1, 256-color). Matrix A-D is 8.8 fixed; the
     * center (X,Y) and scroll (HOFS,VOFS) are 13-bit signed (host
     * sign-extends into int16). VRAM is interleaved: tilemap = low
     * bytes of words 0..0x3FFF (128x128 1-byte entries), char data =
     * high bytes (256 tiles x 8x8 x 8bpp). Out-of-range texels wrap. */
    int16_t  m7a, m7b, m7c, m7d;
    int16_t  m7x, m7y;
    int16_t  m7hofs, m7vofs;
    bool     m7_over_transparent; /* M7SEL: texels outside the 1024x1024 plane are
                                   * transparent (backdrop) instead of wrapping */

    /* Hardware memory, identical to the real chip.
     *   oam[0..511]   = low table: 4 bytes/sprite x 128:
     *                   X(lo8), Y, tile(lo8), attr(N|ppp|oo|h|v)
     *   oam[512..543] = high table: 2 bits/sprite (X hi bit, size bit). */
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
