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
     * leave the BG1 scroll fields at zero — the runtime adds them on
     * top of the matrix output and we don't want them double-counting. */
    out->hofs = 0;
    out->vofs = 0;
}
