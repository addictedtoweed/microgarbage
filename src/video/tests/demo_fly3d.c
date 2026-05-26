/* demo_fly3d.c — MANUAL visual PoC: lava-canyon course flythrough.
 *
 * A dynamic-follow chase camera racing a carved course: mostly an
 * enclosed lava canyon (lava floor, high rock walls), with stretches
 * that open out (low walls, open sky) and others that close into a
 * tunnel (low ceiling), and the channel's height undulates. Rendered by
 * a full per-pixel 3D raycaster over FLOOR + CEILING heightfields (so
 * walls and overhangs/tunnels work, not just terrain you fly over),
 * stretched to 256x224 via Mode 7 (scale-only — no matrix rotation, so
 * roll is real 3D, baked into the render, no square edges).
 *
 * The camera chases a target racing along the path with varying speed:
 * it falls BACK when accelerating and closes in when braking (true
 * perspective, since it's a live raycast), widens FOV with speed, banks
 * into the curves, and clamps off the floor/ceiling (one-ray collision).
 *
 * Enclosed courses are the CHEAP case for a raycaster (rays hit nearby
 * walls fast = short marches); it prints avg steps/ray + fps as a proxy
 * for the on-cart M7 cost. Low-res framebuffer = the perf budget.
 *
 * Build + run on Windows:
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_fly3d \
 *      src/video/ppu.c src/video/present_gl_win32.c src/video/course.c \
 *      src/video/tests/demo_fly3d.c -lopengl32 -lgdi32 -luser32
 *   ./build/demo_fly3d [course.txt]   (no arg = built-in canyon loop)
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#include "video/present.h"
#include "video/course.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
/* no <math.h> (resolves to the repo math aggregator under -Iinclude) */

#define MAPSZ         256
#define MAPMASK       (MAPSZ - 1)
#define SNES_NTSC_HZ  60.0988

#define FBW  64        /* lower res than the open-world demo: enclosed */
#define FBH  56        /* course needs fewer pixels, leaves M7 headroom */

#define RAMP 20
#define ROCK_BASE 1
#define SNOW_BASE (ROCK_BASE + RAMP)
#define LAVA_BASE (SNOW_BASE + RAMP)
#define MAT_ROCK 0
#define MAT_SNOW 1
#define MAT_LAVA 2

#define WALL_H   210           /* solid rock outside the channel */
#define OPEN_CEIL 255          /* >=254 means "no ceiling / open sky" */

#define BGR555(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

