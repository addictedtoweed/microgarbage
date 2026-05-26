/* ============================================================
 *  fixed_point.h — Q-format fixed-point arithmetic
 *
 *  Five formats supported:
 *    q15_t     int16   1 sign +  0 int + 15 frac, ±1.0  resolution 2^-15
 *    q31_t     int32   1 sign +  0 int + 31 frac, ±1.0  resolution 2^-31
 *    q16_16_t  int32   1 sign + 15 int + 16 frac, ±32k  resolution 2^-16
 *    q32_32_t  int64   1 sign + 31 int + 32 frac, ±2e9  resolution 2^-32
 *    q48_16_t  int64   1 sign + 47 int + 16 frac, ±1.4e14 resolution 2^-16
 *
 *  Naming: <format>_<op>(...). For example:
 *    q16_add, q16_mul, q16_div, q16_from_int, q16_to_float
 *    q32_add, q32_mul, q32_sat_add
 *
 *  Saturation: plain operations (add, sub, mul) wrap on overflow,
 *  matching plain integer behavior. Variants prefixed sat_ saturate
 *  to format min/max. Reach for sat_* when doing audio sample math;
 *  reach for plain ops when doing phase accumulation (where wrap
 *  is correct behavior).
 *
 *  64-bit format multiply caveat: Q32.32 × Q32.32 produces a
 *  conceptually 128-bit intermediate. We implement it via 64-bit
 *  parts (Knuth-style long multiplication, 4 partial products).
 *  Same for Q48.16. These multiplies are correct but slower than
 *  the 32-bit-format ones. If you only need to scale a Q32.32 by
 *  a smaller value, use q32_scale_q16 which is much cheaper.
 *
 *  Float/double conversions are provided for every format. They're
 *  intended for constant initialization and test code; the compiler
 *  folds them at compile time when the input is a constant.
 *
 *  Almost everything is `static inline` in this header. The .c file
 *  exists for any non-inlinable helpers (none currently).
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef FIXED_POINT_H
#define FIXED_POINT_H

#include <stdint.h>

/* ============================================================
 *  Types and constants
 * ============================================================ */

typedef int16_t  q15_t;
typedef int32_t  q31_t;
typedef int32_t  q16_16_t;
typedef int64_t  q32_32_t;
typedef int64_t  q48_16_t;

#define Q15_FRAC_BITS   15
#define Q31_FRAC_BITS   31
#define Q16_FRAC_BITS   16
#define Q32_FRAC_BITS   32
#define Q48_FRAC_BITS   16

#define Q15_ONE         ((q15_t)0x7FFF)            /* max positive — true 1.0 unrepresentable in Q15 */
#define Q31_ONE         ((q31_t)0x7FFFFFFF)        /* max positive — true 1.0 unrepresentable in Q31 */
#define Q16_ONE         ((q16_16_t)((int32_t)1 << Q16_FRAC_BITS))    /* exactly 1.0 */
#define Q32_ONE         ((q32_32_t)((int64_t)1 << Q32_FRAC_BITS))    /* exactly 1.0 */
#define Q48_ONE         ((q48_16_t)((int64_t)1 << Q48_FRAC_BITS))    /* exactly 1.0 */

#define Q15_MAX  ((q15_t)INT16_MAX)
#define Q15_MIN  ((q15_t)INT16_MIN)
#define Q31_MAX  ((q31_t)INT32_MAX)
#define Q31_MIN  ((q31_t)INT32_MIN)
#define Q16_MAX  ((q16_16_t)INT32_MAX)
#define Q16_MIN  ((q16_16_t)INT32_MIN)
#define Q32_MAX  ((q32_32_t)INT64_MAX)
#define Q32_MIN  ((q32_32_t)INT64_MIN)
#define Q48_MAX  ((q48_16_t)INT64_MAX)
#define Q48_MIN  ((q48_16_t)INT64_MIN)

/* ============================================================
 *  Internal saturation helpers
 * ============================================================ */

