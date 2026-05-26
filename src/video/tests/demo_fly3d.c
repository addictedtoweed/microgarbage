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
#include "video/hfcast.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
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
static CourseArc g_arc;        /* distance<->param table, built once at load */

/* Built-in fallback: a gentle, near-level lava-canyon descent. The floor
 * stays roughly level relative to the rim (COURSE_WALL_H) so the camera —
 * which flies just above the rim — keeps the violet sky in view even
 * through the bends. (A truly DESCENDING canyon needs the rim to descend
 * with the floor: a baker enhancement, TODO.) */
static void build_default_course(CourseDef *c) {
    c->loop = false;
    c->count = 7;
    for (int i = 0; i < 7; i++) {
        float f = (float)i / 6.0f;
        c->node[i].x = 36.0f + 184.0f * f;
        c->node[i].z = 128.0f + 26.0f * fsin(f * 3.0f);   /* gentle bends */
        c->node[i].y = 40.0f + 6.0f * fsin(f * 5.0f);     /* ~level vs the rim */
        c->node[i].width = 34.0f;
        c->node[i].wall  = 28.0f;                          /* edge ~68 ~ rim 70 */
        c->node[i].ceil  = 0.0f;                            /* open (no tunnel for now) */
        c->node[i].lava  = 7.0f;
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
    /* Each ramp spans dark (shadow / far) -> lit (front-lit / near) with
     * REAL contrast, so the directional light + fog are actually visible
     * (the old ramp's endpoints were nearly identical -> everything flat).
     * Shadows tinted toward the violet sky rather than pure black. */
    struct { uint16_t lit, dark; } m[3] = {
        { BGR555(26, 23, 19), BGR555(4, 3, 8)   },   /* rock: warm lit -> dark violet shadow */
        { BGR555(31, 31, 31), BGR555(10, 12, 19) },  /* snow: white    -> cool blue shadow   */
        { BGR555(31, 29, 9),  BGR555(14, 2, 1)   },  /* lava: hot       -> dark ember         */
    };
    int base[3] = { ROCK_BASE, SNOW_BASE, LAVA_BASE };
    for (int mat = 0; mat < 3; mat++)
        for (int s = 0; s < RAMP; s++) {
            float t = (float)s / (float)(RAMP - 1);
            P.cgram[base[mat] + s] = lerp555(m[mat].dark, m[mat].lit, t);  /* s=0 dark .. s=max lit */
        }
}
/* ---- raycaster: the portable fixed-point hfcast module does the
 * marching; render_fb just adapts the float prototype camera to Q16.16
 * and hands off the scene + camera. ---- */

static long render_fb(V3 cam, V3 fwd, V3 right, V3 up, float fov) {
    float halfh = fsin(fov * 0.5f) / fcos(fov * 0.5f);          /* tan(fov/2) */
    float halfw = halfh * ((float)PPU_SCREEN_W / (float)PPU_SCREEN_H);

    HfScene sc;
    sc.floor = Fmap; sc.ceiling = Cmap; sc.material = Mmap; sc.mapsz = MAPSZ;
    sc.light = vec3_q16_normalize(vec3_q16_make(q16_from_float(0.53f),
                                                q16_from_float(0.74f),
                                                q16_from_float(0.42f)));
    sc.fog_range = q16_from_int(130);
    sc.rock_base = ROCK_BASE; sc.snow_base = SNOW_BASE; sc.lava_base = LAVA_BASE;
    sc.ramp = RAMP;
    sc.rock_mat = MAT_ROCK; sc.snow_mat = MAT_SNOW; sc.lava_mat = MAT_LAVA;
    sc.sky_h = q16_from_int(90);     /* terrain tops out ~80; above it (going up) is sky */

    HfCamera hc;
    hc.pos   = vec3_q16_make(q16_from_float(cam.x),   q16_from_float(cam.y),   q16_from_float(cam.z));
    hc.right = vec3_q16_make(q16_from_float(right.x), q16_from_float(right.y), q16_from_float(right.z));
    hc.up    = vec3_q16_make(q16_from_float(up.x),    q16_from_float(up.y),    q16_from_float(up.z));
    hc.fwd   = vec3_q16_make(q16_from_float(fwd.x),   q16_from_float(fwd.y),   q16_from_float(fwd.z));
    hc.halfw = q16_from_float(halfw);
    hc.halfh = q16_from_float(halfh);

    return hfcast_render(&sc, &hc, fbuf, FBW, FBH);
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

/* Ride by DISTANCE along the path (arc-length), wrapping the loop, so the
 * camera advances at an even pace regardless of node spacing. */
static V3 ride_d(float d) {
    float total = g_arc.total;
    if (total > 0.0f) {
        while (d >= total) d -= total;
        while (d < 0.0f)   d += total;
    }
    return ride(course_param_at_distance(&g_arc, d));
}

int main(int argc, char **argv) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - course flythrough")) return 1;
    printf("GL renderer : %s\n", present_gl_renderer());

    ppu_state_clear(&P);
    P.mode = 7;
    build_palette();

    float SONG_SEC = 12.0f;          /* one lap = this many seconds (streaming overrides) */
    const float GEN_VEL  = 34.0f;    /* tuned cruise velocity for generated courses */
    bool  streaming = false;
    float baked_to  = 0.0f;
    const float STREAM_LEAD = 200.0f; /* bake this far ahead of the camera (must be < MAPSZ) */

    if (argc > 1 && strncmp(argv[1], "stream", 6) == 0) {
        uint32_t seed = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 1234u;
        course_generate_long(seed, GEN_VEL * 60.0f, (float)MAPSZ, &g_course);
        streaming = true;
        SONG_SEC = 60.0f;            /* ~1 minute run, one pass before it wraps */
        printf("course: streamed long ribbon seed=%u, %d nodes (~60s, sliding window)\n",
               seed, g_course.count);
    } else if (argc > 1 && strncmp(argv[1], "gen", 3) == 0) {
        uint32_t seed = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 1234u;
        course_generate(seed, SONG_SEC, GEN_VEL, (float)MAPSZ, &g_course);
        printf("course: generated seed=%u, %d nodes (~%.0fs @ vel %.0f)\n",
               seed, g_course.count, SONG_SEC, GEN_VEL);
    } else if (argc > 1 && load_course(argv[1], &g_course)) {
        printf("course: %s (%d nodes, %s)\n", argv[1], g_course.count, g_course.loop ? "loop" : "linear");
    } else {
        build_default_course(&g_course);
        printf("course: built-in canyon (arg: a .course file, 'gen [seed]', or 'stream [seed]')\n");
    }

    course_build_arc(&g_course, &g_arc);
    if (streaming) {
        baked_to = ride_d(0.0f).x + STREAM_LEAD;   /* prime the window at the start */
        course_bake_strip(&g_course, Fmap, Cmap, Mmap, MAPSZ, 0, (int)baked_to);
    } else {
        course_bake(&g_course, Fmap, Cmap, Mmap, MAPSZ);
    }
    float total = g_arc.total;
    printf("course length: %.0f units; %dx%d raycast -> Mode 7 stretch; %.0fs run.\n",
           total, FBW, FBH, SONG_SEC);
    fflush(stdout);

    float dist = 0.0f;                                /* Kestrel's distance along the path */
    V3    cam = ride_d(0.0f); cam.y += 10.0f;
    float roll = 0.0f;
    const float LAG = 0.11f;        /* track tight enough not to cut into walls */

    /* Song-locked baseline + leashed player offset (the rubber-band).
     * The clock here is a stand-in (emulated-frame time); on the cart it
     * becomes the audio sample position, so the run ends exactly on the
     * song's landmark. The offset is a SIMULATED brake/accelerate (no
     * controller in the demo), in DISTANCE units, clamped to a leash and
     * sprung back to the baseline so it can never accumulate drift. */
    const float LEASH = 26.0f;      /* surge/brake authority (map units) */
    float offset = 0.0f, prev_off = 0.0f;
    int   last_lap = -1;

    const double target_dt = 1.0 / SNES_NTSC_HZ;
    double last = now_sec(), acc = 0.0, report = last;
    unsigned f = 0, frames = 0;
    long step_sum = 0;

    while (!present_should_close()) {
        double tnow = now_sec();
        acc += tnow - last; last = tnow;
        if (acc > 0.25) acc = 0.25;
        bool stepped = false;
        bool wrapped = false;        /* the run looped back to the start this frame */
        while (acc >= target_dt) {
            f++;
            float song_t = (float)f / (float)SNES_NTSC_HZ;       /* stand-in clock (s) */
            float prog   = song_t / SONG_SEC;
            int   lap    = (int)ffloor(prog);
            float baseline = (prog - (float)lap) * total;        /* clock-locked DISTANCE */

            float input = fsin(song_t * 1.7f);                   /* simulated brake/accel */
            offset += input * 0.35f;
            offset -= offset * 0.05f;                            /* spring back to baseline */
            if (offset >  LEASH) offset =  LEASH;
            if (offset < -LEASH) offset = -LEASH;

            dist = baseline + offset;                            /* leashed around the clock */
            if (total > 0.0f) {
                while (dist >= total) dist -= total;
                while (dist < 0.0f)   dist += total;
            }

            if (lap != last_lap) {                               /* a lap landed on the clock */
                if (last_lap >= 0) {
                    printf("  course end #%d at %.2fs (clock-locked every %.1fs)\n",
                           lap, song_t, SONG_SEC);
                    fflush(stdout);
                    wrapped = true;
                }
                last_lap = lap;
            }
            acc -= target_dt; stepped = true;
        }
        if (stepped) {
            V3 kpos = ride_d(dist);
            /* felt speed (units/sec): clock cruise + the leash surge */
            float cruise = (total > 0.0f) ? total / SONG_SEC : 0.0f;
            float surge  = (offset - prev_off) * (float)SNES_NTSC_HZ;
            float vfeel  = cruise + surge;
            if (vfeel < 0.0f) vfeel = 0.0f;
            prev_off = offset;

            /* chase target trails Kestrel; farther back when surging */
            float back = 3.0f + vfeel * 0.045f;
            V3 ctar = ride_d(dist - back);
            ctar.y += 18.0f;                                 /* ~floor+34: just above the rim */

            if (streaming) {                                 /* scroll the map window */
                if (wrapped) baked_to = 0.0f;                /* re-establish at the start */
                if (ctar.x + STREAM_LEAD > baked_to) {
                    course_bake_strip(&g_course, Fmap, Cmap, Mmap, MAPSZ,
                                      (int)baked_to, (int)(ctar.x + STREAM_LEAD));
                    baked_to = ctar.x + STREAM_LEAD;
                }
            }

            /* HUG the path centre laterally (so the camera can never drift
             * into a wall) and only smooth the HEIGHT. */
            cam.x = ctar.x;
            cam.z = ctar.z;
            cam.y += (ctar.y - cam.y) * LAG;
            if (wrapped) { cam.y = ctar.y; roll = 0.0f; }    /* snap across the wrap seam */

            /* clamp off the floor/lava and below any tunnel ceiling at the
             * camera cell (which is on the centre path now). */
            unsigned cc = ((unsigned)((int)ffloor(cam.z) & MAPMASK)) * MAPSZ
                        +  (unsigned)((int)ffloor(cam.x) & MAPMASK);
            if (cam.y < (float)Fmap[cc] + 14.0f) cam.y = (float)Fmap[cc] + 14.0f;
            if (Cmap[cc] < 254 && cam.y > (float)Cmap[cc] - 8.0f) cam.y = (float)Cmap[cc] - 8.0f;

            /* look well AHEAD and gently down INTO the canyon, so the violet
             * sky fills the top and the lava channel recedes below. */
            V3 look = ride_d(dist + 26.0f);
            look.y += 6.0f;                                  /* ~floor+22 ahead vs cam ~floor+34 */
            V3 fwd = vnorm(vsub(look, cam));
            V3 right0 = vnorm(vcross(fwd, v3(0.0f, 1.0f, 0.0f)));
            V3 up0 = vcross(right0, fwd);
            V3 ta = vsub(ride_d(dist + 4.0f), kpos);
            V3 tb = vsub(ride_d(dist + 8.0f), ride_d(dist + 4.0f));
            float turn = ta.x * tb.z - ta.z * tb.x;          /* signed curvature */
            float bank = turn * 0.06f;
            if (bank >  0.8f) bank = 0.8f;
            if (bank < -0.8f) bank = -0.8f;
            roll += (bank - roll) * 0.1f;                    /* smooth the bank */
            float cr = fcos(roll), sr = fsin(roll);
            V3 right = vadd(vmul(right0, cr), vmul(up0, sr));
            V3 up    = vsub(vmul(up0, cr), vmul(right0, sr));

            float fov = 1.05f + vfeel * 0.0045f;             /* widen FOV when surging */
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
