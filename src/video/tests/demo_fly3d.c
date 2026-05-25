/* demo_fly3d.c — MANUAL visual PoC: full-3D heightfield flyover.
 *
 * The roll-capable evolution of demo_voxel7. Same display path (software
 * 8bpp framebuffer -> Mode 7 char + linear tilemap -> matrix STRETCH to
 * 256x224, scale-only — NO matrix rotation, so no square edges), but the
 * renderer is now a FULL per-pixel 3D heightfield raycaster: each
 * framebuffer pixel casts a ray from the camera's yaw/pitch/ROLL basis
 * and marches the terrain. Roll and inversions are baked into the render,
 * so the camera can bank and fly upside-down cleanly.
 *
 * Per-pixel raycasting is heavier than the per-column projector (~80x72
 * rays vs 80), which is exactly why the framebuffer is low-res — that's
 * the perf budget that makes full 3D affordable (a fast coprocessor can
 * do it fixed-point at 60 fps).
 *
 * Build + run on Windows:
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_fly3d \
 *      src/video/ppu.c src/video/present_gl_win32.c \
 *      src/video/tests/demo_fly3d.c -lopengl32 -lgdi32 -luser32
 *   ./build/demo_fly3d
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#include "video/present.h"

#include <windows.h>   /* QueryPerformanceCounter */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
/* no <math.h> (resolves to the repo math aggregator under -Iinclude) */

#define SEED          24601u
#define MAPSZ         256
#define MAPMASK       (MAPSZ - 1)
#define SNES_NTSC_HZ  60.0988

#define FBW  80
#define FBH  72

#define RAMP 20
#define ROCK_BASE 1
#define SNOW_BASE (ROCK_BASE + RAMP)
#define LAVA_BASE (SNOW_BASE + RAMP)

#define MAT_ROCK 0
#define MAT_SNOW 1
#define MAT_LAVA 2

#define BGR555(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

static PpuState P;
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
static uint8_t  Hmap[MAPSZ * MAPSZ];
static uint8_t  Mmap[MAPSZ * MAPSZ];
static uint8_t  fbuf[FBW * FBH];

/* ---- libm-free helpers ------------------------------------- */

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
static float fsqrt(float x) {
    if (x <= 0.0f) return 0.0f;
    float r = x;
    for (int i = 0; i < 6; i++) r = 0.5f * (r + x / r);
    return r;
}

typedef struct { float x, y, z; } V3;
static V3 v3(float x, float y, float z) { V3 r = { x, y, z }; return r; }
static V3 vcross(V3 a, V3 b) {
    return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
}

/* ---- terrain (same generator as the other voxel demos) ----- */

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
static float height_at(float wx, float wz) {
    int hx = (int)ffloor(wx) & MAPMASK, hy = (int)ffloor(wz) & MAPMASK;
    return (float)Hmap[hy * MAPSZ + hx];
}
static int mat_at(float wx, float wz) {
    int hx = (int)ffloor(wx) & MAPMASK, hy = (int)ffloor(wz) & MAPMASK;
    return Mmap[hy * MAPSZ + hx];
}

/* ---- palette: depth-shaded material ramps ------------------ */

