/* demo_canyon4.c — R&D PoC: the r3d lava canyon rendered the Star-Fox way —
 * a 4bpp tiled Mode-1 background at the FMV resolution (240x208), with the
 * shade ramps ordered-dithered for "more colours than the palette".
 *
 * This is the *parallel* 4bpp path: it shares the r3d rasterizer (via the new
 * r3d_render_dither) and the canyon scene, but instead of stretching a small
 * 8bpp bitmap through Mode 7 it swizzles a native 240x208 frame into SNES 4bpp
 * tiles (CGRAM + tilemap + CHR) and feeds the emulated PPU as a Mode-1 BG —
 * exactly the transport demo_fmv proved. The committed Mode-7 canyon
 * (demo_canyon.c) is left untouched for later R&D.
 *
 * Palettes: one per material (rock / snow / lava); each 8x8 tile takes the
 * palette of its dominant material, and a pixel's shade index carries across
 * material boundaries (brightness preserved, hue snaps to the tile's palette —
 * the acceptable Star-Fox straddle artifact). Sky is the shared backdrop.
 *
 * The framerate toggle (1 = 20 fps, 2 = 30 fps) steps the flythrough at that
 * cadence so 20-vs-30 motion can be eyeballed; the HUD reports whether a
 * 240x208 block actually fits that fps's vblank DMA budget on real hardware,
 * and the tallest image that would.
 *
 * Build + run (Windows):
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_canyon4 \
 *      src/video/ppu.c src/video/present_gl_win32.c src/video/r3d.c \
 *      src/math/trig_q16.c src/math/fixed_point.c \
 *      src/video/tests/demo_canyon4.c -lopengl32 -lgdi32 -luser32
 * Headless ASCII (frame 0, no GL): add -DCANYON4_HEADLESS, link without present/GL.
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#include "video/r3d.h"
#ifndef CANYON4_HEADLESS
#include "video/present.h"
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- 4bpp tiled target (matches the FMV engine) ---- */
#define VW 240
#define VH 208
#define TW (VW/8)          /* 30 */
#define TH (VH/8)          /* 26 */
#define NTILES (TW*TH)     /* 780 */
#define TMAP_W 0x0000      /* nametable base (word)        */
#define CHR_W  0x2000      /* CHR base (word, 8KB-aligned) */
#define BLANK_TILE NTILES  /* tile 780 = zeroed = backdrop, for the margins */
#define BLOCK (8*16*2 + NTILES*2 + NTILES*32)   /* 26776 B DMA'd per frame */

/* SNES NTSC DMA budget (for the HUD). Letterboxing to H lines forces-blank the
 * rest, so the blank window — and the per-vblank DMA — grows as H shrinks. */
#define NTSC_LINES 262
#define LINE_CYC   1364

/* ONE shared 15-colour palette holds all three material ramps (rock 1..5,
 * snow 6..10, lava 11..15); index 0 = sky backdrop. Every tile uses palette 0,
 * so the fb index IS the 4bpp colour index and a tile straddling two materials
 * shows both correctly — no per-tile palette choice, no edge treatment (the
 * Star-Fox single-palette model). 5 shades/material, smoothed by the dither. */
#define RAMP4     5
#define ROCK_BASE 1        /* CGRAM 1..5   */
#define SNOW_BASE 6        /* CGRAM 6..10  */
#define LAVA_BASE 11       /* CGRAM 11..15 */

/* canyon cross-section (z, rim flag) — same shape as demo_canyon.c */
#define LAVA_HW   10.0f
#define FLOOR_HW  30.0f
#define WALL_RUN  12.0f
#define PLAT      40.0f
#define RIM       40.0f
#define RIM_AMP    8.0f
#define RIM_FREQ  0.04f
#define RIDE_H    18

#define NP 8
#define NS 7
#define MAX_OBJS 32
#define ROCK_SP  60.0f
#define RINGS 200
#define SEG   6

static const float   zc[NP]     = { -(FLOOR_HW+WALL_RUN+PLAT), -(FLOOR_HW+WALL_RUN), -FLOOR_HW,
                                     -LAVA_HW, LAVA_HW, FLOOR_HW, (FLOOR_HW+WALL_RUN), (FLOOR_HW+WALL_RUN+PLAT) };
static const uint8_t at_rim[NP]  = { 1, 1, 0, 0, 0, 0, 1, 1 };
static const uint8_t seg_base[NS]= { SNOW_BASE, ROCK_BASE, ROCK_BASE, LAVA_BASE, ROCK_BASE, ROCK_BASE, SNOW_BASE };

static PpuState P;
static uint8_t  fbuf[VW * VH];       /* 8bpp render target (dithered ramp indices) */
#ifndef CANYON4_HEADLESS
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
#endif

