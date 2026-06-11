/* demo_voxel.c — MANUAL visual PoC: pseudo-3D "hypervoxel" terrain.
 *
 * Proves the bandwidth-honest approach for Kestrel's wingsuit run: a
 * procedural heightfield is projected (Comanche-style, per screen
 * column) and QUANTIZED into a Mode 1 nametable built from a tiny
 * block-tile vocabulary, then rendered through the real PPU and shown
 * via the present shim. Only the nametable changes per frame (~the 2 KB
 * DMA budget); the tile graphics + palettes are preloaded.
 *
 * Coloring is the 4bpp + per-cell-palette trick: ONE set of "fill"
 * shape tiles, recolored per cell to rock / snow / lava and near/far
 * (atmospheric fade toward the violet Aardvark sky) purely via the
 * nametable's palette bits.
 *
 * The camera auto-flies and LAGS behind its target (critically-damped
 * follow) so you feel it catch up as speed builds — the game-feel beat.
 * Float math here for clarity; the real coprocessor would use fixed.
 *
 * Build + run on Windows:
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_voxel \
 *      src/video/ppu.c src/video/present_gl_win32.c \
 *      src/video/tests/demo_voxel.c -lopengl32 -lgdi32 -luser32 -lm
 *   ./build/demo_voxel            (add .exe on native mingw)
 *
 * Esc / window-X quits. Edit SEED for different terrain (the seed tool
 * you described would drive this).
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#include "video/present.h"

#include <windows.h>   /* QueryPerformanceCounter */
#include <stdint.h>
#include <string.h>
#include <stdio.h>
/* NOTE: no <math.h> — with -Iinclude it resolves to the repo's math
 * aggregator, not libc's, so the trig/floor helpers below are libm-free
 * (and fit the project's no-libm style anyway). */

#define SEED          65536u
#define MAPSZ         256          /* heightfield is MAPSZ x MAPSZ, wraps */
#define MAPMASK       (MAPSZ - 1)
#define SNES_NTSC_HZ  60.0988

/* Material ids -> base palette index. +3 = the depth-faded "far" variant. */
#define MAT_ROCK 0
#define MAT_SNOW 1
#define MAT_LAVA 2

static PpuState P;
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
static uint8_t  Hmap[MAPSZ * MAPSZ];   /* height 0..255   */
static uint8_t  Mmap[MAPSZ * MAPSZ];   /* material id     */

#define BGR555(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

/* Libm-free helpers (see the include note above). */
static float ffloor(float x) {
    int i = (int)x;
    return (x < 0.0f && (float)i != x) ? (float)(i - 1) : (float)i;
}
/* Parabolic sine approximation on [-pi,pi] (plenty for camera angles). */
static float fsin(float x) {
    const float PI = 3.14159265f, TWO_PI = 6.28318531f;
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    float ax = (x < 0.0f) ? -x : x;
    return 1.27323954f * x - 0.405284735f * x * ax;
}
static float fcos(float x) { return fsin(x + 1.57079633f); }

static double now_sec(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

/* ---- procedural terrain (seeded value-noise fBm) ----------- */

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
    return sum;   /* ~0..1 */
}

static void gen_terrain(uint32_t seed) {
    for (int y = 0; y < MAPSZ; y++) {
        for (int x = 0; x < MAPSZ; x++) {
            float n = fbm((float)x / 48.0f, (float)y / 48.0f, seed);
            /* ridged-ish: emphasize peaks for mountains + canyons */
            n = n * n * (3.0f - 2.0f * n);
            int h = (int)(n * 255.0f);
            if (h < 0) h = 0;
            if (h > 255) h = 255;
            uint8_t mat = MAT_ROCK;
            if (h > 160) mat = MAT_SNOW;                       /* snow caps */
            float lava = fbm((float)x / 90.0f + 17.0f,
                             (float)y / 90.0f + 41.0f, seed ^ 0x5A5Au);
            if (h < 100 && lava > 0.55f) mat = MAT_LAVA;        /* lava in low channels */
            Hmap[y * MAPSZ + x] = (uint8_t)h;
            Mmap[y * MAPSZ + x] = mat;
        }
    }
}