static PpuState P;
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
static uint8_t  Fmap[MAPSZ * MAPSZ];   /* floor height   */
static uint8_t  Cmap[MAPSZ * MAPSZ];   /* ceiling height (255 = open) */
static uint8_t  Mmap[MAPSZ * MAPSZ];   /* material       */
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
static V3 vadd(V3 a, V3 b)  { return v3(a.x + b.x, a.y + b.y, a.z + b.z); }
static V3 vsub(V3 a, V3 b)  { return v3(a.x - b.x, a.y - b.y, a.z - b.z); }
static V3 vmul(V3 a, float s){ return v3(a.x * s, a.y * s, a.z * s); }
static V3 vcross(V3 a, V3 b){ return v3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static V3 vnorm(V3 a) { float inv = 1.0f / fsqrt(a.x * a.x + a.y * a.y + a.z * a.z); return vmul(a, inv); }

/* ---- the course: a closed path carved into floor/ceiling --- */

/* Centerline of the racing channel (closed loop in the 256x256 torus). */
/* The active course: loaded from a .course file, else a built-in loop.
 * The maps are baked from it by course_bake() (the same baker the cart
 * uses at level-load). */
static CourseDef g_course;

/* Built-in fallback: a closed canyon loop (canyon / open / tunnel mix). */
static void build_default_course(CourseDef *c) {
    c->loop = true;
    c->count = 8;
    for (int i = 0; i < 8; i++) {
        float a = (float)i / 8.0f * 6.28318531f;
        int open = (i == 0 || i == 4), tunnel = (i == 2 || i == 6);
        c->node[i].x = 128.0f + 80.0f * fcos(a);
        c->node[i].z = 128.0f + 80.0f * fsin(a);
        c->node[i].y = 45.0f + 18.0f * fsin(2.0f * a);
        c->node[i].width = 20.0f;
        c->node[i].wall  = open ? 50.0f : 130.0f;
        c->node[i].ceil  = tunnel ? 30.0f : 0.0f;
        c->node[i].lava  = open ? 0.0f : 5.0f;
    }
}

/* Parse a .course text file: "node x z y width wall ceil lava" lines,
 * an optional "loop 1"; '#' comments and blank lines ignored. */
static bool load_course(const char *path, CourseDef *c) {
    FILE *fp = fopen(path, "r");
    if (!fp) return false;
    c->count = 0; c->loop = false;
    char line[256];
    while (fgets(line, sizeof line, fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        if (strncmp(p, "loop", 4) == 0) {
            int v = 0; (void)sscanf(p + 4, "%d", &v); c->loop = (v != 0); continue;
        }
        if (strncmp(p, "node", 4) == 0 && c->count < COURSE_MAX_NODES) {
            CourseNode n;
            if (sscanf(p + 4, "%f %f %f %f %f %f %f",
                       &n.x, &n.z, &n.y, &n.width, &n.wall, &n.ceil, &n.lava) == 7)
                c->node[c->count++] = n;
        }
    }
    fclose(fp);
    return c->count >= 2;
}

/* ---- palette: depth-shaded material ramps ------------------ */

static uint16_t lerp555(uint16_t a, uint16_t b, float t) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    return BGR555(ar + (int)((br - ar) * t), ag + (int)((bg - ag) * t), ab + (int)((bb - ab) * t));
}
static void build_palette(void) {
    const uint16_t sky = BGR555(29, 23, 31);        /* bright daytime lavender */
    P.cgram[0] = sky;
    struct { uint16_t nr, fr; } m[3] = {
        { BGR555(20, 18, 16), lerp555(BGR555(7, 6, 7),    sky, 0.5f) },  /* rock */
        { BGR555(31, 31, 31), lerp555(BGR555(15, 18, 24), sky, 0.5f) },  /* snow */
        { BGR555(31, 26, 6),  lerp555(BGR555(20, 4, 2),   sky, 0.3f) },  /* lava */
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

/* ---- raycaster over floor + ceiling ------------------------ */

static long render_fb(V3 cam, V3 fwd, V3 right, V3 up, float fov) {
    const float MAXT = 220.0f, DTNEAR = 1.0f, DTK = 0.02f;
    const int   MAXSTEPS = 256;
    float halfh = fsin(fov * 0.5f) / fcos(fov * 0.5f);
    float halfw = halfh * ((float)PPU_SCREEN_W / (float)PPU_SCREEN_H);
    long steps = 0;

    for (int py = 0; py < FBH; py++) {
        float ny = (1.0f - ((float)py + 0.5f) / (float)FBH * 2.0f) * halfh;
        for (int px = 0; px < FBW; px++) {
            float nx = (((float)px + 0.5f) / (float)FBW * 2.0f - 1.0f) * halfw;
            V3 d = vadd(fwd, vadd(vmul(right, nx), vmul(up, ny)));
            d = vnorm(d);

            uint8_t out = 0;                         /* sky */
            float t = 1.0f, prev = 1.0f;
            for (int s = 0; s < MAXSTEPS; s++) {
                steps++;
                float wx = cam.x + d.x * t, wy = cam.y + d.y * t, wz = cam.z + d.z * t;
                unsigned cell = ((unsigned)((int)ffloor(wz) & MAPMASK)) * MAPSZ
                              +  (unsigned)((int)ffloor(wx) & MAPMASK);
                float fl = (float)Fmap[cell], cl = (float)Cmap[cell];
                int hit_mat = -1;
                if (wy <= fl) {                      /* hit floor: refine */
                    float lo = prev, hi = t;
                    for (int it = 0; it < 3; it++) {
                        float mid = (lo + hi) * 0.5f;
                        unsigned mc = ((unsigned)((int)ffloor(cam.z + d.z * mid) & MAPMASK)) * MAPSZ
                                    +  (unsigned)((int)ffloor(cam.x + d.x * mid) & MAPMASK);
                        if (cam.y + d.y * mid <= (float)Fmap[mc]) hi = mid; else lo = mid;
                    }
                    t = hi; cell = ((unsigned)((int)ffloor(cam.z + d.z * t) & MAPMASK)) * MAPSZ
                                 +  (unsigned)((int)ffloor(cam.x + d.x * t) & MAPMASK);
                    hit_mat = Mmap[cell];
                } else if (cl < 254.0f && wy >= cl) { /* hit ceiling (tunnels) */
                    hit_mat = MAT_ROCK;
                } else if (d.y >= 0.0f && wy > 255.0f) {
                    break;                            /* escaped upward -> sky */
                }
                if (hit_mat >= 0) {
                    float bright = 1.0f - t / MAXT;
                    if (bright < 0.0f) bright = 0.0f;
                    out = shade_index(hit_mat, bright);
                    break;
                }
                prev = t;
                t += DTNEAR + t * DTK;
                if (t > MAXT) break;
            }
            fbuf[py * FBW + px] = out;
        }
    }
    return steps;
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
    P.m7a = (int16_t)FBW;
    P.m7d = (int16_t)(256 * FBH / PPU_SCREEN_H);
    P.m7b = 0; P.m7c = 0; P.m7x = 0; P.m7y = 0; P.m7hofs = 0; P.m7vofs = 0;
}

/* Channel fly-height: a bit above the course floor at param s. */
static V3 ride(float s) {
    CourseNode n;
    course_sample(&g_course, s, &n);
    return v3(n.x, n.y + 16.0f, n.z);
}

int main(int argc, char **argv) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - course flythrough")) return 1;
    printf("GL renderer : %s\n", present_gl_renderer());

    ppu_state_clear(&P);
    P.mode = 7;
    build_palette();

    if (argc > 1 && load_course(argv[1], &g_course))
        printf("course: %s (%d nodes, %s)\n", argv[1], g_course.count, g_course.loop ? "loop" : "linear");
    else {
        build_default_course(&g_course);
        printf("course: built-in canyon loop (pass a .course file as arg 1 to load one)\n");
    }
    course_bake(&g_course, Fmap, Cmap, Mmap, MAPSZ);
    float len = course_length(&g_course);
    printf("%dx%d raycast -> Mode 7 stretch; chase cam, speed-distance + banking.\n", FBW, FBH);
    fflush(stdout);

    float kth = 0.0f;                                 /* Kestrel's path param */
    V3    cam = ride(0.0f); cam.y += 10.0f;
    float roll = 0.0f;
    const float LAG = 0.08f;

    const double target_dt = 1.0 / SNES_NTSC_HZ;
    double last = now_sec(), acc = 0.0, report = last;
    unsigned f = 0, frames = 0;
    long step_sum = 0;

    while (!present_should_close()) {
        double tnow = now_sec();
        acc += tnow - last; last = tnow;
        if (acc > 0.25) acc = 0.25;
        bool stepped = false;
        float speed = 0.0f;
        while (acc >= target_dt) {
            /* speed varies: accelerate/brake along the run */
            speed = 0.020f + 0.012f * fsin((float)f * 0.012f);
            kth += speed;
            if (kth >= len) kth -= len;              /* loop the preview */
            acc -= target_dt; stepped = true; f++;
        }
        if (stepped) {
            V3 kpos = ride(kth);
            /* chase target trails Kestrel; farther back when faster */
            float back = 0.9f + speed * 22.0f;
            V3 ctar = ride(kth - back * 0.06f);
            ctar.y += 9.0f;
            cam = vadd(cam, vmul(vsub(ctar, cam), LAG));     /* damped follow */

            /* one-ray collision clamp: stay off floor/ceiling at the camera cell */
            unsigned cc = ((unsigned)((int)ffloor(cam.z) & MAPMASK)) * MAPSZ
                        +  (unsigned)((int)ffloor(cam.x) & MAPMASK);
            if (cam.y < (float)Fmap[cc] + 6.0f) cam.y = (float)Fmap[cc] + 6.0f;
            if (Cmap[cc] < 254 && cam.y > (float)Cmap[cc] - 4.0f) cam.y = (float)Cmap[cc] - 4.0f;

            /* look ahead along the path; bank into the curve */
            V3 look = ride(kth + 0.10f);
            V3 fwd = vnorm(vsub(look, cam));
            V3 right0 = vnorm(vcross(fwd, v3(0.0f, 1.0f, 0.0f)));
            V3 up0 = vcross(right0, fwd);
            V3 ta = vsub(ride(kth + 0.05f), kpos);
            V3 tb = vsub(ride(kth + 0.10f), ride(kth + 0.05f));
            float turn = ta.x * tb.z - ta.z * tb.x;          /* signed curvature */
            float bank = turn * 0.06f;
            if (bank >  0.8f) bank = 0.8f;
            if (bank < -0.8f) bank = -0.8f;
            roll += (bank - roll) * 0.1f;                    /* smooth the bank */
            float cr = fcos(roll), sr = fsin(roll);
            V3 right = vadd(vmul(right0, cr), vmul(up0, sr));
            V3 up    = vsub(vmul(up0, cr), vmul(right0, sr));

            float fov = 1.05f + speed * 6.0f;                /* widen FOV with speed */
            step_sum += render_fb(cam, fwd, right, up, fov);
            frames++;
            load_mode7();
            ppu_render(&P, FB);
        }
        present_frame(FB);

        if (tnow - report >= 1.0 && frames > 0) {
            printf("  %.1f render-fps  |  ~%ld steps/ray  (%dx%d rays)\n",
                   (double)frames / (tnow - report),
                   step_sum / ((long)frames * FBW * FBH), FBW, FBH);
            fflush(stdout);
            report = tnow; frames = 0; step_sum = 0;
        }
    }

    present_shutdown();
    return 0;
}
