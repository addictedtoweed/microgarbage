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
    /* Minimal backdrop-only demo (simplified from the original rainbow
     * version during the v1.x debugging arc). BG2 is enabled with no
     * CHR upload and no tilemap blit, so BG2 reads zeros from VRAM and
     * renders fully transparent; the screen shows backdrop CGRAM[0]
     * everywhere. CGRAM[0] is re-set every frame so the dirty range
     * stays non-zero (a defensive habit from the v1.17 race-fix era;
     * post-v1.17 the once-at-setup write is sufficient too). */
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_2, 0x0400, MG_BG_SIZE_32x32, 0x2000);
    mg_bg_enable(MG_BG_LAYER_2, /*main=*/true, /*sub=*/false);

    mg_palette_set_rgb(0, 255, 0, 0);   /* backdrop = bright red */

    for (;;) {
        mg_palette_set_rgb(0, 255, 0, 0);
        mg_frame_commit();
        mg_wait_frame();
    }
    (void)SOLID_CHR;
    (void)rainbow;
    (void)sys_exit;
}