static vec3_q16 g_verts[RINGS * NP];
static uint16_t g_tris[(RINGS - 1) * NS * 2 * 3];
static uint8_t  g_tribase[(RINGS - 1) * NS * 2];
static int      g_ntris;
static R3dMesh   mesh;
static R3dScene  scene;

static vec3_q16 rock_v[6];
static const uint16_t rock_t[24] = { 2,4,0, 2,1,4, 2,5,1, 2,0,5,  3,0,4, 3,4,1, 3,1,5, 3,5,0 };
static R3dMesh   g_rockmesh;
static R3dObject g_objs[MAX_OBJS];
static vec3_q16  g_cam_r, g_cam_u, g_cam_f;

#define BGR555(r,g,b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

static void set_camera(float dy, float dz) {
    scene.view = affine3_q16_view(
        vec3_q16_make(0, q16_from_double((double)RIDE_H + dy), q16_from_double(dz)),
        g_cam_r, g_cam_u, g_cam_f);
}

static float ffloor_(float x) { int i = (int)x; return (x < 0.0f && (float)i != x) ? (float)(i - 1) : (float)i; }
static float fwrap_(float x) { const float T = 6.2831853f; return x - T * ffloor_(x * (1.0f / T) + 0.5f); }
static float fsin_(float x) { x = fwrap_(x); float a = x < 0 ? -x : x; return 1.2732395f * x - 0.4052847f * x * a; }

static uint16_t lerp555(uint16_t a, uint16_t b, int num, int den) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    return BGR555(ar + (br - ar) * num / den, ag + (bg - ag) * num / den, ab + (bb - ab) * num / den);
}

/* one 15-colour ramp per material, laid out so CGRAM index == fb shade index. */
static void build_palette(void) {
    ppu_state_clear(&P);
    P.mode = 1;
    P.brightness = 15;
    P.cgram[0] = BGR555(20, 18, 28);                       /* sky = shared backdrop */
    for (int s = 0; s < RAMP4; s++) {
        P.cgram[ROCK_BASE + s] = lerp555(BGR555(5, 5, 6),    BGR555(17, 16, 14), s, RAMP4 - 1);
        P.cgram[SNOW_BASE + s] = lerp555(BGR555(16, 18, 23), BGR555(31, 31, 31), s, RAMP4 - 1);
        P.cgram[LAVA_BASE + s] = lerp555(BGR555(20, 5, 0),   BGR555(31, 28, 9),  s, RAMP4 - 1);
    }
}

static void build_tris(void) {
    int nt = 0;
    for (int r = 0; r < RINGS - 1; r++)
        for (int s = 0; s < NS; s++) {
            int A = r * NP + s, B = r * NP + s + 1, C = (r + 1) * NP + s + 1, D = (r + 1) * NP + s;
            g_tris[nt*3] = (uint16_t)A; g_tris[nt*3+1] = (uint16_t)C; g_tris[nt*3+2] = (uint16_t)B;
            g_tribase[nt] = seg_base[s]; nt++;
            g_tris[nt*3] = (uint16_t)A; g_tris[nt*3+1] = (uint16_t)D; g_tris[nt*3+2] = (uint16_t)C;
            g_tribase[nt] = seg_base[s]; nt++;
        }
    g_ntris = nt;
}

static void gen_verts(float cam_x) {
    int seg0 = (int)ffloor_(cam_x / (float)SEG);
    for (int r = 0; r < RINGS; r++) {
        float gx   = (float)(seg0 - 1 + r) * (float)SEG;
        float relx = gx - cam_x;
        float rim  = RIM + RIM_AMP * fsin_(gx * RIM_FREQ);
        for (int i = 0; i < NP; i++) {
            float y = at_rim[i] ? rim : 0.0f;
            g_verts[r * NP + i] = vec3_q16_make(q16_from_double(relx), q16_from_double(y), q16_from_double(zc[i]));
        }
    }
}