static uint16_t lerp555(uint16_t a, uint16_t b, float t) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    return BGR555(ar + (int)((br - ar) * t), ag + (int)((bg - ag) * t), ab + (int)((bb - ab) * t));
}
static void build_palette(void) {
    const uint16_t sky = BGR555(17, 6, 26);
    P.cgram[0] = sky;
    struct { uint16_t nr, fr; } m[3] = {
        { BGR555(22, 20, 17), lerp555(BGR555(6, 5, 6),    sky, 0.5f) },
        { BGR555(31, 31, 31), lerp555(BGR555(13, 16, 22), sky, 0.5f) },
        { BGR555(31, 27, 7),  lerp555(BGR555(16, 3, 2),   sky, 0.35f) },
    };
    int base[3] = { ROCK_BASE, SNOW_BASE, LAVA_BASE };
    for (int mat = 0; mat < 3; mat++)
        for (int s = 0; s < RAMP; s++) {
            float t = (float)s / (float)(RAMP - 1);
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

/* ---- full-3D heightfield raycaster ------------------------- */

static void render_fb(float cx, float cy, float cz, float yaw, float pitch, float roll) {
    const float FOV    = 1.15f;
    const float MAXT   = 320.0f;
    const float DTNEAR = 1.3f, DTK = 0.015f;
    const int   MAXSTEPS = 320;

    /* Orthonormal camera basis with roll applied around the view axis. */
    float cp = fcos(pitch), sp = fsin(pitch), sy = fsin(yaw), cyw = fcos(yaw);
    V3 fwd    = v3(cp * sy, sp, cp * cyw);
    V3 right0 = v3(cyw, 0.0f, -sy);
    V3 up0    = vcross(fwd, right0);
    float cr = fcos(roll), sr = fsin(roll);
    V3 right = v3(right0.x * cr + up0.x * sr, right0.y * cr + up0.y * sr, right0.z * cr + up0.z * sr);
    V3 up    = v3(up0.x * cr - right0.x * sr, up0.y * cr - right0.y * sr, up0.z * cr - right0.z * sr);

    float halfh = fsin(FOV * 0.5f) / fcos(FOV * 0.5f);
    float halfw = halfh * ((float)PPU_SCREEN_W / (float)PPU_SCREEN_H);

    for (int py = 0; py < FBH; py++) {
        float ny = (1.0f - ((float)py + 0.5f) / (float)FBH * 2.0f) * halfh;
        for (int px = 0; px < FBW; px++) {
            float nx = (((float)px + 0.5f) / (float)FBW * 2.0f - 1.0f) * halfw;
            V3 d = v3(fwd.x + nx * right.x + ny * up.x,
                      fwd.y + nx * right.y + ny * up.y,
                      fwd.z + nx * right.z + ny * up.z);
            float inv = 1.0f / fsqrt(1.0f + nx * nx + ny * ny);
            d.x *= inv; d.y *= inv; d.z *= inv;

            uint8_t out = 0;                         /* sky */
            float t = 2.0f, prev = 2.0f;
            for (int s = 0; s < MAXSTEPS; s++) {
                float wx = cx + d.x * t, wy = cy + d.y * t, wz = cz + d.z * t;
                if (d.y >= 0.0f && wy > 255.0f) break;   /* ray climbing above all terrain -> sky */
                if (wy <= height_at(wx, wz)) {           /* hit: refine the crossing */
                    float lo = prev, hi = t;
                    for (int it = 0; it < 4; it++) {
                        float mid = (lo + hi) * 0.5f;
                        if (cy + d.y * mid <= height_at(cx + d.x * mid, cz + d.z * mid)) hi = mid; else lo = mid;
                    }
                    float bright = 1.0f - hi / MAXT;
                    if (bright < 0.0f) bright = 0.0f;
                    out = shade_index(mat_at(cx + d.x * hi, cz + d.z * hi), bright);
                    break;
                }
                prev = t;
                t += DTNEAR + t * DTK;
                if (t > MAXT) break;
            }
            fbuf[py * FBW + px] = out;
        }
    }
}

/* ---- load framebuffer into Mode 7 (scale-only) ------------- */

static void load_mode7(void) {
    const int TW = FBW / 8;
    for (int ty = 0; ty < FBH / 8; ty++)
        for (int tx = 0; tx < TW; tx++) {
            unsigned tile = (unsigned)(ty * TW + tx);
            for (int py = 0; py < 8; py++)
                for (int px = 0; px < 8; px++) {
                    uint8_t idx = fbuf[(ty * 8 + py) * FBW + (tx * 8 + px)];
                    unsigned w = (tile * 64u + (unsigned)py * 8u + (unsigned)px) & 0x7FFFu;
                    P.vram[w] = (uint16_t)((P.vram[w] & 0x00FFu) | ((unsigned)idx << 8));
                }
            unsigned mw = ((unsigned)ty * 128u + (unsigned)tx) & 0x7FFFu;
            P.vram[mw] = (uint16_t)((P.vram[mw] & 0xFF00u) | (tile & 0xFFu));
        }
    /* Pure scale — roll lives in the render now, so Mode 7 never rotates. */
    P.m7a = (int16_t)FBW;
    P.m7d = (int16_t)(256 * FBH / PPU_SCREEN_H);
    P.m7b = 0; P.m7c = 0; P.m7x = 0; P.m7y = 0; P.m7hofs = 0; P.m7vofs = 0;
}

int main(void) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - full-3D flyover")) {
        return 1;
    }
    printf("GL renderer : %s\n", present_gl_renderer());
    printf("full-3D heightfield raycast (%dx%d) -> Mode 7 stretch; rolls + inverts.\n", FBW, FBH);
    fflush(stdout);

    ppu_state_clear(&P);
    P.mode = 7;
    build_palette();
    gen_terrain(SEED);

    float px = 128.0f, pz = 0.0f;                /* camera position (high soar) */
    float apx = 128.0f, apz = 0.0f;              /* lagged position */
    const float SPEED = 2.2f, LAG = 0.06f;

    const double target_dt = 1.0 / SNES_NTSC_HZ;
    double last = now_sec(), acc = 0.0;
    unsigned f = 0;

    while (!present_should_close()) {
        double t = now_sec();
        acc += t - last; last = t;
        if (acc > 0.25) acc = 0.25;
        bool stepped = false;
        float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
        while (acc >= target_dt) {
            yaw   = 0.3f * fsin((float)f * 0.010f);          /* heading weave   */
            pitch = -0.42f + 0.10f * fsin((float)f * 0.013f); /* look down a bit */
            roll  = 3.0f * fsin((float)f * 0.006f);          /* barrel roll to inverted */
            px += fsin(yaw) * SPEED; pz += fcos(yaw) * SPEED;
            apx += (px - apx) * LAG; apz += (pz - apz) * LAG;
            acc -= target_dt; stepped = true; f++;
        }
        if (stepped) {
            yaw   = 0.3f * fsin((float)(f) * 0.010f);
            pitch = -0.42f + 0.10f * fsin((float)(f) * 0.013f);
            roll  = 3.0f * fsin((float)(f) * 0.006f);
            render_fb(apx, 205.0f, apz, yaw, pitch, roll);   /* 205 = high altitude */
            load_mode7();
            ppu_render(&P, FB);
        }
        present_frame(FB);
    }

    present_shutdown();
    return 0;
}
