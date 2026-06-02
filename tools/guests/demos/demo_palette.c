/* ============================================================
 *  demo_palette.c — solid-color BG with a cycling color-1 palette.
 *
 *  Smallest possible "the seam works" demo for cart-side games:
 *  one CHR tile (all color 1), 32×32 tilemap of that tile, and a
 *  per-frame mg_palette_set_rgb() that walks color 1 through a
 *  rainbow. No DMA per frame past the initial CGRAM upload — every
 *  later frame is "shadow-only writes" + the runtime emitting one
 *  small CGRAM range from its dirty watermarks. D-pad LEFT / RIGHT
 *  slow / speed the cycle; START exits.
 *
 *  Demonstrates: mg_bg_mode + mg_bg_setup + mg_bg_enable, mg_chr_upload,
 *  mg_bg_blit, mg_palette_set_rgb, the shadow-OAM/CGRAM commit path.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_game.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* One 4bpp tile, all pixels = palette index 1.
 * 4bpp planar = 4 planes × 8 rows × 1 byte, interleaved by row pair:
 *   bytes 0,1   = row 0 plane 0, plane 1
 *   bytes 2,3   = row 1 plane 0, plane 1
 *   ...
 *   bytes 16,17 = row 0 plane 2, plane 3
 *   ...
 * For "all pixels = 1": plane 0 = 0xFF, planes 1-3 = 0. */
static const uint8_t SOLID_CHR[32] = {
    0xFF, 0,  0xFF, 0,  0xFF, 0,  0xFF, 0,
    0xFF, 0,  0xFF, 0,  0xFF, 0,  0xFF, 0,
    0,    0,  0,    0,  0,    0,  0,    0,
    0,    0,  0,    0,  0,    0,  0,    0,
};

/* Tiny rainbow palette walk — cheaper than sin/cos. 12 entries
 * cycling through R→Y→G→C→B→M→R, blended through the in-betweens
 * by phase, so the result looks continuous. */
static void rainbow(uint8_t phase, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint8_t sector  = (uint8_t)(phase / 43);   /* 0..5 (256/6 ≈ 43) */
    uint8_t frac    = (uint8_t)(phase - sector * 43);
    uint8_t up      = (uint8_t)((frac * 255) / 43);
    uint8_t down    = (uint8_t)(255 - up);
    switch (sector) {
        case 0: *r = 255;  *g = up;   *b = 0;    break;  /* R→Y */
        case 1: *r = down; *g = 255;  *b = 0;    break;  /* Y→G */
        case 2: *r = 0;    *g = 255;  *b = up;   break;  /* G→C */
        case 3: *r = 0;    *g = down; *b = 255;  break;  /* C→B */
        case 4: *r = up;   *g = 0;    *b = 255;  break;  /* B→M */
        default:*r = 255;  *g = 0;    *b = down; break;  /* M→R */
    }
}

void _start(void) {
    /* Mode 1; BG1 tilemap at VRAM word $0000, CHR at $1000.
     * BG1 CHR base is set via BG12NBA low nibble x $1000 -- only
     * $0000/$1000/$2000/.../$7000 are representable. $1000 is the
     * first $1000-aligned address past the 32x32 tilemap. */
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, 0x0000, MG_BG_SIZE_32x32, 0x1000);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);

    /* Upload our one solid-color tile to VRAM word $1000. */
    MG_OR_PANIC(mg_chr_upload(0x1000, SOLID_CHR, sizeof SOLID_CHR));

    /* Fill the 32×32 tilemap with tile 0 (the one we just uploaded).
     * mg_bg_blit takes a contiguous cells array and wraps row-major
     * past the right edge; one call fills the whole layer. */
    static MgBgTile tilemap_buf[32 * 32];
    MgBgTile cell = { .word = 0 };   /* tile=0, palette=0, flags=0 */
    for (int i = 0; i < 32 * 32; i++) tilemap_buf[i] = cell;
    mg_bg_blit(MG_BG_LAYER_1, 0, 0, tilemap_buf, 32 * 32);

    /* Backdrop (color 0) = black. Color 1 starts red. */
    mg_palette_set_rgb(0, 0, 0, 0);
    mg_palette_set_rgb(1, 255, 0, 0);

    uint8_t phase = 0;
    uint8_t step  = 1;   /* phase delta per frame, 1..6 */
    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) sys_exit(0);
        if (mg_pad_pressed(pads.p0, MG_BTN_LEFT)  && step > 1) step--;
        if (mg_pad_pressed(pads.p0, MG_BTN_RIGHT) && step < 6) step++;

        phase = (uint8_t)(phase + step);
        uint8_t r, g, b;
        rainbow(phase, &r, &g, &b);
        mg_palette_set_rgb(1, r, g, b);

        mg_frame_commit();
        mg_wait_frame();
    }
}
