/* ============================================================
 *  hc_preview.c — host preview of the 60-colour dual-layer path.
 *
 *  Renders a shaded cube with r3d into an 8bpp framebuffer whose values
 *  pack (hue<<2 | brightness), splits each pixel via the hicolor LUT
 *  (base = BG1 4bpp, sub = BG3 2bpp), composites them with SNES half-add
 *  colour math, and writes a 24-bit BMP — so we can eyeball the 60-colour
 *  shading before any SNES plumbing. A 60-swatch strip along the bottom
 *  shows the whole palette (15 hues x 4 brightness).
 *
 *  Build (from repo root, MSYS2 mingw64 gcc):
 *    gcc -Iinclude src/video/tests/hc_preview.c src/video/r3d.c \
 *        src/math/fixed_point.c src/math/trig_q16.c src/math/fast_div.c \
 *        -lm -o build/hc_preview.exe
 *  Run: ./build/hc_preview.exe build/hc_preview.bmp
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "video/r3d.h"
#include "math/mat_q16.h"
#include "math/vec3_q16.h"
#include "math/fixed_point.h"
#include "video/hicolor.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VW 240
#define VH 208
#define SWATCH_H 24
#define IMG_H (VH + SWATCH_H)

/* unit cube (verified winding, from copro_r3d.c) */
static const int cube_vd[8][3] = {
    {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
    {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
};
static const uint16_t cube_t[36] = {
    4,5,6, 4,6,7,   0,3,2, 0,2,1,   1,6,5, 1,2,6,
    0,4,7, 0,7,3,   3,7,6, 3,6,2,   0,1,5, 0,5,4,
};
/* per-face hue<<2 (6 hues, 2 tris each). r3d emits tri_base + shade(0..3). */
#define H(hue) ((uint8_t)((hue) << 2))
static const uint8_t cube_base[12] = {
    H(1),H(1),  H(3),H(3),  H(5),H(5),  H(7),H(7),  H(9),H(9),  H(11),H(11),
};

static vec3_q16 cube_v[8];
static uint8_t  fb[VW * VH];

static void put_bmp(const char *path, const uint8_t *rgb /*IMG_H*VW*3, top-down*/) {
    int W = VW, Hh = IMG_H;
    int row = (W * 3 + 3) & ~3;               /* 4-byte aligned rows */
    int dsz = row * Hh;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    uint32_t fsz = 54 + dsz;
    hdr[2]=fsz; hdr[3]=fsz>>8; hdr[4]=fsz>>16; hdr[5]=fsz>>24;
    hdr[10]=54; hdr[14]=40;
    hdr[18]=W; hdr[19]=W>>8; hdr[22]=Hh; hdr[23]=Hh>>8;
    hdr[26]=1; hdr[28]=24;
    hdr[34]=dsz; hdr[35]=dsz>>8; hdr[36]=dsz>>16; hdr[37]=dsz>>24;
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return; }
    fwrite(hdr, 1, 54, f);
    uint8_t *line = (uint8_t *)calloc(1, row);
    for (int y = Hh - 1; y >= 0; y--) {       /* BMP is bottom-up */
        for (int x = 0; x < W; x++) {
            const uint8_t *p = rgb + (y * W + x) * 3;
            line[x*3+0] = p[2]; line[x*3+1] = p[1]; line[x*3+2] = p[0]; /* BGR */
        }
        fwrite(line, 1, row, f);
    }
    free(line);
    fclose(f);
}

int main(int argc, char **argv) {
    const char *out = (argc > 1) ? argv[1] : "build/hc_preview.bmp";

    for (int i = 0; i < 8; i++)
        cube_v[i] = vec3_q16_make(q16_from_int(cube_vd[i][0]),
                                  q16_from_int(cube_vd[i][1]),
                                  q16_from_int(cube_vd[i][2]));
    R3dMesh mesh = { cube_v, 8, cube_t, 12, cube_base };

    /* camera at origin, looking +Z */
    R3dObject obj;
    obj.mesh = &mesh;
    mat3_q16 rot = mat3_q16_mul(mat3_q16_rotation_y(q16_from_double(0.7)),
                                mat3_q16_rotation_x(q16_from_double(0.5)));
    obj.xform = affine3_q16_compose(
        affine3_q16_from_translation(vec3_q16_make(0, 0, q16_from_int(6))),
        affine3_q16_from_rotation(rot));

    R3dScene sc;
    sc.view    = affine3_q16_view(vec3_q16_make(0,0,0),
                                  vec3_q16_make(Q16_ONE,0,0),
                                  vec3_q16_make(0,Q16_ONE,0),
                                  vec3_q16_make(0,0,Q16_ONE));
    sc.focal   = q16_from_int(160);
    sc.near_z  = q16_from_double(0.5);
    sc.light   = vec3_q16_normalize(vec3_q16_make(q16_from_double(-0.4),
                                                  q16_from_double(0.55),
                                                  q16_from_double(-0.73)));
    sc.ambient = q16_from_double(0.30);
    sc.diffuse = q16_from_double(0.70);
    sc.base    = 0;
    sc.ramp    = HC_RAMP;
    sc.objs    = &obj;
    sc.nobjs   = 1;

    r3d_render_dither(&sc, fb, VW, VH);

    /* ---- dual-layer encode round-trip (the copro_r3d.c logic) ----
     * base = v>>2 -> BG1 4bpp planar CHR (32 B/tile), sub = v&3 -> BG3 2bpp
     * planar CHR (16 B/tile). Decode both back and confirm lossless. */
    enum { TW = VW/8, TH = VH/8, NT = TW*TH };   /* 30 x 26 = 780 tiles */
    static uint8_t base_chr[NT][32], sub_chr[NT][16];
    memset(base_chr, 0, sizeof base_chr);
    memset(sub_chr,  0, sizeof sub_chr);
    for (int t = 0; t < NT; t++) {
        int tx = (t % TW) * 8, ty = (t / TW) * 8;
        for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) {
            uint8_t v = fb[(ty+yy)*VW + (tx+xx)];
            int b = hc_base_of(v), s = hc_sub_of(v), bit = 7 - xx;
            base_chr[t][yy*2+0]      |= (uint8_t)(((b>>0)&1) << bit);
            base_chr[t][yy*2+1]      |= (uint8_t)(((b>>1)&1) << bit);
            base_chr[t][16+yy*2+0]   |= (uint8_t)(((b>>2)&1) << bit);
            base_chr[t][16+yy*2+1]   |= (uint8_t)(((b>>3)&1) << bit);
            sub_chr[t][yy*2+0]       |= (uint8_t)(((s>>0)&1) << bit);
            sub_chr[t][yy*2+1]       |= (uint8_t)(((s>>1)&1) << bit);
        }
    }
    int bad = 0;
    for (int t = 0; t < NT; t++) {
        int tx = (t % TW) * 8, ty = (t / TW) * 8;
        for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) {
            int bit = 7 - xx;
            int b = ((base_chr[t][yy*2+0]>>bit)&1) | (((base_chr[t][yy*2+1]>>bit)&1)<<1)
                  | (((base_chr[t][16+yy*2+0]>>bit)&1)<<2) | (((base_chr[t][16+yy*2+1]>>bit)&1)<<3);
            int s = ((sub_chr[t][yy*2+0]>>bit)&1) | (((sub_chr[t][yy*2+1]>>bit)&1)<<1);
            uint8_t recon = (uint8_t)((b<<2) | s);
            if (recon != fb[(ty+yy)*VW + (tx+xx)]) bad++;
        }
    }
    printf("dual-layer encode round-trip: %s (%d mismatches)\n",
           bad ? "FAIL" : "PASS", bad);

    /* composite -> RGB image (top region) + swatch strip (bottom) */
    static uint8_t rgb[IMG_H * VW * 3];
    for (int y = 0; y < VH; y++)
        for (int x = 0; x < VW; x++) {
            uint8_t r,g,b; hc_rgb888(fb[y*VW+x], &r,&g,&b);
            uint8_t *p = rgb + (y*VW+x)*3; p[0]=r; p[1]=g; p[2]=b;
        }
    /* 60 swatches: 15 hues x 4 subs, 4px wide each = 240px */
    for (int y = VH; y < IMG_H; y++)
        for (int x = 0; x < VW; x++) {
            int idx = x / 4;                  /* 0..59 */
            int hue = idx / 4 + 1;            /* 1..15 */
            int sub = idx % 4;                /* 0..3  */
            uint8_t v = (uint8_t)((hue << 2) | sub);
            uint8_t r,g,b; hc_rgb888(v, &r,&g,&b);
            uint8_t *p = rgb + (y*VW+x)*3; p[0]=r; p[1]=g; p[2]=b;
        }

    put_bmp(out, rgb);
    printf("wrote %s (%dx%d, cube + 60-swatch strip)\n", out, VW, IMG_H);
    return 0;
}
