/* demo_cube.c — M1 of the coprocessor 3D port: a slowly rotating + bouncing
 * cube rendered by the r3d polygon engine, dithered to a 4bpp tiled Mode-1
 * frame (240x208) — the same swizzle + transport demo_canyon4/demo_fmv use.
 *
 * This is the renderer+swizzle HALF of the coprocessor port: the cube scene
 * setup and the load_4bpp() swizzle here are reused verbatim when the OpenGL
 * preview is swapped for the real cart-window transport (so the cube renders in
 * bsnes / on the SNES). Host-side preview first to validate geometry, the
 * two-axis tumble, the bounce, the dither and the 4bpp tile encode in isolation.
 *
 * Geometry: 8-vertex cube, 12 triangles (6 faces x 2), wound CCW as seen from
 * OUTSIDE so r3d's backface cull (front-facing <=> n.ctr < 0) keeps the visible
 * faces. Each axis-pair of faces gets its own hue ramp (Z=red, X=green, Y=blue),
 * 5 ordered-dithered shades each -> a flat-shaded, tumbling, high-ish-colour cube.
 *
 * Build + run (Windows):
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_cube \
 *      src/video/ppu.c src/video/present_gl_win32.c src/video/r3d.c \
 *      src/math/trig_q16.c src/math/fixed_point.c \
 *      src/video/tests/demo_cube.c -lopengl32 -lgdi32 -luser32
 * Headless ASCII (frame 0, no GL): add -DCUBE_HEADLESS, link without present/GL.
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#include "video/r3d.h"
#ifndef CUBE_HEADLESS
#include "video/present.h"
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- 4bpp tiled target (matches the FMV / canyon4 engine) ---- */
#define VW 240
#define VH 208
#define TW (VW/8)          /* 30 */
#define TH (VH/8)          /* 26 */
#define NTILES (TW*TH)     /* 780 */
#define TMAP_W 0x0000      /* nametable base (word)        */
#define CHR_W  0x2000      /* CHR base (word, 8KB-aligned) */
#define BLANK_TILE NTILES  /* tile 780 = zeroed = backdrop, for the margins */

/* three 5-shade hue ramps (CGRAM 1..15), index 0 = backdrop. Per-face palette
 * base picks the hue; the dither spreads brightness across the ramp. */
#define RAMP4    5
#define RED_BASE   1       /* CGRAM 1..5   : +Z / -Z faces */
#define GREEN_BASE 6       /* CGRAM 6..10  : +X / -X faces */
#define BLUE_BASE  11      /* CGRAM 11..15 : +Y / -Y faces */

#define CAM_Z   q16_from_double(6.0)   /* cube sits 6 units in front of the eye */

static PpuState P;
static uint8_t  fbuf[VW * VH];         /* 8bpp render target (dithered ramp indices) */
#ifndef CUBE_HEADLESS
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
#endif

/* unit cube, corners at (+-1, +-1, +-1) */
static vec3_q16 cube_v[8];
static const double cube_vd[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},   /* 0..3 : z = -1 */
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},   /* 4..7 : z = +1 */
};
/* 12 triangles, CCW from outside (each face's outward normal verified by hand) */
static const uint16_t cube_t[36] = {
    4,5,6,  4,6,7,    /* +Z */
    0,3,2,  0,2,1,    /* -Z */
    1,6,5,  1,2,6,    /* +X */
    0,4,7,  0,7,3,    /* -X */
    3,7,6,  3,6,2,    /* +Y */
    0,1,5,  0,5,4,    /* -Y */
};
/* per-triangle palette base: Z->red, X->green, Y->blue (opposite faces share). */
static const uint8_t cube_base[12] = {
    RED_BASE,   RED_BASE,     /* +Z */
    RED_BASE,   RED_BASE,     /* -Z */
    GREEN_BASE, GREEN_BASE,   /* +X */
    GREEN_BASE, GREEN_BASE,   /* -X */
    BLUE_BASE,  BLUE_BASE,    /* +Y */
    BLUE_BASE,  BLUE_BASE,    /* -Y */
};

static R3dMesh   cube_mesh;
static R3dObject g_obj;
static R3dScene  scene;

#define BGR555(r,g,b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