static float height_at(float wx, float wz) {
    int hx = (int)ffloor(wx) & MAPMASK, hy = (int)ffloor(wz) & MAPMASK;
    return (float)Hmap[hy * MAPSZ + hx];
}

/* ---- tile vocabulary + palettes ---------------------------- */

static void set_tile_4bpp(uint16_t *vram, unsigned cw, unsigned tile, uint8_t p[8][8]) {
    unsigned base = (cw + tile * 16u) & 0x7FFFu;
    for (unsigned r = 0; r < 8; r++) {
        unsigned p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (unsigned x = 0; x < 8; x++) {
            unsigned v = p[r][x], b = 7u - x;
            p0 |= (v & 1u) << b;        p1 |= ((v >> 1) & 1u) << b;
            p2 |= ((v >> 2) & 1u) << b; p3 |= ((v >> 3) & 1u) << b;
        }
        vram[(base + r) & 0x7FFFu]      = (uint16_t)(p0 | (p1 << 8));
        vram[(base + 8u + r) & 0x7FFFu] = (uint16_t)(p2 | (p3 << 8));
    }
}

#define TILEMAP_W 0x0000u
#define CHAR_W    0x1000u

/* Blend a color toward the sky (atmospheric perspective) for far palettes. */
static uint16_t fade(uint16_t c, uint16_t sky, float t) {
    int r = c & 31, g = (c >> 5) & 31, b = (c >> 10) & 31;
    int sr = sky & 31, sg = (sky >> 5) & 31, sb = (sky >> 10) & 31;
    r += (int)((sr - r) * t); g += (int)((sg - g) * t); b += (int)((sb - b) * t);
    return BGR555(r, g, b);
}

static void build_vocab_and_palettes(void) {
    /* Tile 0 = sky (transparent). Tiles 1..8 = terrain filling the bottom
     * k rows; the top filled row is the "lit rim" (value 2), the rest is
     * the surface (value 1). */
    uint8_t t[8][8];
    for (unsigned k = 1; k <= 8; k++) {
        for (int r = 0; r < 8; r++)
            for (int c = 0; c < 8; c++)
                t[r][c] = (r >= (int)(8u - k)) ? (r == (int)(8u - k) ? 2 : 1) : 0;
        set_tile_4bpp(P.vram, CHAR_W, k, t);
    }

    const uint16_t sky = BGR555(17, 6, 26);     /* Aardvark violet */
    P.cgram[0] = sky;

    struct { uint16_t base, hi; } mat[3] = {
        { BGR555(13, 11, 9),  BGR555(20, 18, 16) },  /* rock  */
        { BGR555(23, 26, 31), BGR555(31, 31, 31) },  /* snow  */
        { BGR555(31, 7, 1),   BGR555(31, 24, 5)  },  /* lava  */
    };
    for (int m = 0; m < 3; m++) {
        P.cgram[(m) * 16 + 1] = mat[m].base;            /* near palette */
        P.cgram[(m) * 16 + 2] = mat[m].hi;
        P.cgram[(m + 3) * 16 + 1] = fade(mat[m].base, sky, 0.62f);  /* far (hazed) */
        P.cgram[(m + 3) * 16 + 2] = fade(mat[m].hi,   sky, 0.62f);
    }
}

/* ---- projector: heightfield -> Mode 1 nametable ------------ */

