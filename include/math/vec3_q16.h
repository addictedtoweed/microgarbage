/* ============================================================
 *  vec3_q16.h — Q16.16 fixed-point 3D vector.
 *
 *  The shared vector backbone for the fixed-point renderer port: ray
 *  directions, the camera basis, surface normals. Replaces the ad-hoc
 *  float V3 helpers that each module rolls today. Header-only, all
 *  static inline; built on math/fixed_point.h (no FPU, no libm).
 *
 *  Range notes (Q16.16 holds +-32767 with 2^-16 resolution):
 *    - add/sub/scale/cross are exact within range.
 *    - dot and length^2 accumulate in int64 then shift to Q16.16, so
 *      the RESULT must fit Q16.16 (|result| < 32767). Fine for the
 *      unit-ish vectors the renderer dots together.
 *    - length() is valid while the true length^2 < 32767 (length < ~181).
 *    - normalize() computes length^2 in int64 and reciprocal-roots it
 *      there, so it is correct for ANY representable vector (e.g. a
 *      heightfield normal with components in the hundreds) and returns
 *      a unit vector in Q16.16.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VEC3_Q16_H
#define VEC3_Q16_H

#include "math/fixed_point.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { q16_16_t x, y, z; } vec3_q16;

static inline vec3_q16 vec3_q16_make(q16_16_t x, q16_16_t y, q16_16_t z) {
    vec3_q16 v = { x, y, z }; return v;
}
static inline vec3_q16 vec3_q16_from_int(int x, int y, int z) {
    return vec3_q16_make(q16_from_int(x), q16_from_int(y), q16_from_int(z));
}

static inline vec3_q16 vec3_q16_add(vec3_q16 a, vec3_q16 b) {
    return vec3_q16_make(a.x + b.x, a.y + b.y, a.z + b.z);
}
static inline vec3_q16 vec3_q16_sub(vec3_q16 a, vec3_q16 b) {
    return vec3_q16_make(a.x - b.x, a.y - b.y, a.z - b.z);
}
static inline vec3_q16 vec3_q16_scale(vec3_q16 a, q16_16_t s) {
    return vec3_q16_make(q16_mul(a.x, s), q16_mul(a.y, s), q16_mul(a.z, s));
}

/* Dot product in Q16.16. Result must fit the format (unit-ish inputs). */
static inline q16_16_t vec3_q16_dot(vec3_q16 a, vec3_q16 b) {
    int64_t d = (int64_t)a.x * b.x + (int64_t)a.y * b.y + (int64_t)a.z * b.z;
    return (q16_16_t)(d >> Q16_FRAC_BITS);
}

static inline vec3_q16 vec3_q16_cross(vec3_q16 a, vec3_q16 b) {
    return vec3_q16_make(q16_mul(a.y, b.z) - q16_mul(a.z, b.y),
                         q16_mul(a.z, b.x) - q16_mul(a.x, b.z),
                         q16_mul(a.x, b.y) - q16_mul(a.y, b.x));
}

/* Length in Q16.16. Valid while the true length^2 fits Q16.16
 * (length < ~181). Use normalize() for arbitrary-magnitude vectors. */
static inline q16_16_t vec3_q16_length(vec3_q16 v) {
    int64_t s = ((int64_t)v.x * v.x + (int64_t)v.y * v.y + (int64_t)v.z * v.z)
                >> Q16_FRAC_BITS;
    if (s <= 0) return 0;
    return q16_sqrt((q16_16_t)s);
}

/* Unit vector. length^2 is taken in int64 and reciprocal-rooted there
 * (same isqrt(2^48 / s) trick as q16_rsqrt), so this is correct for any
 * representable vector, not just ones whose length^2 fits Q16.16.
 * Returns the input unchanged for a zero vector. */
static inline vec3_q16 vec3_q16_normalize(vec3_q16 v) {
    int64_t s = ((int64_t)v.x * v.x + (int64_t)v.y * v.y + (int64_t)v.z * v.z)
                >> Q16_FRAC_BITS;                       /* Q16.16 length^2, in int64 */
    if (s <= 0) return v;
    q16_16_t r = (q16_16_t)fx_isqrt64(((uint64_t)1 << 48) / (uint64_t)s);
    return vec3_q16_scale(v, r);
}

#ifdef __cplusplus
}
#endif

#endif /* VEC3_Q16_H */
