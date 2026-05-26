/* demo_canyon.c — MANUAL PoC: lava canyon as an EXTRUDED MESH (r3d).
 *
 * A fixed cross-section (snow plateau -> rock walls -> rock floor -> lava
 * channel) swept along +x. Only the visible rings are regenerated each
 * frame, in CAMERA-RELATIVE x (so world coords never overflow Q16.16), so
 * the canyon is endless with no wrap-jump and the world data stays a few KB.
 * The rim height undulates along the path for a forward-motion cue. Camera
 * flies below the rim; z-buffered so objects (later) occlude correctly.
 *
 * Build + run (Windows):
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_canyon \
 *      src/video/ppu.c src/video/present_gl_win32.c src/video/r3d.c \
 *      src/math/trig_q16.c src/math/fixed_point.c \
 *      src/video/tests/demo_canyon.c -lopengl32 -lgdi32 -luser32
 * Headless ASCII: add -DCANYON_HEADLESS, link without present/GL.
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#include "video/r3d.h"
#ifndef CANYON_HEADLESS
#include "video/present.h"
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>

#define FBW 120
#define FBH 104
#define NTSC_LINES     262
#define LINE_CYCLES    1364
#define VBLANK_LINES   (NTSC_LINES - PPU_SCREEN_H)
#define DMA_PER_VBLANK (VBLANK_LINES * LINE_CYCLES / 8)
#define BGR555(r,g,b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

/* palette layout (shared ramp length) */
#define RAMP      8
#define ROCK_BASE 1
#define SNOW_BASE 16
#define LAVA_BASE 32

/* canyon cross-section (z, and whether the point is at the rim or the floor) */
#define LAVA_HW   10.0f
#define FLOOR_HW  30.0f
#define WALL_RUN  12.0f
#define PLAT      40.0f
#define RIM       40.0f
#define RIM_AMP    8.0f
#define RIM_FREQ  0.04f
#define RIDE_H    18         /* camera height above the floor (below the rim) */

#define NP 8                 /* cross-section points */
#define NS 7                 /* cross-section segments */
#define MAX_OBJS 32          /* canyon + visible lava-rocks */
#define ROCK_SP  60.0f       /* lava-rock spacing along x   */
#define RINGS 200           /* long draw distance: each new far ring is ~focal/RINGS px tall */
#define SEG   6              /* ring spacing along x */
/* SWAG per-op cycle costs for the on-cart coprocessor estimate (M7, Q16). */
#define EST_CYC_VERT 50      /* transform + project per vertex     */
#define EST_CYC_TRI  100     /* cull + shade + setup per triangle  */
#define EST_CYC_PIX   25     /* edge test + z per pixel-test       */

static const float   zc[NP]    = { -(FLOOR_HW+WALL_RUN+PLAT), -(FLOOR_HW+WALL_RUN), -FLOOR_HW,
                                    -LAVA_HW, LAVA_HW, FLOOR_HW, (FLOOR_HW+WALL_RUN), (FLOOR_HW+WALL_RUN+PLAT) };
static const uint8_t at_rim[NP]= { 1, 1, 0, 0, 0, 0, 1, 1 };
static const uint8_t seg_base[NS] = { SNOW_BASE, ROCK_BASE, ROCK_BASE, LAVA_BASE, ROCK_BASE, ROCK_BASE, SNOW_BASE };

static PpuState P;
static uint8_t  fbuf[FBW * FBH];
#ifndef CANYON_HEADLESS
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
#endif

static vec3_q16 g_verts[RINGS * NP];
static uint16_t g_tris[(RINGS - 1) * NS * 2 * 3];
static uint8_t  g_tribase[(RINGS - 1) * NS * 2];
static int      g_ntris;
static R3dMesh   mesh;
static R3dScene  scene;

/* a small faceted rock (irregular octahedron) — the lava-rocks */
static vec3_q16 rock_v[6];                              /* filled in build_scene */
static const uint16_t rock_t[24] = {                    /* CCW-outward */
    2,4,0, 2,1,4, 2,5,1, 2,0,5,  3,0,4, 3,4,1, 3,1,5, 3,5,0
};
static R3dMesh   g_rockmesh;
static R3dObject g_objs[MAX_OBJS];                      /* [0]=canyon, [1..]=rocks */
static vec3_q16  g_cam_r, g_cam_u, g_cam_f;             /* camera basis (fixed look) */

/* Place the camera at eye (0, RIDE_H+dy, dz), keeping the forward look — so
 * the arrows shift the viewpoint within the canyon for parallax. */
static void set_camera(float dy, float dz) {
    scene.view = affine3_q16_view(
        vec3_q16_make(0, q16_from_double((double)RIDE_H + dy), q16_from_double(dz)),
        g_cam_r, g_cam_u, g_cam_f);
}

