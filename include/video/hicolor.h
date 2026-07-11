/* ============================================================
 *  hicolor.h — 60-colour dual-layer palette + LUT split.
 *
 *  The coprocessor renders into an 8bpp framebuffer whose value packs
 *  (hue << 2 | brightness): the high bits pick one of 16 base hues
 *  (BG1 4bpp, MAIN screen), the low 2 bits pick one of 4 brightness
 *  levels (BG3 2bpp, SUB screen). The SNES composites them with
 *  half-add colour math -> up to 16x4 = 64 (~60 distinct) blended
 *  colours, the scheme proven on hardware by snes/cmtest.s + cubes_demo.s.
 *
 *  This is the "no swizzle, just a LUT" split the design calls for:
 *      base(BG1) = value >> 2          (hc_base_of)
 *      sub (BG3) = value & 3           (hc_sub_of)
 *  and r3d needs NO changes — set each face's tri_base = hue<<2 and
 *  scene.ramp = HC_RAMP, so r3d emits (hue<<2 | brightness) directly.
 *
 *  Palettes are first-cut and TUNABLE (saturated hues + a neutral
 *  brightness ramp; highlights desaturate as they brighten). Adjust
 *  hc_base[]/hc_sub[] freely — nothing else depends on the values.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HICOLOR_H
#define HICOLOR_H

#include <stdint.h>

/* Brightness levels per hue (BG3 sub 0..3). r3d scene.ramp = this. */
#define HC_RAMP 4

/* CGRAM 0-15: 16 base hues (BG1 4bpp main). BGR555 (R=bit0-4, G=5-9, B=10-14).
 * 0 = backdrop; 1..12 a spectrum; 13-15 white/grey/brown. */
static const uint16_t hc_base[16] = {
    0x0000, /* 0  backdrop / sky   */
    0x001F, /* 1  red              */
    0x019F, /* 2  orange           */
    0x03FF, /* 3  yellow           */
    0x03F0, /* 4  lime             */
    0x03E0, /* 5  green            */
    0x43E0, /* 6  spring           */
    0x7FE0, /* 7  cyan             */
    0x7E00, /* 8  azure            */
    0x7C00, /* 9  blue             */
    0x7C10, /* 10 violet           */
    0x7C1F, /* 11 magenta          */
    0x401F, /* 12 rose             */
    0x7FFF, /* 13 white            */
    0x4210, /* 14 grey             */
    0x1110, /* 15 brown            */
};

/* CGRAM 16-19: 4 sub brightness levels (BG3 2bpp sub screen). Neutral greys
 * added by half-add: 0 darkest (halves the hue), 3 brightest (washes toward
 * grey). */
static const uint16_t hc_sub[4] = {
    0x0000, /* 0 dark   */
    0x2529, /* 1 dim    */
    0x4A52, /* 2 mid    */
    0x6739, /* 3 bright */
};

/* SNES half-add colour math: per 5-bit channel, out = (a + b) >> 1. */
static inline uint16_t hc_halfadd(uint16_t a, uint16_t b) {
    int r  = (( a        & 0x1F) + ( b        & 0x1F)) >> 1;
    int g  = (((a >> 5)  & 0x1F) + ((b >> 5)  & 0x1F)) >> 1;
    int bl = (((a >> 10) & 0x1F) + ((b >> 10) & 0x1F)) >> 1;
    return (uint16_t)(r | (g << 5) | (bl << 10));
}

/* Framebuffer value -> layer split (for the SNES encode). */
static inline uint8_t hc_base_of(uint8_t v) { return (uint8_t)((v >> 2) & 0x0F); }
static inline uint8_t hc_sub_of (uint8_t v) { return (uint8_t)(v & 0x03); }

/* Framebuffer value -> composited RGB888 (for host preview / verification). */
static inline void hc_rgb888(uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b) {
    uint16_t c = hc_halfadd(hc_base[hc_base_of(v)], hc_sub[hc_sub_of(v)]);
    int R = c & 0x1F, G = (c >> 5) & 0x1F, B = (c >> 10) & 0x1F;
    *r = (uint8_t)((R << 3) | (R >> 2));
    *g = (uint8_t)((G << 3) | (G >> 2));
    *b = (uint8_t)((B << 3) | (B >> 2));
}

#endif /* HICOLOR_H */
