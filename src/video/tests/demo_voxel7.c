/* demo_voxel7.c — MANUAL visual PoC: Mode 7 "stretched framebuffer".
 *
 * The alternative to the tile-vocabulary approach (demo_voxel.c): the
 * coprocessor SOFTWARE-renders the whole scene into a small 8bpp
 * framebuffer (here FBW x FBH ~= 80x72, ~5.6 KB = one NTSC vblank's DMA
 * budget), DMAs it into the Mode 7 char region with a linear tilemap,
 * and the Mode 7 matrix STRETCHES it to fill 256x224. Arbitrary
 * per-pixel imagery (no tile vocabulary), depth-shaded 256-color ramps,
 * single-buffered (the buffer fits in vblank, so no tearing despite
 * Mode 7 being unable to page-flip).
 *
 * This is the better fit for high-speed flying: more effective
 * resolution than the 32x28 tile grid, smooth depth shading, and the
 * Mode 7 matrix gives free camera roll later (scale-only here; roll is
 * a matrix-rotation tweak).
 *
 * Build + run on Windows:
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_voxel7 \
 *      src/video/ppu.c src/video/present_gl_win32.c \
 *      src/video/tests/demo_voxel7.c -lopengl32 -lgdi32 -luser32
 *   ./build/demo_voxel7
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#include "video/present.h"

#include <windows.h>   /* QueryPerformanceCounter */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
/* no <math.h> (it resolves to the repo's math aggregator under -Iinclude) */

#define SEED          24601u
#define MAPSZ         256
#define MAPMASK       (MAPSZ - 1)
#define SNES_NTSC_HZ  60.0988

#define FBW  80        /* framebuffer width  (10 tiles) */
#define FBH  72        /* framebuffer height ( 9 tiles) -> ~5.6 KB / frame */

/* 256-color palette layout: 0 = sky (transparent -> violet backdrop),
 * then three material ramps of RAMP steps (far/hazed .. near/bright). */
#define RAMP 20
#define ROCK_BASE 1
#define SNOW_BASE (ROCK_BASE + RAMP)
#define LAVA_BASE (SNOW_BASE + RAMP)

static PpuState P;
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
static uint8_t  Hmap[MAPSZ * MAPSZ];
static uint8_t  Mmap[MAPSZ * MAPSZ];
static uint8_t  fbuf[FBW * FBH];        /* the 8bpp software framebuffer */

#define BGR555(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))
#define MAT_ROCK 0
#define MAT_SNOW 1
#define MAT_LAVA 2

