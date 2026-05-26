/* ============================================================
 *  trig_q16.h — Q16.16 fixed-point trig via CORDIC.
 *
 *  Integer-only: no FPU, no libm, no runtime table-fill. CORDIC
 *  rotation mode for sin/cos, vectoring mode for atan2; ~16 shift/
 *  add iterations per call, deterministic, accurate to ~2^-15.
 *
 *  Angles are Q16.16 RADIANS (matches the float prototype code).
 *  Not a per-pixel path — it's for camera roll/FOV, the course
 *  generator's heading walk, Mode 7 matrix setup, etc.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef TRIG_Q16_H
#define TRIG_Q16_H

#include "math/fixed_point.h"

#ifdef __cplusplus
extern "C" {
#endif

#define Q16_PI      ((q16_16_t)205887)   /* pi      * 65536 */
#define Q16_TWO_PI  ((q16_16_t)411775)   /* 2*pi    * 65536 */
#define Q16_HALF_PI ((q16_16_t)102944)   /* pi/2    * 65536 */

/* sin and cos of the same angle in one pass (the CORDIC produces both).
 * Either out pointer may be NULL. */
void q16_sincos(q16_16_t radians, q16_16_t *sin_out, q16_16_t *cos_out);

q16_16_t q16_sin(q16_16_t radians);
q16_16_t q16_cos(q16_16_t radians);
q16_16_t q16_tan(q16_16_t radians);                 /* saturates near +-pi/2 */

/* atan2(y, x) in Q16.16 radians, range (-pi, pi]. (0,0) -> 0. */
q16_16_t q16_atan2(q16_16_t y, q16_16_t x);

#ifdef __cplusplus
}
#endif

#endif /* TRIG_Q16_H */