static void build_scene(void) {
    build_palette();
    build_tris();
    mesh.verts = g_verts; mesh.nverts = RINGS * NP;
    mesh.tris  = g_tris;  mesh.ntris  = g_ntris;
    mesh.tri_base = g_tribase;

    static const double rr[6][3] = { {3,0,0}, {-2.5,0,0}, {0,2,0}, {0,-2,0}, {0,0,2.8}, {0,0,-2.4} };
    for (int i = 0; i < 6; i++)
        rock_v[i] = vec3_q16_make(q16_from_double(rr[i][0]), q16_from_double(rr[i][1]), q16_from_double(rr[i][2]));
    g_rockmesh.verts = rock_v; g_rockmesh.nverts = 6;
    g_rockmesh.tris = rock_t;  g_rockmesh.ntris = 8; g_rockmesh.tri_base = NULL;

    g_objs[0].mesh = &mesh; g_objs[0].xform = affine3_q16_identity();

    g_cam_r = vec3_q16_make(0, 0, Q16_ONE);
    g_cam_u = vec3_q16_make(q16_from_double(0.177), q16_from_double(0.984), 0);
    g_cam_f = vec3_q16_make(q16_from_double(0.984), q16_from_double(-0.177), 0);
    set_camera(0.0f, 0.0f);
    scene.focal   = q16_from_int(200);                     /* 2x demo_canyon: 2x the fb width */
    scene.near_z  = q16_from_double(0.5);
    scene.light   = vec3_q16_normalize(vec3_q16_make(q16_from_double(0.3), q16_from_double(0.6), q16_from_double(0.5)));
    scene.ambient = q16_from_double(0.30);
    scene.diffuse = q16_from_double(0.70);
    scene.base    = ROCK_BASE;
    scene.ramp    = RAMP4;
    scene.objs    = g_objs; scene.nobjs = 1;
}

static void place_objects(float cam_x) {
    int no = 1;
    float draw = (float)((RINGS - 1) * SEG);
    int k0 = (int)ffloor_(cam_x / ROCK_SP);
    for (int k = k0; no < MAX_OBJS; k++) {
        float relx = (float)k * ROCK_SP - cam_x;
        if (relx < 1.0f) continue;
        if (relx > draw) break;
        unsigned h = (unsigned)k * 2654435761u;
        float z = (((float)((h >> 16) & 0xFFu) / 255.0f) * 2.0f - 1.0f) * (LAVA_HW - 2.0f);
        float ang = cam_x * 0.015f + (float)k * 1.3f;
        affine3_q16 rot = affine3_q16_from_rotation(
            mat3_q16_mul(mat3_q16_rotation_y(q16_from_double(fwrap_(ang))),
                         mat3_q16_rotation_x(q16_from_double(fwrap_(ang * 0.6f)))));
        g_objs[no].mesh  = &g_rockmesh;
        g_objs[no].xform = affine3_q16_compose(
            affine3_q16_from_translation(vec3_q16_make(q16_from_double(relx),
                                                       q16_from_double(0.8), q16_from_double(z))),
            rot);
        no++;
    }
    scene.nobjs = no;
}

/* swizzle the 8bpp dithered frame -> SNES 4bpp tiles (CHR + tilemap), centered
 * in the 256x224 field with a one-tile backdrop margin (16px letterbox).
 * Single shared palette 0: the fb index IS the 4bpp colour index, so there is
 * no per-tile palette decision and no boundary treatment. */
