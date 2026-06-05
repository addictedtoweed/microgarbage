/* ============================================================
 *  mg_mode7.c — guest-side Mode 7 ecall stubs + helpers.
 *  See mg_mode7.h for the contract.
 *
 *  Trig path is the shared CORDIC library (math/trig_q16.h), not a
 *  private LUT — the same source rebuilds on the M7 as a thin wrapper
 *  around the H745's hardware CORDIC peripheral, so a guest that
 *  compiles on Windows runs the same matrix math on real silicon.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_mode7.h"
#include "vm_runtime.h"

#include "math/trig_q16.h"

void mg_mode7_set(const MgMode7Params *p) {
    (void)_vm_sys1(SYS_MG_MODE7_SET, (uint32_t)p);
}

void mg_mode7_wrap(MgMode7Wrap behavior) {
    (void)_vm_sys1(SYS_MG_MODE7_WRAP, (uint32_t)behavior);
}

/* ---- internal helpers --------------------------------------- */

/* Narrow a q16.16 matrix component to the PPU's 8.8 signed slot
 * (int16_t). The PPU range is ±127.something; saturate above/below. */
static int16_t q16_to_q8_8_sat(q16_16_t v) {
    /* q16.16 -> q8.8 is shift-right by 8. Saturate to int16. */
    int32_t r = (int32_t)(v >> 8);
    if (r >  32767) r =  32767;
    if (r < -32768) r = -32768;
    return (int16_t)r;
}

/* Narrow a q16.16 plane coordinate to the PPU's 13-bit signed M7X/Y
 * center field. Range is -4096..4095; saturate. The integer part of
 * q16.16 is the high 16 bits. */
static int16_t q16_to_m7center_sat(q16_16_t v) {
    int32_t r = (int32_t)(v >> 16);   /* integer part */
    if (r >  4095) r =  4095;
    if (r < -4096) r = -4096;
    return (int16_t)r;
}

/* ---- public helpers ----------------------------------------- */

void mg_mode7_scale_rotate(MgMode7Params *out,
                           uint16_t scale_q8, int16_t angle_q15) {
    /* angle_q15 has 32768 = 2π; convert to q16.16 radians.
     * Q16_TWO_PI / 32768 = scale factor — do the multiply in 64-bit
     * to dodge intermediate overflow at the max angle. */
    q16_16_t radians =
        (q16_16_t)(((int64_t)angle_q15 * (int64_t)Q16_TWO_PI) >> 15);

    q16_16_t s, c;
    q16_sincos(radians, &s, &c);

    /* scale (Q8.8) * trig (Q16.16) -> Q8.8 of the matrix slot.
     * scale_q8 << 16 promotes to Q16.16; q16_mul then yields Q16.16;
     * q16_to_q8_8_sat shifts right by 8 to land in Q8.8. */
    q16_16_t scale_q16 = (q16_16_t)((int32_t)scale_q8 << 8);
    out->a = q16_to_q8_8_sat(q16_mul(scale_q16,  c));
    out->b = q16_to_q8_8_sat(q16_mul(scale_q16, -s));
    out->c = q16_to_q8_8_sat(q16_mul(scale_q16,  s));
    out->d = q16_to_q8_8_sat(q16_mul(scale_q16,  c));
    /* cx/cy/hofs/vofs are left untouched — caller sets those. */
}

void mg_mode7_camera(const MgMode7Camera *cam, MgMode7Params *out) {
    q16_16_t s, c;
    q16_sincos(cam->yaw, &s, &c);

    /* The four matrix slots: zoom * R(yaw), expressed as Q16.16 then
     * narrowed to Q8.8 for the PPU. The negation on B carries the
     * CCW rotation convention that mat2_q16_rotation uses, so a
     * guest that already builds a mat2_q16 by hand will see matching
     * signs. */
    q16_16_t z = cam->zoom;
    out->a = q16_to_q8_8_sat(q16_mul(z,  c));
    out->b = q16_to_q8_8_sat(q16_mul(z, -s));
    out->c = q16_to_q8_8_sat(q16_mul(z,  s));
    out->d = q16_to_q8_8_sat(q16_mul(z,  c));

    /* Center: the screen-center pixel maps directly to (cam.x, cam.y)
     * on the world plane, so M7X/Y carry the camera position
     * (truncated to the PPU's 13-bit signed field). */
    out->cx = q16_to_m7center_sat(cam->x);
    out->cy = q16_to_m7center_sat(cam->y);

    /* Scroll: the camera abstraction folds position into M7X/Y, so
     * leave the BG1 scroll fields at zero -- the runtime adds them on
     * top of the matrix output and we don't want them double-counting. */
    out->hofs = 0;
    out->vofs = 0;
}

/* ============================================================
 *  Perspective camera ("3D" Mode 7 with horizon)
 * ============================================================ */

/* Internal: write a write-twice 16-bit little-endian value into
 * `dst` at offset `off`. The SNES PPU expects low-byte first. */
static inline void wr16le(uint8_t *dst, uint16_t off, int16_t v) {
    dst[off + 0] = (uint8_t)((uint16_t)v       & 0xFF);
    dst[off + 1] = (uint8_t)(((uint16_t)v >> 8) & 0xFF);
}

