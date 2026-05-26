/* ============================================================
 *  vec2_q16.h — Q16.16 fixed-point 2D vector.
 *
 *  Companion to vec3_q16.h for the 2D side (Mode 7 / affine /
 *  sprite work). Header-only, static inline, built on
 *  math/fixed_point.h. Same range notes as vec3_q16: dot/length
 *  results must fit Q16.16; normalize() takes its length^2 in
 *  int64 so it is correct for any representable vector.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VEC2_Q16_H
#define VEC2_Q16_H

#include "math/fixed_point.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { q16_16_t x, y; } vec2_q16;

static inline vec2_q16 vec2_q16_make(q16_16_t x, q16_16_t y) {
    vec2_q16 v = { x, y }; return v;
}
static inline vec2_q16 vec2_q16_from_int(int x, int y) {
    return vec2_q16_make(q16_from_int(x), q16_from_int(y));
}

static inline vec2_q16 vec2_q16_add(vec2_q16 a, vec2_q16 b) {
    return vec2_q16_make(a.x + b.x, a.y + b.y);
}
static inline vec2_q16 vec2_q16_sub(vec2_q16 a, vec2_q16 b) {
    return vec2_q16_make(a.x - b.x, a.y - b.y);
}
static inline vec2_q16 vec2_q16_scale(vec2_q16 a, q16_16_t s) {
    return vec2_q16_make(q16_mul(a.x, s), q16_mul(a.y, s));
}
static inline vec2_q16 vec2_q16_perp(vec2_q16 a) {     /* 90deg CCW */
    return vec2_q16_make(-a.y, a.x);
}

static inline q16_16_t vec2_q16_dot(vec2_q16 a, vec2_q16 b) {
    int64_t d = (int64_t)a.x * b.x + (int64_t)a.y * b.y;
    return (q16_16_t)(d >> Q16_FRAC_BITS);
}
/* z of the 3D cross of (a,0) x (b,0): a.x*b.y - a.y*b.x (signed area). */
static inline q16_16_t vec2_q16_cross(vec2_q16 a, vec2_q16 b) {
    int64_t c = (int64_t)a.x * b.y - (int64_t)a.y * b.x;
    return (q16_16_t)(c >> Q16_FRAC_BITS);
}

static inline q16_16_t vec2_q16_length(vec2_q16 v) {
    int64_t s = ((int64_t)v.x * v.x + (int64_t)v.y * v.y) >> Q16_FRAC_BITS;
    if (s <= 0) return 0;
    return q16_sqrt((q16_16_t)s);
}
static inline vec2_q16 vec2_q16_normalize(vec2_q16 v) {
    int64_t s = ((int64_t)v.x * v.x + (int64_t)v.y * v.y) >> Q16_FRAC_BITS;
    if (s <= 0) return v;
    q16_16_t r = (q16_16_t)fx_isqrt64(((uint64_t)1 << 48) / (uint64_t)s);
    return vec2_q16_scale(v, r);
}

#ifdef __cplusplus
}
#endif

#endif /* VEC2_Q16_H */