static float ffloor_(float x) { int i = (int)x; return (x < 0.0f && (float)i != x) ? (float)(i - 1) : (float)i; }
static float fwrap_(float x)  { const float T = 6.2831853f; return x - T * ffloor_(x * (1.0f / T) + 0.5f); }

static uint16_t lerp555(uint16_t a, uint16_t b, int num, int den) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    return BGR555(ar + (br - ar) * num / den, ag + (bg - ag) * num / den, ab + (bb - ab) * num / den);
}

/* three hue ramps so CGRAM index == fb shade index (single shared palette 0). */
static void build_palette(void) {
    ppu_state_clear(&P);
    P.mode = 1;
    P.brightness = 15;
    P.cgram[0] = BGR555(2, 2, 4);                          /* dark backdrop */
    for (int s = 0; s < RAMP4; s++) {
        P.cgram[RED_BASE   + s] = lerp555(BGR555(7, 2, 2),  BGR555(31, 12, 10), s, RAMP4 - 1);
        P.cgram[GREEN_BASE + s] = lerp555(BGR555(2, 7, 2),  BGR555(12, 31, 12), s, RAMP4 - 1);
        P.cgram[BLUE_BASE  + s] = lerp555(BGR555(2, 3, 8),  BGR555(12, 16, 31), s, RAMP4 - 1);
    }
}

static void build_scene(void) {
    build_palette();
    for (int i = 0; i < 8; i++)
        cube_v[i] = vec3_q16_make(q16_from_double(cube_vd[i][0]),
                                  q16_from_double(cube_vd[i][1]),
                                  q16_from_double(cube_vd[i][2]));
    cube_mesh.verts = cube_v; cube_mesh.nverts = 8;
    cube_mesh.tris  = cube_t; cube_mesh.ntris  = 12;
    cube_mesh.tri_base = cube_base;

    g_obj.mesh = &cube_mesh; g_obj.xform = affine3_q16_identity();

    /* camera at the origin looking down +z, y up, x right -> view == world. */
    scene.view = affine3_q16_view(vec3_q16_make(0, 0, 0),
                                  vec3_q16_make(Q16_ONE, 0, 0),
                                  vec3_q16_make(0, Q16_ONE, 0),
                                  vec3_q16_make(0, 0, Q16_ONE));
    scene.focal   = q16_from_int(160);
    scene.near_z  = q16_from_double(0.5);
    /* light in VIEW space: upper-left, angled toward the camera (-z) so the
     * face currently facing us is lit. */
    scene.light   = vec3_q16_normalize(vec3_q16_make(q16_from_double(-0.4),
                                                     q16_from_double(0.55),
                                                     q16_from_double(-0.73)));
    scene.ambient = q16_from_double(0.30);
    scene.diffuse = q16_from_double(0.70);
    scene.base    = RED_BASE;
    scene.ramp    = RAMP4;
    scene.objs    = &g_obj; scene.nobjs = 1;
}

/* set the cube's model->world: rotate (Y by ay, X by ax), then translate to
 * (bx, by, CAM_Z) so it tumbles in place and drifts around the playfield. */
static void update_cube(float ay, float ax, float bx, float by) {
    affine3_q16 rot = affine3_q16_from_rotation(
        mat3_q16_mul(mat3_q16_rotation_y(q16_from_double(fwrap_(ay))),
                     mat3_q16_rotation_x(q16_from_double(fwrap_(ax)))));
    g_obj.xform = affine3_q16_compose(
        affine3_q16_from_translation(vec3_q16_make(q16_from_double(bx),
                                                   q16_from_double(by), CAM_Z)),
        rot);
}

/* swizzle the 8bpp dithered frame -> SNES 4bpp tiles (CHR + tilemap), centered
 * with a one-tile backdrop margin. Single shared palette 0: fb index IS the
 * 4bpp colour index. (Identical to demo_canyon4's load_4bpp.) */
