/* ============================================================
 *  mg_mode7.c — guest-side Mode 7 ecall stubs + helper.
 *  See mg_mode7.h for the contract.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_mode7.h"
#include "vm_runtime.h"

void mg_mode7_set(const MgMode7Params *p) {
    (void)_vm_sys1(SYS_MG_MODE7_SET, (uint32_t)p);
}

void mg_mode7_wrap(MgMode7Wrap behavior) {
    (void)_vm_sys1(SYS_MG_MODE7_WRAP, (uint32_t)behavior);
}

/* Sine in Q15 (32767 = 1.0) for angles in 8.8 quadrants. 17 entries
 * cover 0..π/2 in steps of (π/2)/16 ≈ 5.625°. We linear-interp
 * between adjacent entries and use symmetry to extend to a full
 * circle. Total ROM cost: 17 × 2 = 34 bytes. */
static const int16_t s_sin_q15_quarter[17] = {
        0,  3212,  6393,  9512, 12539, 15446, 18204, 20787,
    23170, 25329, 27245, 28898, 30273, 31356, 32137, 32609, 32767,
};

/* sin(angle_q15) where angle_q15 is the angle scaled so 32768 = 2π. */
static int32_t mg_sin_q15(int16_t angle_q15) {
    /* Wrap into 0..65535 then split into quadrant + position. */
    uint16_t a = (uint16_t)angle_q15;
    unsigned quadrant = (a >> 14) & 3;   /* 0..3 over the circle      */
    unsigned pos      = a & 0x3FFF;      /* 0..16383 inside a quadrant */

    /* Map pos to an index into the 17-entry quarter table (which
     * covers idx 0..16). 16384 / 1024 = 16 — so each idx step is
     * 1024 of position. */
    unsigned idx = pos >> 10;
    unsigned frac = pos & 0x3FF;         /* 0..1023 between idx/idx+1 */

    int32_t s0 = s_sin_q15_quarter[idx];
    int32_t s1 = s_sin_q15_quarter[idx + 1];   /* idx maxes at 15;
                                                 * the +1 reads idx 16
                                                 * which holds 32767. */
    int32_t v  = s0 + ((s1 - s0) * (int32_t)frac >> 10);

    /* Quadrant flip + sign. */
    switch (quadrant) {
        case 0: return v;
        case 1: {  /* mirror: sin(π - x) = sin(x), read backwards */
            int32_t s0b = s_sin_q15_quarter[16 - idx];
            int32_t s1b = s_sin_q15_quarter[15 - idx];
            return s0b + ((s1b - s0b) * (int32_t)frac >> 10);
        }
        case 2: return -v;
        case 3: {
            int32_t s0b = s_sin_q15_quarter[16 - idx];
            int32_t s1b = s_sin_q15_quarter[15 - idx];
            return -(s0b + ((s1b - s0b) * (int32_t)frac >> 10));
        }
    }
    return v;   /* unreachable */
}

static int32_t mg_cos_q15(int16_t angle_q15) {
    /* cos = sin(angle + π/2); π/2 = 16384 in this Q15 angle space. */
    return mg_sin_q15((int16_t)(angle_q15 + 16384));
}

void mg_mode7_scale_rotate(MgMode7Params *out,
                           uint16_t scale_q8, int16_t angle_q15) {
    int32_t s = mg_sin_q15(angle_q15);   /* Q15 */
    int32_t c = mg_cos_q15(angle_q15);   /* Q15 */

    /* matrix component = scale * trig, output as 8.8 fixed-point.
     * scale is 8.8, trig is Q15. Product / 32768 keeps the result
     * in 8.8 (since 256 * 32768 / 32768 = 256 = 1.0 8.8). */
    out->a = (int16_t)((int32_t)scale_q8 *  c / 32768);
    out->b = (int16_t)((int32_t)scale_q8 * -s / 32768);
    out->c = (int16_t)((int32_t)scale_q8 *  s / 32768);
    out->d = (int16_t)((int32_t)scale_q8 *  c / 32768);
    /* cx/cy/hofs/vofs are left untouched — caller sets those. */
}
