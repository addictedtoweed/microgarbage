/* ============================================================
 *  trig_q16.c — CORDIC sin/cos/tan/atan2 in Q16.16.
 *  See include/math/trig_q16.h. Integer-only, no libm.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "math/trig_q16.h"

#define NCORDIC 16

/* atan(2^-i) in Q16.16 radians, i = 0..15. (For i >= 6 these equal
 * 2^-i to the bit, since atan(t) ~= t for tiny t.) */
static const q16_16_t ATAN[NCORDIC] = {
    51472, 30386, 16055, 8150, 4091, 2048, 1024, 512,
    256,   128,   64,    32,   16,   8,    4,    2
};

/* 1/K, K = prod sqrt(1 + 2^-2i): pre-loaded into x so the CORDIC gain
 * cancels and the result comes out as true cos/sin. 0.60725294 * 65536. */
#define CORDIC_INV_GAIN ((q16_16_t)39797)

void q16_sincos(q16_16_t radians, q16_16_t *sin_out, q16_16_t *cos_out) {
    q16_16_t z = radians;
    while (z >  Q16_PI) z -= Q16_TWO_PI;          /* fold to [-pi, pi] */
    while (z < -Q16_PI) z += Q16_TWO_PI;

    int neg = 0;                                  /* fold to [-pi/2, pi/2] */
    if (z > Q16_HALF_PI)       { z -= Q16_PI; neg = 1; }
    else if (z < -Q16_HALF_PI) { z += Q16_PI; neg = 1; }

    q16_16_t x = CORDIC_INV_GAIN, y = 0;
    for (int i = 0; i < NCORDIC; i++) {
        q16_16_t dx = x >> i, dy = y >> i;
        if (z >= 0) { x -= dy; y += dx; z -= ATAN[i]; }
        else        { x += dy; y -= dx; z += ATAN[i]; }
    }
    if (neg) { x = -x; y = -y; }                  /* reflected through origin */
    if (sin_out) *sin_out = y;
    if (cos_out) *cos_out = x;
}

q16_16_t q16_sin(q16_16_t radians) { q16_16_t s; q16_sincos(radians, &s, 0); return s; }
q16_16_t q16_cos(q16_16_t radians) { q16_16_t c; q16_sincos(radians, 0, &c); return c; }

q16_16_t q16_tan(q16_16_t radians) {
    q16_16_t s, c;
    q16_sincos(radians, &s, &c);
    if (c == 0) return (s >= 0) ? Q16_MAX : Q16_MIN;
    return q16_div(s, c);
}

q16_16_t q16_atan2(q16_16_t y, q16_16_t x) {
    if (x == 0 && y == 0) return 0;
    q16_16_t xx = x, yy = y, z = 0;
    int negx = 0;
    if (xx < 0) { xx = -xx; yy = -yy; negx = 1; }  /* fold to right half-plane */
    for (int i = 0; i < NCORDIC; i++) {
        q16_16_t dx = xx >> i, dy = yy >> i;
        if (yy > 0) { xx += dy; yy -= dx; z += ATAN[i]; }  /* drive y -> 0 */
        else        { xx -= dy; yy += dx; z -= ATAN[i]; }
    }
    if (negx) z += (y >= 0) ? Q16_PI : -Q16_PI;
    return z;
}