static void load_4bpp(void) {
    static uint8_t chr[NTILES][32];
    for (int i = 0; i < 1024; i++) P.vram[TMAP_W + i] = BLANK_TILE;
    for (int t = 0; t < NTILES; t++) {
        int tx = (t % TW) * 8, ty = (t / TW) * 8;
        memset(chr[t], 0, 32);
        for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) {
            int bi = fbuf[(ty + yy) * VW + (tx + xx)] & 0x0F;
            int row = yy, bit = 7 - xx;
            chr[t][row*2+0]    |= ((bi>>0)&1)<<bit;
            chr[t][row*2+1]    |= ((bi>>1)&1)<<bit;
            chr[t][16+row*2+0] |= ((bi>>2)&1)<<bit;
            chr[t][16+row*2+1] |= ((bi>>3)&1)<<bit;
        }
        int r = t / TW, c = t % TW;
        P.vram[TMAP_W + (r + 1) * 32 + (c + 1)] = (uint16_t)t;
    }
    memcpy(&P.vram[CHR_W], chr, NTILES * 32);
}

static void setup_bg(void) {
    P.bg[0].tilemap_word = TMAP_W;
    P.bg[0].char_word    = CHR_W;
    P.bg[0].size         = PPU_SC_32x32;
    P.bg[0].hofs = 0; P.bg[0].vofs = 0;
    P.bg[0].on_main = true;
}

#ifdef CUBE_HEADLESS
int main(void) {
    build_scene();
    setup_bg();
    update_cube(0.6f, 0.4f, 0.0f, 0.0f);                   /* three faces visible */
    long px = r3d_render_dither(&scene, fbuf, VW, VH);
    load_4bpp();
    static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
    ppu_render(&P, FB);
    printf("cube %dx%d (%d tiles)  %ld px-tests\n", VW, VH, NTILES, px);
    for (int y = 0; y < PPU_SCREEN_H; y += 7) {
        for (int x = 0; x < PPU_SCREEN_W; x += 4) {
            uint32_t c = FB[y * PPU_SCREEN_W + x];
            unsigned r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, lum = (r + g + b) / 3;
            putchar(lum < 40 ? ' ' : lum < 100 ? '.' : lum < 170 ? ':' : '#');
        }
        putchar('\n');
    }
    return 0;
}
#else
static double now_sec(void) {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
int main(void) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - r3d cube (4bpp dithered)")) return 1;
    printf("GL: %s\nr3d cube (4bpp dithered -> Mode 1, %dx%d)\n",
           present_gl_renderer(), VW, VH);
    fflush(stdout);

    build_scene();
    setup_bg();

    double t0 = now_sec(), report = t0;
    unsigned presents = 0; double hostfps = 0.0;
    float ay = 0.0f, ax = 0.0f;                            /* tumble angles */
    float bx = 0.0f, by = 0.0f, vx = 1.7f, vy = 1.1f;      /* bounce pos / vel (units/sec) */
    double next_step = t0;
    const int fps = 20;                                    /* slow, like the eventual cart cadence */
    long px = 0;
    while (!present_should_close()) {
        double now = now_sec();
        double step = 1.0 / fps;
        if (now >= next_step) {
            if (now - next_step > 0.25) next_step = now;
            float dt = (float)step;
            ay += 0.9f * dt;                               /* slow two-axis tumble */
            ax += 0.6f * dt;
            bx += vx * dt; by += vy * dt;                  /* bounce within the playfield */
            if (bx >  2.5f) { bx =  2.5f; vx = -vx; } else if (bx < -2.5f) { bx = -2.5f; vx = -vx; }
            if (by >  1.5f) { by =  1.5f; vy = -vy; } else if (by < -1.5f) { by = -1.5f; vy = -vy; }
            update_cube(ay, ax, bx, by);
            px = r3d_render_dither(&scene, fbuf, VW, VH);
            load_4bpp();
            ppu_render(&P, FB);
            next_step += step;
        }
        char ov[400];
        snprintf(ov, sizeof ov,
            "SNES PPU emulated (Mode 1, 4bpp dithered) - r3d cube on host coprocessor\n"
            "video res : %d x %d  (4bpp tiles, 3 hue ramps x 5 shades, ordered dither)\n"
            "geometry  : 8 verts | 12 tris | %ld px-tests\n"
            "host fps  : %.1f (GPU-bound; sim steps at %d fps)",
            VW, VH, px, hostfps, fps);
        present_set_overlay(ov);
        present_frame(FB);
        presents++;
        if (now - report >= 1.0) { hostfps = (double)presents / (now - report); report = now; presents = 0; }
    }
    present_shutdown();
    return 0;
}
#endif