static float ffloor_(float x) { int i = (int)x; return (x < 0.0f && (float)i != x) ? (float)(i - 1) : (float)i; }
static float fwrap_(float x) { const float T = 6.2831853f; return x - T * ffloor_(x * (1.0f / T) + 0.5f); }
static float fsin_(float x) {                     /* O(1) range reduce — args grow unbounded */
    x = fwrap_(x);
    float a = x < 0 ? -x : x;
    return 1.2732395f * x - 0.4052847f * x * a;
}

static uint16_t lerp555(uint16_t a, uint16_t b, int num, int den) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    return BGR555(ar + (br - ar) * num / den, ag + (bg - ag) * num / den, ab + (bb - ab) * num / den);
}
static void build_palette(void) {
    ppu_state_clear(&P);
    P.mode = 7;
    P.brightness = 15;
    P.cgram[0] = BGR555(20, 18, 28);                       /* sky */
    for (int s = 0; s < RAMP; s++) {
        P.cgram[ROCK_BASE + s] = lerp555(BGR555(5, 5, 6),   BGR555(17, 16, 14), s, RAMP - 1);
        P.cgram[SNOW_BASE + s] = lerp555(BGR555(16, 18, 23), BGR555(31, 31, 31), s, RAMP - 1);
        P.cgram[LAVA_BASE + s] = lerp555(BGR555(20, 5, 0),  BGR555(31, 28, 9),  s, RAMP - 1);
    }
}