static double now_sec(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
static float ffloor(float x) { int i = (int)x; return (x < 0.0f && (float)i != x) ? (float)(i - 1) : (float)i; }
static float fsin(float x) {
    const float PI = 3.14159265f, TWO_PI = 6.28318531f;
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    float ax = (x < 0.0f) ? -x : x;
    return 1.27323954f * x - 0.405284735f * x * ax;
}
static float fcos(float x) { return fsin(x + 1.57079633f); }

/* ---- procedural terrain (same generator as demo_voxel) ----- */

static uint32_t hash2(int x, int y, uint32_t seed) {
    uint32_t h = seed + (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float lattice(int x, int y, uint32_t seed) {
    return (float)(hash2(x & MAPMASK, y & MAPMASK, seed) & 0xFFFFu) / 65535.0f;
}
static float vnoise(float x, float y, uint32_t seed) {
    int xi = (int)ffloor(x), yi = (int)ffloor(y);
    float fx = x - (float)xi, fy = y - (float)yi;
    float a = lattice(xi, yi, seed),     b = lattice(xi + 1, yi, seed);
    float c = lattice(xi, yi + 1, seed), d = lattice(xi + 1, yi + 1, seed);
    float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}
static float fbm(float x, float y, uint32_t seed) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f;
    for (int o = 0; o < 5; o++) {
        sum += amp * vnoise(x * freq, y * freq, seed + (uint32_t)o * 101u);
        freq *= 2.0f; amp *= 0.5f;
    }
    return sum;
}
static void gen_terrain(uint32_t seed) {
    for (int y = 0; y < MAPSZ; y++)
        for (int x = 0; x < MAPSZ; x++) {
            float n = fbm((float)x / 48.0f, (float)y / 48.0f, seed);
            n = n * n * (3.0f - 2.0f * n);
            int h = (int)(n * 255.0f);
            if (h < 0) h = 0;
            if (h > 255) h = 255;
            uint8_t mat = MAT_ROCK;
            if (h > 160) mat = MAT_SNOW;
            float lava = fbm((float)x / 90.0f + 17.0f, (float)y / 90.0f + 41.0f, seed ^ 0x5A5Au);
            if (h < 100 && lava > 0.55f) mat = MAT_LAVA;
            Hmap[y * MAPSZ + x] = (uint8_t)h;
            Mmap[y * MAPSZ + x] = mat;
        }
}

/* ---- palette: depth-shaded material ramps ------------------ */

static uint16_t lerp555(uint16_t a, uint16_t b, float t) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    return BGR555(ar + (int)((br - ar) * t), ag + (int)((bg - ag) * t), ab + (int)((bb - ab) * t));
}
static void build_palette(void) {
    const uint16_t sky = BGR555(17, 6, 26);          /* violet Aardvark */
    P.cgram[0] = sky;                                 /* index 0 = sky/backdrop */
    /* nr/fr (not near/far — those are windows.h macros): near + far color. */
    struct { uint16_t nr, fr; } m[3] = {
        { BGR555(22, 20, 17), lerp555(BGR555(6, 5, 6),   sky, 0.5f) },  /* rock */
        { BGR555(31, 31, 31), lerp555(BGR555(13, 16, 22), sky, 0.5f) }, /* snow */
        { BGR555(31, 27, 7),  lerp555(BGR555(16, 3, 2),  sky, 0.35f) }, /* lava */
    };
    int base[3] = { ROCK_BASE, SNOW_BASE, LAVA_BASE };
    for (int mat = 0; mat < 3; mat++)
        for (int s = 0; s < RAMP; s++) {
            float t = (float)s / (float)(RAMP - 1);   /* 0 = far/hazed, 1 = near/bright */
            P.cgram[base[mat] + s] = lerp555(m[mat].fr, m[mat].nr, t);
        }
}
static uint8_t shade_index(int mat, float bright) {
    int base = (mat == MAT_SNOW) ? SNOW_BASE : (mat == MAT_LAVA) ? LAVA_BASE : ROCK_BASE;
    int s = (int)(bright * (float)(RAMP - 1) + 0.5f);
    if (s < 0) s = 0;
    if (s > RAMP - 1) s = RAMP - 1;
    return (uint8_t)(base + s);
}

/* ---- software render into the 8bpp framebuffer ------------- */

static void render_fb(float cx, float cz, float cy, float yaw) {
    /* HORIZON higher on screen => more ground visible below (looking down
     * from the high soar). */
    const float FOV = 1.15f, HORIZON = (float)FBH * 0.55f, HSCALE = (float)FBH * 0.62f;
    const float ZNEAR = 4.0f, ZFAR = 200.0f;

    for (int col = 0; col < FBW; col++) {
        float ang = yaw + ((float)col / (float)(FBW - 1) - 0.5f) * FOV;
        float dx = fsin(ang), dz = fcos(ang);
        for (int r = 0; r < FBH; r++) fbuf[r * FBW + col] = 0;  /* sky */

        int ybuf = FBH;
        for (float z = ZNEAR; z < ZFAR; z += 1.0f) {
            float wx = cx + dx * z, wz = cz + dz * z;
            int hx = (int)ffloor(wx) & MAPMASK, hy = (int)ffloor(wz) & MAPMASK;
            float h = (float)Hmap[hy * MAPSZ + hx];
            int sy = (int)(HORIZON - (h - cy) * HSCALE / z);
            if (sy < 0) sy = 0;
            if (sy < ybuf) {
                int mat = Mmap[hy * MAPSZ + hx];
                float bright = 1.0f - z / ZFAR;       /* near bright, far hazed */
                uint8_t idx = shade_index(mat, bright);
                for (int r = sy; r < ybuf; r++) fbuf[r * FBW + col] = idx;
                ybuf = sy;
            }
        }
    }
}

/* ---- load framebuffer into Mode 7 VRAM + set the scale ----- */

static void load_mode7(void) {
    const int TW = FBW / 8;                            /* tiles across */
    for (int ty = 0; ty < FBH / 8; ty++)
        for (int tx = 0; tx < TW; tx++) {
            unsigned tile = (unsigned)(ty * TW + tx);
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++) {
                    uint8_t idx = fbuf[(ty * 8 + py) * FBW + (tx * 8 + px)];
                    unsigned w = (tile * 64u + (unsigned)py * 8u + (unsigned)px) & 0x7FFFu;
                    P.vram[w] = (uint16_t)((P.vram[w] & 0x00FFu) | ((unsigned)idx << 8)); /* char = high byte */
                }
            unsigned mw = ((unsigned)ty * 128u + (unsigned)tx) & 0x7FFFu;
            P.vram[mw] = (uint16_t)((P.vram[mw] & 0xFF00u) | (tile & 0xFFu));             /* tilemap = low byte */
        }
    /* Matrix maps screen -> framebuffer: tex_x = FBW/256 * x, etc. */
    P.m7a = (int16_t)FBW;
    P.m7d = (int16_t)(256 * FBH / PPU_SCREEN_H);
    P.m7b = 0; P.m7c = 0; P.m7x = 0; P.m7y = 0; P.m7hofs = 0; P.m7vofs = 0;
}

int main(void) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - Mode 7 stretch PoC")) {
        return 1;
    }
    printf("GL renderer : %s\n", present_gl_renderer());
    printf("Mode 7 stretch: software %dx%d framebuffer (~%.1f KB/frame) -> stretched to 256x224.\n",
           FBW, FBH, (double)(FBW * FBH) / 1024.0);
    fflush(stdout);

    ppu_state_clear(&P);
    P.mode = 7;
    build_palette();
    gen_terrain(SEED);

    float tx = 128.0f, tz = 0.0f, ty = 0.0f, tyaw = 0.0f;
    float ax = 128.0f, az = 0.0f, ay = 200.0f, ayaw = 0.0f;
    const float SPEED = 1.30f, LAG = 0.06f;   /* fast soar to read velocity */

    const double target_dt = 1.0 / SNES_NTSC_HZ;
    double last = now_sec(), acc = 0.0;
    unsigned f = 0;

    while (!present_should_close()) {
        double t = now_sec();
        acc += t - last; last = t;
        if (acc > 0.25) acc = 0.25;
        bool stepped = false;
        while (acc >= target_dt) {
            tyaw = 0.3f * fsin((float)f * 0.012f);     /* gentler turns at speed */
            tx += fsin(tyaw) * SPEED; tz += fcos(tyaw) * SPEED;
            ty  = 205.0f;                              /* steady high altitude (above the peaks) */
            ax += (tx - ax) * LAG; az += (tz - az) * LAG;
            ay += (ty - ay) * LAG; ayaw += (tyaw - ayaw) * LAG;
            acc -= target_dt; stepped = true; f++;
        }
        if (stepped) {
            render_fb(ax, az, ay, ayaw);
            load_mode7();
            ppu_render(&P, FB);
        }
        present_frame(FB);
    }

    present_shutdown();
    return 0;
}