uint16_t mg_mode7_camera3d(const MgMode7Camera3D *cam,
                           uint8_t  *table_m7a,
                           uint8_t  *table_m7b,
                           uint8_t  *table_m7c,
                           uint8_t  *table_m7d,
                           MgMode7Params *out_static) {
    /* Yaw matrix is constant per frame -- cache the trig once. */
    q16_16_t s, c;
    q16_sincos(cam->base.yaw, &s, &c);

    const uint8_t h = cam->horizon_row;
    /* "Active" scanlines run from horizon_row .. 223. Lines above
     * get a (0,0,0,0) matrix that pairs with MG_MODE7_FILL_BLACK
     * to read as solid backdrop. */
    const uint16_t active_count = (h < 224u) ? (uint16_t)(224u - h) : 0u;
    uint16_t off = 0;

    /* ---- "Sky" segment: write a maxed-out diagonal scale matrix
     * ($7FFF = +127.996 in Q8.8) for `h` scanlines. The intent is to
     * push every sky-pixel's plane coordinate far outside the 1024×
     * 1024 plane bounds so MG_MODE7_FILL_BLACK actually fires and
     * the pixel reads as the backdrop color.
     *
     * Why not (0,0,0,0)? That collapses every screen pixel to the
     * single plane coord (M7X, M7Y) = (cam.x, cam.y), which IS in
     * plane range — FILL_BLACK doesn't trigger and the PPU samples
     * whatever tile happens to be at the camera's world position
     * (visible as a black/garbage band in early mode7_3d builds).
     *
     * Why $7FFF? Largest positive Q8.8 → multiplied by (sx - M7X)
     * up to 384 yields ~49000, well past the 1024 wrap boundary
     * for any screen pixel.
     *
     * Hybrid encoding: count = $80|N + N×2 data bytes per chunk;
     * bsnes-plus advances source per scanline regardless of the
     * repeat-mode bit (see INIDISP letterbox in copro_mg_state.c).
     * Cap at 127 lines per chunk; for h > 127 we split. */
    {
        uint16_t lines_left = h;
        while (lines_left > 0) {
            uint8_t chunk = (lines_left > 127u) ? 127u : (uint8_t)lines_left;
            uint8_t count = (uint8_t)(0x80u | chunk);
            table_m7a[off] = count;
            table_m7b[off] = count;
            table_m7c[off] = count;
            table_m7d[off] = count;
            off++;
            for (uint8_t i = 0; i < chunk; i++) {
                wr16le(table_m7a, off, (int16_t)0x7FFF);
                wr16le(table_m7b, off, 0);
                wr16le(table_m7c, off, 0);
                wr16le(table_m7d, off, (int16_t)0x7FFF);
                off += 2;
            }
            lines_left -= chunk;
        }
    }

    /* ---- Active segment: per-line M7 values for the ground.
     * Same $80|N hybrid encoding as the sky chunk so bsnes-plus
     * fires exactly one transfer per scanline. Cap at 127 lines per
     * chunk. */
    {
        uint16_t lines_left = active_count;
        uint16_t row = h;
        while (lines_left > 0) {
            uint8_t group = (lines_left > 127u) ? 127u : (uint8_t)lines_left;
            uint8_t count = (uint8_t)(0x80u | group);
            uint16_t count_off = off++;
            table_m7a[count_off] = count;
            table_m7b[count_off] = count;
            table_m7c[count_off] = count;
            table_m7d[count_off] = count;
            for (uint8_t i = 0; i < group; i++, row++) {
                /* depth_factor = height / (row - horizon + 1).
                 * +1 keeps the line just past the horizon from
                 * blowing up to infinity. Dividing Q16.16 by a
                 * plain integer stays in Q16.16 -- no q16_div
                 * needed because the divisor isn't itself
                 * fixed-point. */
                int32_t y_off = (int32_t)(row - h) + 1;
                q16_16_t depth = cam->height / y_off;

                /* Multiply by the camera's base zoom, then by the
                 * yaw matrix components. Same Q16.16 -> Q8.8
                 * narrowing as the flat camera. */
                q16_16_t z = q16_mul(depth, cam->base.zoom);
                int16_t m_a = q16_to_q8_8_sat(q16_mul(z,  c));
                int16_t m_b = q16_to_q8_8_sat(q16_mul(z, -s));
                int16_t m_c = q16_to_q8_8_sat(q16_mul(z,  s));
                int16_t m_d = q16_to_q8_8_sat(q16_mul(z,  c));

                wr16le(table_m7a, off, m_a);
                wr16le(table_m7b, off, m_b);
                wr16le(table_m7c, off, m_c);
                wr16le(table_m7d, off, m_d);
                off += 2;
            }
            lines_left -= group;
        }
    }

    /* ---- Terminator. */
    table_m7a[off] = 0;
    table_m7b[off] = 0;
    table_m7c[off] = 0;
    table_m7d[off] = 0;
    off += 1;

    /* Static part: M7X/Y carries world-plane center; M7A..D get
     * overwritten every scanline by HDMA but we fill them in case
     * the first vblank's HDMA hasn't fired yet. Use a flat
     * (identity * zoom) matrix so the first frame doesn't flash
     * garbage. */
    q16_16_t z = cam->base.zoom;
    out_static->a = q16_to_q8_8_sat(q16_mul(z,  c));
    out_static->b = q16_to_q8_8_sat(q16_mul(z, -s));
    out_static->c = q16_to_q8_8_sat(q16_mul(z,  s));
    out_static->d = q16_to_q8_8_sat(q16_mul(z,  c));
    out_static->cx = q16_to_m7center_sat(cam->base.x);
    out_static->cy = q16_to_m7center_sat(cam->base.y);
    out_static->hofs = 0;
    out_static->vofs = 0;

    return off;
}
