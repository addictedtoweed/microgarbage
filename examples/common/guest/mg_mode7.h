/* ============================================================
 *  mg_mode7.h — Mode 7 for cart-side games.
 *
 *  Separate from mg_bg.h because Mode 7 has no per-tile concept;
 *  it's one big 8bpp 1024x1024 plane with a 2D affine transform.
 *  Activate with mg_bg_mode(MG_BG_MODE_7); from there mg_bg_setup
 *  is not used, and BG2..4 don't exist in Mode 7.
 *
 *  Perspective ("F-Zero", "canyon") is NOT a separate API. Use HDMA
 *  (mg_hdma.h) on the M7A..M7D targets to vary the matrix per
 *  scanline; the canyon demo does this.
 *
 *  See docs/game-api.md for design rationale.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_MODE7_H
#define MG_MODE7_H

#include <stdint.h>

#include "math/fixed_point.h"   /* q16_16_t — Q16.16 fixed-point */

#include "mg_panic.h"   /* MgResult */

#ifdef __cplusplus
extern "C" {
#endif

/* 2D affine matrix + center + scroll. All fields are 8.8 fixed-point
 * except scroll/center which are 13-bit signed (the SNES PPU limits).
 * Identity: a=d=256, b=c=0. */
typedef struct {
    int16_t a, b, c, d;
    int16_t cx, cy;
    int16_t hofs, vofs;
} MgMode7Params;

/* Wrap / fill behaviour at the edges of the 1024x1024 Mode 7 plane. */
typedef enum {
    MG_MODE7_WRAP        = 0,  /* wrap around (default)              */
    MG_MODE7_CLAMP       = 1,  /* clamp to edge tile                 */
    MG_MODE7_FILL_TILE0  = 2,  /* fill outside with tile 0           */
    MG_MODE7_FILL_BLACK  = 3,  /* fill outside with color 0          */
} MgMode7Wrap;

/* Set the static Mode 7 matrix + scroll. Stages M7A..M7D + M7X/M7Y
 * + M7HOFS/M7VOFS register writes into the vblank register batch.
 * No DMA cost. Only meaningful when mg_bg_mode(MG_BG_MODE_7) is
 * active. */
void mg_mode7_set (const MgMode7Params *p);

/* Set the wrap/fill behaviour. PPU register (M7SEL) write, no DMA
 * cost. */
void mg_mode7_wrap(MgMode7Wrap behavior);

/* Build a uniform scale + Z-rotation matrix in one call. `scale_q8`
 * is 8.8 fixed-point (256 = 1.0). `angle_q15` is 32768 = 2π. Writes
 * the matrix into the caller-owned MgMode7Params; cx/cy/hofs/vofs
 * are left untouched. Pure C, no ecall. */
void mg_mode7_scale_rotate(MgMode7Params *out,
                           uint16_t scale_q8, int16_t angle_q15);

/* -------- High-level camera abstraction --------
 *
 * MgMode7Camera puts a "fly over a flat plane" camera in front of the
 * raw MgMode7Params. Position lives in Q16.16 world-plane pixels
 * (the M7 plane is 1024×1024, so values up to ~1024.0 are useful).
 * `yaw` rotates the view CCW around the camera (also Q16.16 radians).
 * `zoom` is Q16.16 screen-to-world scale — 1.0 = 1 screen pixel covers
 * 1 world pixel, < 1.0 = wider field of view, > 1.0 = magnify.
 *
 * Forward direction at yaw=0 is +X. To move "forward N pixels":
 *
 *     q16_16_t s, c; q16_sincos(cam.yaw, &s, &c);
 *     cam.x += q16_mul(N_in_q16, c);
 *     cam.y += q16_mul(N_in_q16, s);
 *
 * mg_mode7_camera narrows the q16.16 components to the PPU's native
 * 8.8 matrix slots and 13-bit signed center coords. Pure C, no ecall;
 * stage the result with mg_mode7_set().  */
typedef struct {
    q16_16_t x, y;       /* world-plane position (Q16.16 pixels) */
    q16_16_t zoom;       /* Q16.16; Q16_ONE = 1:1 screen-to-world */
    q16_16_t yaw;        /* Q16.16 radians; wraps freely         */
} MgMode7Camera;

void mg_mode7_camera(const MgMode7Camera *cam, MgMode7Params *out);

#ifdef __cplusplus
}
#endif

#endif /* MG_MODE7_H */