static inline int32_t fx_sat32_(int64_t v) {
    if (v > (int64_t)INT32_MAX) return INT32_MAX;
    if (v < (int64_t)INT32_MIN) return INT32_MIN;
    return (int32_t)v;
}
static inline int16_t fx_sat16_(int64_t v) {
    if (v > (int64_t)INT16_MAX) return INT16_MAX;
    if (v < (int64_t)INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

/* Floor of the integer square root of a 64-bit value, with no FPU and
 * no division: the classic restoring bit-by-bit method, one step per
 * two bits. The building block for q16_sqrt / q16_rsqrt and vector
 * normalize. Branch-predictable; ~24 iterations for a 48-bit input. */
static inline uint32_t fx_isqrt64(uint64_t x) {
    uint64_t res = 0, bit = (uint64_t)1 << 62;   /* top even power of two */
    while (bit > x) bit >>= 2;
    while (bit != 0) {
        if (x >= res + bit) { x -= res + bit; res = (res >> 1) + bit; }
        else                  res >>= 1;
        bit >>= 2;
    }
    return (uint32_t)res;
}

/* ============================================================
 *  Q15 — int16, 15 fractional bits
 * ============================================================ */

static inline q15_t  q15_from_int(int x)        { return (q15_t)((int32_t)x << Q15_FRAC_BITS); }
static inline int    q15_to_int(q15_t x)        { return (int)(x >> Q15_FRAC_BITS); }
static inline q15_t  q15_from_float(float f)    { return (q15_t)(f * 32768.0f); }
static inline float  q15_to_float(q15_t x)      { return (float)x / 32768.0f; }
static inline q15_t  q15_from_double(double d)  { return (q15_t)(d * 32768.0); }
static inline double q15_to_double(q15_t x)     { return (double)x / 32768.0; }

static inline q15_t q15_add(q15_t a, q15_t b)   { return (q15_t)(a + b); }
static inline q15_t q15_sub(q15_t a, q15_t b)   { return (q15_t)(a - b); }
static inline q15_t q15_neg(q15_t a)            { return (q15_t)(-a); }
static inline q15_t q15_abs(q15_t a)            { return (q15_t)(a < 0 ? -a : a); }
static inline q15_t q15_mul(q15_t a, q15_t b) {
    return (q15_t)(((int32_t)a * (int32_t)b) >> Q15_FRAC_BITS);
}
static inline q15_t q15_div(q15_t a, q15_t b) {
    return (q15_t)(((int32_t)a << Q15_FRAC_BITS) / b);
}
static inline q15_t q15_mac(q15_t a, q15_t b, q15_t c) {
    return (q15_t)(a + (q15_t)(((int32_t)b * (int32_t)c) >> Q15_FRAC_BITS));
}
static inline q15_t q15_reciprocal(q15_t x) {
    return (q15_t)(((int32_t)Q15_ONE << Q15_FRAC_BITS) / x);
}

static inline q15_t q15_sat_add(q15_t a, q15_t b) { return fx_sat16_((int64_t)a + b); }
static inline q15_t q15_sat_sub(q15_t a, q15_t b) { return fx_sat16_((int64_t)a - b); }
static inline q15_t q15_sat_neg(q15_t a) { return (a == Q15_MIN) ? Q15_MAX : (q15_t)(-a); }
static inline q15_t q15_sat_abs(q15_t a) { return (a == Q15_MIN) ? Q15_MAX : (q15_t)(a < 0 ? -a : a); }
static inline q15_t q15_sat_mul(q15_t a, q15_t b) {
    return fx_sat16_(((int32_t)a * (int32_t)b) >> Q15_FRAC_BITS);
}

/* ============================================================
 *  Q31 — int32, 31 fractional bits
 * ============================================================ */

static inline q31_t  q31_from_int(int x)        { return (q31_t)((int32_t)x << Q31_FRAC_BITS); }
static inline int    q31_to_int(q31_t x)        { return (int)(x >> Q31_FRAC_BITS); }
static inline q31_t  q31_from_float(float f)    { return (q31_t)((double)f * 2147483648.0); }
static inline float  q31_to_float(q31_t x)      { return (float)((double)x / 2147483648.0); }
static inline q31_t  q31_from_double(double d)  { return (q31_t)(d * 2147483648.0); }
static inline double q31_to_double(q31_t x)     { return (double)x / 2147483648.0; }

static inline q31_t q31_add(q31_t a, q31_t b)   { return a + b; }
static inline q31_t q31_sub(q31_t a, q31_t b)   { return a - b; }
static inline q31_t q31_neg(q31_t a)            { return -a; }
static inline q31_t q31_abs(q31_t a)            { return a < 0 ? -a : a; }
static inline q31_t q31_mul(q31_t a, q31_t b) {
    return (q31_t)(((int64_t)a * (int64_t)b) >> Q31_FRAC_BITS);
}
static inline q31_t q31_div(q31_t a, q31_t b) {
    return (q31_t)(((int64_t)a << Q31_FRAC_BITS) / b);
}
static inline q31_t q31_mac(q31_t a, q31_t b, q31_t c) {
    return a + (q31_t)(((int64_t)b * (int64_t)c) >> Q31_FRAC_BITS);
}
static inline q31_t q31_reciprocal(q31_t x) {
    return (q31_t)(((int64_t)Q31_ONE << Q31_FRAC_BITS) / x);
}

static inline q31_t q31_sat_add(q31_t a, q31_t b) { return fx_sat32_((int64_t)a + b); }
static inline q31_t q31_sat_sub(q31_t a, q31_t b) { return fx_sat32_((int64_t)a - b); }
static inline q31_t q31_sat_neg(q31_t a) { return (a == Q31_MIN) ? Q31_MAX : -a; }
static inline q31_t q31_sat_abs(q31_t a) { return (a == Q31_MIN) ? Q31_MAX : (a < 0 ? -a : a); }
static inline q31_t q31_sat_mul(q31_t a, q31_t b) {
    return fx_sat32_(((int64_t)a * (int64_t)b) >> Q31_FRAC_BITS);
}

/* ============================================================
 *  Q16.16 — int32, 16 fractional bits
 * ============================================================ */

static inline q16_16_t q16_from_int(int x)        { return (q16_16_t)((int32_t)x << Q16_FRAC_BITS); }
static inline int      q16_to_int(q16_16_t x)     { return (int)(x >> Q16_FRAC_BITS); }
static inline int      q16_to_int_round(q16_16_t x) {
    return (int)((x + (Q16_ONE >> 1)) >> Q16_FRAC_BITS);
}
static inline q16_16_t q16_from_float(float f)    { return (q16_16_t)(f * 65536.0f); }
static inline float    q16_to_float(q16_16_t x)   { return (float)x / 65536.0f; }
static inline q16_16_t q16_from_double(double d)  { return (q16_16_t)(d * 65536.0); }
static inline double   q16_to_double(q16_16_t x)  { return (double)x / 65536.0; }

static inline q16_16_t q16_add(q16_16_t a, q16_16_t b) { return a + b; }
static inline q16_16_t q16_sub(q16_16_t a, q16_16_t b) { return a - b; }
static inline q16_16_t q16_neg(q16_16_t a)             { return -a; }
static inline q16_16_t q16_abs(q16_16_t a)             { return a < 0 ? -a : a; }
static inline q16_16_t q16_mul(q16_16_t a, q16_16_t b) {
    return (q16_16_t)(((int64_t)a * (int64_t)b) >> Q16_FRAC_BITS);
}
static inline q16_16_t q16_div(q16_16_t a, q16_16_t b) {
    return (q16_16_t)(((int64_t)a << Q16_FRAC_BITS) / b);
}
static inline q16_16_t q16_mac(q16_16_t a, q16_16_t b, q16_16_t c) {
    return a + (q16_16_t)(((int64_t)b * (int64_t)c) >> Q16_FRAC_BITS);
}
static inline q16_16_t q16_reciprocal(q16_16_t x) {
    return (q16_16_t)(((int64_t)Q16_ONE << Q16_FRAC_BITS) / x);
}

/* sqrt(x) in Q16.16. x must be >= 0. Exact for perfect squares,
 * floor-rounded otherwise. No FPU, no divide (~24 shift/compare steps).
 * Valid for the full positive Q16.16 range. */
static inline q16_16_t q16_sqrt(q16_16_t x) {
    if (x <= 0) return 0;
    return (q16_16_t)fx_isqrt64((uint64_t)(uint32_t)x << Q16_FRAC_BITS);
}

/* 1/sqrt(x) in Q16.16 (reciprocal square root). x must be > 0.
 * Identity: (2^24 / sqrt(x_raw)) == sqrt(2^48 / x_raw), so this is one
 * 64-bit divide + one isqrt — no FPU, no Newton iteration. This is the
 * per-pixel normalize hot path. Precision is excellent for small x and
 * degrades to ~1% for very large x; vec3_q16_normalize uses the same
 * trick on a 64-bit length^2 so it stays correct past the Q16.16 range. */
static inline q16_16_t q16_rsqrt(q16_16_t x) {
    if (x <= 0) return Q16_MAX;
    return (q16_16_t)fx_isqrt64(((uint64_t)1 << 48) / (uint64_t)(uint32_t)x);
}

static inline q16_16_t q16_sat_add(q16_16_t a, q16_16_t b) { return fx_sat32_((int64_t)a + b); }
static inline q16_16_t q16_sat_sub(q16_16_t a, q16_16_t b) { return fx_sat32_((int64_t)a - b); }
static inline q16_16_t q16_sat_neg(q16_16_t a) { return (a == Q16_MIN) ? Q16_MAX : -a; }
static inline q16_16_t q16_sat_abs(q16_16_t a) { return (a == Q16_MIN) ? Q16_MAX : (a < 0 ? -a : a); }
static inline q16_16_t q16_sat_mul(q16_16_t a, q16_16_t b) {
    return fx_sat32_(((int64_t)a * (int64_t)b) >> Q16_FRAC_BITS);
}

/* ============================================================
 *  Q32.32 — int64, 32 fractional bits
 *
 *  add/sub/neg/abs are trivial 64-bit ops.
 *  mul uses Knuth-style 4-partial-product 128-bit math via two
 *  64-bit halves. Slower than the 32-bit-format mul (~4x more
 *  multiplies) but doesn't require compiler 128-bit support.
 *  div is best-effort with 64-bit intermediate; for full range
 *  use the q32_scale_q16 helper if your scaling factor fits in
 *  Q16.16 — it's much cheaper.
 * ============================================================ */

static inline q32_32_t q32_from_int(int64_t x)   { return x << Q32_FRAC_BITS; }
static inline int64_t  q32_to_int(q32_32_t x)    { return x >> Q32_FRAC_BITS; }
static inline q32_32_t q32_from_float(float f)   { return (q32_32_t)((double)f * 4294967296.0); }
static inline float    q32_to_float(q32_32_t x)  { return (float)((double)x / 4294967296.0); }
static inline q32_32_t q32_from_double(double d) { return (q32_32_t)(d * 4294967296.0); }
static inline double   q32_to_double(q32_32_t x) { return (double)x / 4294967296.0; }

static inline q32_32_t q32_add(q32_32_t a, q32_32_t b) { return a + b; }
static inline q32_32_t q32_sub(q32_32_t a, q32_32_t b) { return a - b; }
static inline q32_32_t q32_neg(q32_32_t a)             { return -a; }
static inline q32_32_t q32_abs(q32_32_t a)             { return a < 0 ? -a : a; }

/* Q32.32 multiply using two 64-bit halves. The full result is
 * conceptually 128-bit; we want it shifted right by 32 to get back
 * to Q32.32. Decompose a = a_hi*2^32 + a_lo (a_hi signed, a_lo unsigned).
 * Same for b. The product is:
 *   a*b = a_hi*b_hi*2^64 + (a_hi*b_lo + a_lo*b_hi)*2^32 + a_lo*b_lo
 * Shifted right by 32:
 *   (a*b) >> 32 = a_hi*b_hi*2^32 + (a_hi*b_lo + a_lo*b_hi) + (a_lo*b_lo)>>32
 * Each partial product fits in int64 (a_hi*b_hi is signed 32*32 = 64).
 * The sum may not — we accept that for now; for phase-accumulator
 * use cases where multiplying isn't typical, this is rarely hit. */
static inline q32_32_t q32_mul(q32_32_t a, q32_32_t b) {
    int64_t  a_hi = a >> 32;
    uint64_t a_lo = (uint64_t)a & 0xFFFFFFFFu;
    int64_t  b_hi = b >> 32;
    uint64_t b_lo = (uint64_t)b & 0xFFFFFFFFu;

    int64_t hi_hi  = a_hi * b_hi;
    int64_t hi_lo  = a_hi * (int64_t)b_lo;
    int64_t lo_hi  = (int64_t)a_lo * b_hi;
    uint64_t lo_lo = a_lo * b_lo;

    return (hi_hi << 32) + hi_lo + lo_hi + (q32_32_t)(lo_lo >> 32);
}

/* Cheaper: scale a Q32.32 by a Q16.16 factor. The result fits in
 * Q32.32 as long as the scale factor's magnitude is reasonable.
 * This is the common audio sync-correction case (resample ratio
 * applied to a phase accumulator). */
static inline q32_32_t q32_scale_q16(q32_32_t a, q16_16_t scale) {
    /* a is up to 64-bit; scale is 32-bit. Product is up to 96-bit.
     * After >>16 we have 80 bits, which doesn't fit in 64. So do
     * the multiply in halves. */
    int64_t  a_hi = a >> 32;
    uint64_t a_lo = (uint64_t)a & 0xFFFFFFFFu;
    int64_t  s    = (int64_t)scale;

    int64_t  hi_part = a_hi * s;                        /* scaled high */
    int64_t  lo_part = (int64_t)((a_lo * (uint64_t)(scale < 0 ? -s : s))
                                 >> Q16_FRAC_BITS);     /* scaled low, unsigned mul */
    if (scale < 0) lo_part = -lo_part;

    return (hi_part << (32 - Q16_FRAC_BITS)) + lo_part;
}

static inline q32_32_t q32_div(q32_32_t a, q32_32_t b) {
    /* Best-effort 64-bit division. For |a| < 2^32 this is exact.
     * For larger |a|, precision degrades. Audio sync use cases
     * stay within the safe range. */
    int64_t q = (a / b) << Q32_FRAC_BITS;
    int64_t r = (a % b);
    q += (r << Q32_FRAC_BITS) / b;
    return q;
}

static inline q32_32_t q32_mac(q32_32_t a, q32_32_t b, q32_32_t c) {
    return a + q32_mul(b, c);
}

static inline q32_32_t q32_reciprocal(q32_32_t x) {
    return q32_div(Q32_ONE, x);
}

static inline q32_32_t q32_sat_add(q32_32_t a, q32_32_t b) {
    q32_32_t r = a + b;
    if (((a ^ b) >= 0) && ((a ^ r) < 0)) return (a < 0) ? Q32_MIN : Q32_MAX;
    return r;
}
static inline q32_32_t q32_sat_sub(q32_32_t a, q32_32_t b) {
    q32_32_t r = a - b;
    if (((a ^ b) < 0) && ((a ^ r) < 0)) return (a < 0) ? Q32_MIN : Q32_MAX;
    return r;
}
static inline q32_32_t q32_sat_neg(q32_32_t a) {
    return (a == Q32_MIN) ? Q32_MAX : -a;
}
static inline q32_32_t q32_sat_abs(q32_32_t a) {
    return (a == Q32_MIN) ? Q32_MAX : (a < 0 ? -a : a);
}

/* ============================================================
 *  Q48.16 — int64, 16 fractional bits
 *
 *  Same approach as Q32.32 for multiply. The "scale by Q16.16"
 *  variant is the practical hot-path operation.
 * ============================================================ */

static inline q48_16_t q48_from_int(int64_t x)   { return x << Q48_FRAC_BITS; }
static inline int64_t  q48_to_int(q48_16_t x)    { return x >> Q48_FRAC_BITS; }
static inline q48_16_t q48_from_float(float f)   { return (q48_16_t)((double)f * 65536.0); }
static inline float    q48_to_float(q48_16_t x)  { return (float)((double)x / 65536.0); }
static inline q48_16_t q48_from_double(double d) { return (q48_16_t)(d * 65536.0); }
static inline double   q48_to_double(q48_16_t x) { return (double)x / 65536.0; }

static inline q48_16_t q48_add(q48_16_t a, q48_16_t b) { return a + b; }
static inline q48_16_t q48_sub(q48_16_t a, q48_16_t b) { return a - b; }
static inline q48_16_t q48_neg(q48_16_t a)             { return -a; }
static inline q48_16_t q48_abs(q48_16_t a)             { return a < 0 ? -a : a; }

/* Q48.16 multiply: similar partial-product approach. Q48.16 × Q48.16
 * has 96+16 = 112 useful bits before shift; after >>16 we have 96 bits,
 * which doesn't fit in 64. We compute the lower 64 bits, which is
 * correct as long as the result magnitude stays within Q48.16. */
static inline q48_16_t q48_mul(q48_16_t a, q48_16_t b) {
    int64_t  a_hi = a >> 32;
    uint64_t a_lo = (uint64_t)a & 0xFFFFFFFFu;
    int64_t  b_hi = b >> 32;
    uint64_t b_lo = (uint64_t)b & 0xFFFFFFFFu;

    int64_t  hi_hi = a_hi * b_hi;
    int64_t  hi_lo = a_hi * (int64_t)b_lo;
    int64_t  lo_hi = (int64_t)a_lo * b_hi;
    uint64_t lo_lo = a_lo * b_lo;

    /* Final shift right is by 16, not 32 like Q32.32. */
    return (hi_hi << (64 - Q48_FRAC_BITS))
         + ((hi_lo + lo_hi) << (32 - Q48_FRAC_BITS))
         + (q48_16_t)(lo_lo >> Q48_FRAC_BITS);
}

static inline q48_16_t q48_div(q48_16_t a, q48_16_t b) {
    int64_t q = (a / b) << Q48_FRAC_BITS;
    int64_t r = (a % b);
    q += (r << Q48_FRAC_BITS) / b;
    return q;
}

static inline q48_16_t q48_mac(q48_16_t a, q48_16_t b, q48_16_t c) {
    return a + q48_mul(b, c);
}

static inline q48_16_t q48_reciprocal(q48_16_t x) {
    return q48_div(Q48_ONE, x);
}

static inline q48_16_t q48_sat_add(q48_16_t a, q48_16_t b) {
    q48_16_t r = a + b;
    if (((a ^ b) >= 0) && ((a ^ r) < 0)) return (a < 0) ? Q48_MIN : Q48_MAX;
    return r;
}
static inline q48_16_t q48_sat_sub(q48_16_t a, q48_16_t b) {
    q48_16_t r = a - b;
    if (((a ^ b) < 0) && ((a ^ r) < 0)) return (a < 0) ? Q48_MIN : Q48_MAX;
    return r;
}
static inline q48_16_t q48_sat_neg(q48_16_t a) {
    return (a == Q48_MIN) ? Q48_MAX : -a;
}
static inline q48_16_t q48_sat_abs(q48_16_t a) {
    return (a == Q48_MIN) ? Q48_MAX : (a < 0 ? -a : a);
}

/* ============================================================
 *  Cross-format conversions
 * ============================================================ */

static inline q31_t    q15_to_q31(q15_t x)    { return (q31_t)x << 16; }
static inline q15_t    q31_to_q15(q31_t x)    { return (q15_t)(x >> 16); }
static inline q16_16_t q15_to_q16(q15_t x)    { return (q16_16_t)x << 1; }
static inline q15_t    q16_to_q15(q16_16_t x) { return fx_sat16_((int64_t)(x >> 1)); }
static inline q32_32_t q16_to_q32(q16_16_t x) { return (q32_32_t)x << 16; }
static inline q16_16_t q32_to_q16(q32_32_t x) { return fx_sat32_(x >> 16); }
static inline q48_16_t q16_to_q48(q16_16_t x) { return (q48_16_t)x; }
static inline q16_16_t q48_to_q16(q48_16_t x) { return fx_sat32_(x); }

#endif /* FIXED_POINT_H */