/* connectivity + per-face material — built once (only vertex positions move) */
static void build_tris(void) {
    int nt = 0;
    for (int r = 0; r < RINGS - 1; r++)
        for (int s = 0; s < NS; s++) {
            int A = r * NP + s, B = r * NP + s + 1, C = (r + 1) * NP + s + 1, D = (r + 1) * NP + s;
            /* wound so the INNER (channel-facing) surface fronts the camera */
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

    /* rock mesh (irregular radii -> faceted look) */
    static const double rr[6][3] = { {3,0,0}, {-2.5,0,0}, {0,2,0}, {0,-2,0}, {0,0,2.8}, {0,0,-2.4} };
    for (int i = 0; i < 6; i++)
        rock_v[i] = vec3_q16_make(q16_from_double(rr[i][0]), q16_from_double(rr[i][1]), q16_from_double(rr[i][2]));
    g_rockmesh.verts = rock_v; g_rockmesh.nverts = 6;
    g_rockmesh.tris = rock_t;  g_rockmesh.ntris = 8; g_rockmesh.tri_base = NULL;

    g_objs[0].mesh = &mesh; g_objs[0].xform = affine3_q16_identity();

    g_cam_r = vec3_q16_make(0, 0, Q16_ONE);                                          /* right = +z */
    g_cam_u = vec3_q16_make(q16_from_double(0.177), q16_from_double(0.984), 0);      /* up         */
    g_cam_f = vec3_q16_make(q16_from_double(0.984), q16_from_double(-0.177), 0);     /* fwd = +x, tilt down */
    set_camera(0.0f, 0.0f);
    scene.focal   = q16_from_int(100);
    scene.near_z  = q16_from_double(0.5);
    scene.light   = vec3_q16_normalize(vec3_q16_make(q16_from_double(0.3), q16_from_double(0.6), q16_from_double(0.5)));
    scene.ambient = q16_from_double(0.35);
    scene.diffuse = q16_from_double(0.65);
    scene.base    = ROCK_BASE;
    scene.ramp    = RAMP;
    scene.objs    = g_objs; scene.nobjs = 1;
}

/* place the lava-rocks visible ahead of the camera as scene objects [1..],
 * tumbling and scattered across the lava channel; scrolls with the canyon. */
static void place_objects(float cam_x) {
    int no = 1;                                         /* [0] = canyon */
    float draw = (float)((RINGS - 1) * SEG);
    int k0 = (int)ffloor_(cam_x / ROCK_SP);
    for (int k = k0; no < MAX_OBJS; k++) {
        float relx = (float)k * ROCK_SP - cam_x;
        if (relx < 1.0f) continue;                      /* at/behind the camera */
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

#ifndef CANYON_HEADLESS
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
    P.m7a = (int16_t)FBW;
    P.m7d = (int16_t)(256 * FBH / PPU_SCREEN_H);
    P.m7b = 0; P.m7c = 0; P.m7x = 0; P.m7y = 0; P.m7hofs = 0; P.m7vofs = 0;
}
#endif

#ifdef CANYON_HEADLESS
int main(void) {
    build_scene();
    gen_verts(20.0f);
    place_objects(20.0f);
    long px = r3d_render(&scene, fbuf, FBW, FBH);
    int tv = mesh.nverts + (scene.nobjs - 1) * g_rockmesh.nverts;
    int tt = g_ntris    + (scene.nobjs - 1) * g_rockmesh.ntris;
    double est = (tv * (double)EST_CYC_VERT + tt * (double)EST_CYC_TRI + px * (double)EST_CYC_PIX) / 1e6;
    printf("RINGS=%d draw~%d  %d objs  %d verts  %d tris  %ld px  ~%.2f M cyc/frame (of 16M)\n",
           RINGS, (RINGS - 1) * SEG, scene.nobjs, tv, tt, px, est);
    for (int y = 0; y < FBH; y += 3) {
        for (int x = 0; x < FBW; x += 2) {
            uint8_t v = fbuf[y * FBW + x];
            char ch = v == 0 ? ' '
                    : (v >= LAVA_BASE) ? 'L'
                    : (v >= SNOW_BASE) ? 'S'
                    : (v - ROCK_BASE) < 3 ? '.' : '#';
            putchar(ch);
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
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - r3d canyon")) return 1;
    printf("GL: %s\nr3d canyon (%dx%d -> Mode 7)\n"
           "keys: arrows = move camera  |  A/Z = faster/slower  |  I = info  V = vsync\n",
           present_gl_renderer(), FBW, FBH);
    fflush(stdout);

    build_scene();
    double t0 = now_sec(), report = t0, prev = t0;
    unsigned frames = 0;
    double fps = 0.0;
    float cam_x = 0.0f, speed = 76.0f, dy = 0.0f, dz = 0.0f;
    while (!present_should_close()) {
        double now = now_sec();
        float dt = (float)(now - prev); prev = now;
        if (dt > 0.1f) dt = 0.1f;                        /* clamp hitches */

        float mv = 40.0f * dt;                           /* camera move within a bounding box */
        if (GetAsyncKeyState(VK_UP)    & 0x8000) dy += mv;
        if (GetAsyncKeyState(VK_DOWN)  & 0x8000) dy -= mv;
        if (GetAsyncKeyState(VK_RIGHT) & 0x8000) dz += mv;
        if (GetAsyncKeyState(VK_LEFT)  & 0x8000) dz -= mv;
        if (dy >  18.0f) dy =  18.0f; else if (dy <  -6.0f) dy =  -6.0f;
        if (dz >  18.0f) dz =  18.0f; else if (dz < -18.0f) dz = -18.0f;
        if (GetAsyncKeyState('A') & 0x8000) speed += 120.0f * dt;   /* faster */
        if (GetAsyncKeyState('Z') & 0x8000) speed -= 120.0f * dt;   /* slower */
        if (speed < 8.0f) speed = 8.0f; else if (speed > 220.0f) speed = 220.0f;
        cam_x += speed * dt;

        set_camera(dy, dz);
        gen_verts(cam_x);
        place_objects(cam_x);
        long px = r3d_render(&scene, fbuf, FBW, FBH);
        load_mode7();
        ppu_render(&P, FB);

        int need = FBW * FBH, avail = 2 * DMA_PER_VBLANK;
        int tv = mesh.nverts + (scene.nobjs - 1) * g_rockmesh.nverts;
        int tt = g_ntris    + (scene.nobjs - 1) * g_rockmesh.ntris;
        double est_cyc = (tv * (double)EST_CYC_VERT + tt * (double)EST_CYC_TRI
                        + px * (double)EST_CYC_PIX) / 1e6;
        char ov[640];
        snprintf(ov, sizeof ov,
            "SNES PPU emulated (Mode 7) - geometry on host coprocessor\n"
            "rendered bitmap : %d x %d   (8bpp = %d B)\n"
            "display out     : %d x %d   (Mode 7 stretch)\n"
            "DMA / frame     : %d need | %d avail @30fps -> %s\n"
            "geometry        : %d verts | %d tris | %ld px-tests (%d objs)\n"
            "coproc est      : ~%.2f M cyc/frame  (M7 480MHz/30fps = 16M)\n"
            "controls        : arrows move | A/Z speed=%.0f | dy=%.0f dz=%.0f\n"
            "render rate     : %.1f fps (host, GPU-bound - ignore)",
            FBW, FBH, need, PPU_SCREEN_W, PPU_SCREEN_H,
            need, avail, (need <= avail ? "FITS" : "OVER"),
            tv, tt, px, scene.nobjs, est_cyc, speed, dy, dz, fps);
        present_set_overlay(ov);
        present_frame(FB);
        frames++;
        if (now - report >= 1.0) { fps = (double)frames / (now - report);
            printf("  %.1f fps | %d tris | %ld px | ~%.2fM cyc\n", fps, g_ntris, px, est_cyc);
            fflush(stdout); report = now; frames = 0; }
    }
    present_shutdown();
    return 0;
}
#endif