static void load_4bpp(void) {
    static uint8_t chr[NTILES][32];
    for (int i = 0; i < 1024; i++) P.vram[TMAP_W + i] = BLANK_TILE;   /* margins -> backdrop */
    for (int t = 0; t < NTILES; t++) {
        int tx = (t % TW) * 8, ty = (t / TW) * 8;
        memset(chr[t], 0, 32);
        for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) {
            int bi = fbuf[(ty + yy) * VW + (tx + xx)] & 0x0F;         /* 0 = sky, 1..15 = colour */
            int row = yy, bit = 7 - xx;
            chr[t][row*2+0]    |= ((bi>>0)&1)<<bit;
            chr[t][row*2+1]    |= ((bi>>1)&1)<<bit;
            chr[t][16+row*2+0] |= ((bi>>2)&1)<<bit;
            chr[t][16+row*2+1] |= ((bi>>3)&1)<<bit;
        }
        int r = t / TW, c = t % TW;
        P.vram[TMAP_W + (r + 1) * 32 + (c + 1)] = (uint16_t)t;        /* palette 0 for every tile */
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

/* tallest image (in 8px tile-rows) whose 4bpp block fits `windows` vblanks. */
static int max_fit_rows(int windows) {
    for (int th = TH; th >= 1; th--) {
        long block = 8*16*2 + (long)(TW*th)*2 + (long)(TW*th)*32;
        int  blank = NTSC_LINES - th * 8;
        long avail = (long)windows * blank * LINE_CYC / 8;
        if (block <= avail) return th;
    }
    return 0;
}

#ifdef CANYON4_HEADLESS
int main(void) {
    build_scene();
    setup_bg();
    gen_verts(20.0f);
    place_objects(20.0f);
    long px = r3d_render_dither(&scene, fbuf, VW, VH);
    load_4bpp();
    static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
    ppu_render(&P, FB);
    printf("4bpp canyon %dx%d (%d tiles, %d B/frame)  %ld px-tests\n", VW, VH, NTILES, BLOCK, px);
    printf("DMA fit: 20fps -> %dx%d | 30fps -> %dx%d\n",
           VW, max_fit_rows(3) * 8, VW, max_fit_rows(2) * 8);
    for (int y = 0; y < PPU_SCREEN_H; y += 7) {
        for (int x = 0; x < PPU_SCREEN_W; x += 4) {
            uint32_t c = FB[y * PPU_SCREEN_W + x];
            unsigned r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, lum = (r + g + b) / 3;
            putchar(lum < 50 ? ' ' : lum < 110 ? '.' : lum < 180 ? ':' : '#');
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
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - r3d canyon (4bpp dithered)")) return 1;
    printf("GL: %s\nr3d canyon (4bpp dithered -> Mode 1, %dx%d)\n"
           "keys: arrows = move | A/Z = faster/slower | 1 = 20fps  2 = 30fps | I info  V vsync\n",
           present_gl_renderer(), VW, VH);
    fflush(stdout);

    build_scene();
    setup_bg();

    double t0 = now_sec(), report = t0;
    unsigned presents = 0; double hostfps = 0.0;
    float cam_x = 0.0f, speed = 120.0f, dy = 0.0f, dz = 0.0f;
    int fps = 20;                                          /* simulated flythrough cadence */
    double next_step = t0;
    long px = 0;
    while (!present_should_close()) {
        double now = now_sec();
        if (GetAsyncKeyState('1') & 0x8000) fps = 20;
        if (GetAsyncKeyState('2') & 0x8000) fps = 30;

        /* step the sim at the chosen fps so 20-vs-30 motion is what you see */
        double step = 1.0 / fps;
        if (now >= next_step) {
            if (now - next_step > 0.25) next_step = now;   /* resync after a hitch/toggle */
            float dt = (float)step;
            float mv = 40.0f * dt;
            if (GetAsyncKeyState(VK_UP)    & 0x8000) dy += mv;
            if (GetAsyncKeyState(VK_DOWN)  & 0x8000) dy -= mv;
            if (GetAsyncKeyState(VK_RIGHT) & 0x8000) dz += mv;
            if (GetAsyncKeyState(VK_LEFT)  & 0x8000) dz -= mv;
            if (dy >  18.0f) dy =  18.0f; else if (dy <  -6.0f) dy =  -6.0f;
            if (dz >  18.0f) dz =  18.0f; else if (dz < -18.0f) dz = -18.0f;
            if (GetAsyncKeyState('A') & 0x8000) speed += 250.0f * dt;
            if (GetAsyncKeyState('Z') & 0x8000) speed -= 250.0f * dt;
            if (speed < 8.0f) speed = 8.0f; else if (speed > 900.0f) speed = 900.0f;
            cam_x += speed * dt;

            set_camera(dy, dz);
            gen_verts(cam_x);
            place_objects(cam_x);
            px = r3d_render_dither(&scene, fbuf, VW, VH);
            load_4bpp();
            ppu_render(&P, FB);
            next_step += step;
        }

        int windows = (60 + fps - 1) / fps;                /* vblanks per video frame: 3@20, 2@30 */
        int blank   = NTSC_LINES - VH;                     /* 54 forced-blank+vblank lines @208 */
        long winB   = (long)blank * LINE_CYC / 8;          /* bytes / 60Hz window */
        long avail  = (long)windows * winB;
        int  fitH   = max_fit_rows(windows) * 8;
        int tv = mesh.nverts + (scene.nobjs - 1) * g_rockmesh.nverts;
        int tt = g_ntris    + (scene.nobjs - 1) * g_rockmesh.ntris;
        char ov[700];
        snprintf(ov, sizeof ov,
            "SNES PPU emulated (Mode 1, 4bpp dithered) - geometry on host coprocessor\n"
            "video res     : %d x %d   (4bpp tiles, 1 shared palette, ordered dither)\n"
            "DMA / frame   : %d B   (CGRAM 256 + tilemap %d + CHR %d)\n"
            "blank window  : %d lines @208 -> %ld B / 60Hz frame\n"
            "DMA @%d fps    : %d need | %ld avail (%d windows) -> %s   [%dfps max fits %dx%d]\n"
            "geometry      : %d verts | %d tris | %ld px-tests (%d objs)\n"
            "controls      : arrows move | A/Z speed=%.0f | 1=20fps 2=30fps | I V\n"
            "host present  : %.1f fps (GPU-bound - ignore; sim steps at %d fps)",
            VW, VH, BLOCK, NTILES*2, NTILES*32,
            blank, winB,
            fps, BLOCK, avail, windows, (BLOCK <= avail ? "FITS" : "OVER"), fps, VW, fitH,
            tv, tt, px, scene.nobjs, speed, hostfps, fps);
        present_set_overlay(ov);
        present_frame(FB);
        presents++;
        if (now - report >= 1.0) { hostfps = (double)presents / (now - report); report = now; presents = 0; }
    }
    present_shutdown();
    return 0;
}
#endif