static void render_terrain(float cx, float cz, float cy, float yaw) {
    const int   TCOLS   = PPU_SCREEN_W / 8;   /* 32 */
    const int   TROWS   = PPU_SCREEN_H / 8;   /* 28 */
    const float FOV     = 1.15f;
    const float HORIZON = 96.0f;              /* horizon screen row (pitch) */
    const float HSCALE  = 150.0f;
    const float ZNEAR   = 4.0f, ZFAR = 200.0f, FARZ = 80.0f;

    for (int tc = 0; tc < TCOLS; tc++) {
        float scx = (float)(tc * 8 + 4);
        float ang = yaw + (scx - 128.0f) / 128.0f * (FOV * 0.5f);
        float dx = fsin(ang), dz = fcos(ang);

        /* Comanche span fill: march near->far, painting each newly-visible
         * band (terrain that rises above what nearer terrain already drew)
         * with ITS material + depth. Gives the receding, shaded slope. */
        int8_t  rmat[PPU_SCREEN_H];      /* material per screen row, -1 = sky */
        uint8_t rfar[PPU_SCREEN_H];      /* depth band (0 near, 1 far)        */
        for (int y = 0; y < PPU_SCREEN_H; y++) { rmat[y] = -1; rfar[y] = 0; }

        int ybuf = PPU_SCREEN_H;         /* lowest row not yet painted */
        for (float z = ZNEAR; z < ZFAR; z += 1.0f) {
            float wx = cx + dx * z, wz = cz + dz * z;
            int hx = (int)ffloor(wx) & MAPMASK, hy = (int)ffloor(wz) & MAPMASK;
            float h = (float)Hmap[hy * MAPSZ + hx];
            int sy = (int)(HORIZON - (h - cy) * HSCALE / z);
            if (sy < 0) sy = 0;
            if (sy < ybuf) {             /* this terrain rises above the slope so far */
                int mat = Mmap[hy * MAPSZ + hx];
                uint8_t isfar = (z > FARZ) ? 1u : 0u;   /* 'far' is a windows.h macro */
                for (int y = sy; y < ybuf; y++) { rmat[y] = (int8_t)mat; rfar[y] = isfar; }
                ybuf = sy;
            }
        }

        /* Collapse screen rows to tile cells. Terrain is contiguous from the
         * bottom up to the silhouette, so a cell's terrain is its bottom
         * `fill` rows; material/depth come from the nearest (lowest) row. */
        for (int tr = 0; tr < TROWS; tr++) {
            int top = tr * 8, bot = tr * 8 + 8, fill = 0, mat = MAT_ROCK, isfar = 0;
            for (int y = top; y < bot; y++)
                if (rmat[y] >= 0) { fill++; mat = rmat[y]; isfar = rfar[y]; }
            uint16_t tile = (fill == 0) ? 0u : (fill >= 8) ? 8u : (uint16_t)fill;
            int pal = mat + (isfar ? 3 : 0);
            P.vram[(TILEMAP_W + (unsigned)tr * 32u + (unsigned)tc) & 0x7FFFu] =
                (uint16_t)(tile | ((unsigned)pal << 10));
        }
    }
}

int main(void) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - hypervoxel PoC")) {
        return 1;
    }
    printf("GL renderer : %s\n", present_gl_renderer());
    printf("hypervoxel: seeded terrain, Comanche projector -> Mode 1 nametable.\n");
    printf("the camera lags its target so it catches up as speed builds.\n");
    fflush(stdout);

    ppu_state_clear(&P);
    P.mode = 1;
    P.bg[0].on_main = true;
    P.bg[0].tilemap_word = TILEMAP_W;
    P.bg[0].char_word    = CHAR_W;
    build_vocab_and_palettes();
    gen_terrain(SEED);

    /* Camera: target flies forward with a gentle S-weave; the actual
     * camera follows with damping (the catch-up feel). */
    float tx = 128.0f, tz = 0.0f, ty = 0.0f, tyaw = 0.0f;
    float ax = 128.0f, az = 0.0f, ay = 200.0f, ayaw = 0.0f;
    const float SPEED = 0.45f, LAG = 0.06f;

    const double target_dt = 1.0 / SNES_NTSC_HZ;
    double last = now_sec(), acc = 0.0;
    unsigned f = 0;
    render_terrain(ax, az, ay, ayaw);

    while (!present_should_close()) {
        double t = now_sec();
        acc += t - last; last = t;
        if (acc > 0.25) acc = 0.25;
        bool stepped = false;
        while (acc >= target_dt) {
            tyaw = 0.6f * fsin((float)f * 0.012f);     /* weaving flight path */
            tx  += fsin(tyaw) * SPEED;
            tz  += fcos(tyaw) * SPEED;
            ty   = height_at(tx, tz) + 55.0f;          /* glide above the surface */
            ax  += (tx - ax) * LAG;   az += (tz - az) * LAG;   /* damped follow */
            ay  += (ty - ay) * LAG;   ayaw += (tyaw - ayaw) * LAG;
            acc -= target_dt; stepped = true; f++;
        }
        if (stepped) {
            render_terrain(ax, az, ay, ayaw);
            ppu_render(&P, FB);
        }
        present_frame(FB);
    }

    present_shutdown();
    return 0;
}
