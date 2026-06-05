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

/* -------- Perspective ("3D") camera with horizon --------
 *
 * Add a horizon to the flat camera and the scene reads as a tilted
 * plane receding into the distance -- F-Zero / Mario Kart's
 * "looking-at-the-horizon" effect. The trick on real hardware is
 * HDMA on M7A..M7D: every scanline gets its own matrix, and the
 * matrix's effective scale grows with screen-Y so the ground looks
 * further away closer to the horizon.
 *
 * `horizon_row` is the scanline where the horizon sits. Lines
 * strictly ABOVE the horizon get a degenerate (0,0,0,0) matrix
 * which, paired with MG_MODE7_FILL_BLACK in mg_mode7_wrap, draws as
 * solid backdrop -- the "sky." Lines below get a 1/y perspective
 * scaled by `height`, so a higher camera shows less ground per
 * pixel (the world is further away).
 *
 * mg_mode7_camera3d builds the four HDMA tables in caller-owned
 * buffers and returns the bytes used (same for all four). The
 * caller wires them with mg_hdma_setup/upload_table/enable on four
 * channels (1..6 are free; 0 is the runtime's DMA list dispatch
 * and 7 is the INIDISP letterbox).  */
typedef struct {
    MgMode7Camera base;        /* x, y, zoom, yaw -- same flat-cam fields  */
    q16_16_t      height;      /* Q16.16; bigger = camera further from plane */
    uint8_t       horizon_row; /* 0..223; lines above draw as backdrop      */
} MgMode7Camera3D;

/* Per-table size budget. The M7A..D tables and the M7SEL table are
 * mode-0/mode-2 (1 or 2 bytes per scanline) so 512 bytes is more
 * than enough. The combined HOFS+VOFS table is mode-3 (4 bytes per
 * scanline) so its worst case is 224*4 + chunk counts + terminator
 * ≈ 900 bytes — declare it separately as 1024. */
#define MG_MODE7_3D_TABLE_BYTES    512
#define MG_MODE7_3D_HV_TABLE_BYTES 1024

/* Build 6 HDMA tables for the camera (M7A/B/C/D + BG1HOFS + BG1VOFS).
 * Returns the number of bytes written into each table — pass that as
 * `len` to mg_hdma_upload_table. out_static gets the camera's static
 * fields (M7X/Y set to 0; scroll zeroed since HOFS/VOFS now come
 * from HDMA); pass it to mg_mode7_set. Pure C, no ecall.
 *
 * The per-scanline HOFS/VOFS values are computed so that camera
 * translation produces uniform plane-coord shifts across all rows
 * (no skew). Specifically:
 *
 *     HOFS_at_row = (cos·cam.x + sin·cam.y) / z_at_row
 *     VOFS_at_row = (-sin·cam.x + cos·cam.y) / z_at_row
 *
 * which, fed into the SNES Mode-7 formula plane_x = A·(X' + HOFS) +
 * B·(Y' + VOFS), exactly cancels the per-row z multiplier. The
 * effect: moving the camera 1 unit forward slides every scanline's
 * sampled plane region by 1 unit too, like F-Zero. With the prior
 * (HOFS = constant) approach the same move produced shifts of z
 * units per scanline -- visible as a strong "shear" toward the
 * horizon. */
/* Returned struct gives the byte count for each table type. The 4
 * M7A..D tables share the same size (mode-2: 2 bytes/scanline); the
 * combined HOFS+VOFS table is mode-3 (4 bytes/scanline) and is
 * larger. Callers upload each table with its respective count. */
typedef struct {
    uint16_t bytes_m7;    /* size of each M7A..D table */
    uint16_t bytes_hv;    /* size of the HOFS+VOFS combined table */
} MgMode7TableSizes;

MgMode7TableSizes mg_mode7_camera3d(const MgMode7Camera3D *cam,
                                    uint8_t  *table_m7a,
                                    uint8_t  *table_m7b,
                                    uint8_t  *table_m7c,
                                    uint8_t  *table_m7d,
                                    uint8_t  *table_hv,
                                    MgMode7Params *out_static);

/* Build a static M7SEL HDMA table that switches "screen over" mode
 * per scanline so the sky band uses MG_MODE7_FILL_BLACK (out-of-plane
 * → backdrop = a solid sky color) and the active band uses MG_MODE7_
 * WRAP (plane coords wrap mod 1024 → no FILL_BLACK wedges from per-
 * scanline-varying matrices). Call once at demo startup; the table
 * doesn't depend on the camera. Returns bytes written. */
uint16_t mg_mode7_build_m7sel_table(uint8_t *table, uint8_t horizon_row);

#ifdef __cplusplus
}
#endif

#endif /* MG_MODE7_H */
