/* ============================================================
 *  hfcast.h — fixed-point heightfield raycaster (the canyon renderer).
 *
 *  Portable Q16.16 per-pixel raymarcher over FLOOR + CEILING + MATERIAL
 *  heightfields: the same renderer the host demo previews and the cart
 *  runs. No stdio, no libm, no float — built on math/vec3_q16 + mat_q16.
 *  Produces an 8bpp palette-index framebuffer; the caller stretches it
 *  to 256x224 via Mode 7 (on the cart, the SNES PPU does that for free).
 *
 *  This is the straight Q16.16 port of the float prototype's march
 *  (adaptive step + binary-search hit refine + gradient-normal light).
 *  Grid-DDA traversal and a baked light map are the next optimisation
 *  passes; the API stays the same.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VIDEO_HFCAST_H
#define VIDEO_HFCAST_H

#include <stdint.h>
#include "math/vec3_q16.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The world: three mapsz*mapsz uint8 maps + how to shade/colour it.
 * mapsz must be a power of two (coords wrap via mask). */
typedef struct {
    const uint8_t *floor;       /* floor height per cell (0..255)        */
    const uint8_t *ceiling;     /* ceiling height; >= 254 means open sky */
    const uint8_t *material;    /* material id per floor cell            */
    int       mapsz;

    vec3_q16  light;            /* unit light direction (diffuse)        */
    q16_16_t  fog_range;        /* depth at which fog reaches its floor  */

    /* output palette layout: index = base[material] + shade(0..ramp-1). */
    int rock_base, snow_base, lava_base, ramp;
    int rock_mat, snow_mat, lava_mat;
    q16_16_t sky_h;             /* a ray climbing past this height is guaranteed sky:
                                 * early-out (must exceed the tallest terrain) */
} HfScene;

/* The camera: position + orthonormal-ish basis + half-FOV tangents. */
typedef struct {
    vec3_q16 pos;
    vec3_q16 right, up, fwd;    /* fwd ~unit; right/up span the screen   */
    q16_16_t halfw, halfh;      /* tan(fov/2) * aspect , tan(fov/2)      */
} HfCamera;

/* Render the scene into fb (fbw*fbh, 8bpp). Returns the total number of
 * march steps taken — the on-cart cycle-cost proxy. */
long hfcast_render(const HfScene *s, const HfCamera *c,
                   uint8_t *fb, int fbw, int fbh);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_HFCAST_H */
