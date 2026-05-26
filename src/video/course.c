/* ============================================================
 *  course.c — spline sampling + map baking for flight courses.
 *  See include/video/course.h. Portable: no stdio, no libm
 *  (baking uses squared distance; arc-length + the generator use
 *  the libm-free fsqrt_/fsin_/fcos_ below — no <math.h>).
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "video/course.h"

/* ---- libm-free float helpers (this module avoids <math.h>) ---- */
#define COURSE_PI 3.14159265358979f

static float fsqrt_(float v) {
    if (v <= 0.0f) return 0.0f;
    float g = v;                          /* Newton's method, plenty of iters */
    for (int i = 0; i < 12; i++) g = 0.5f * (g + v / g);
    return g;
}
static float fsin_(float x) {
    while (x >  COURSE_PI) x -= 2.0f * COURSE_PI;
    while (x < -COURSE_PI) x += 2.0f * COURSE_PI;
    const float B = 4.0f / COURSE_PI, C = -4.0f / (COURSE_PI * COURSE_PI);
    float ax = (x < 0.0f) ? -x : x;
    float y = B * x + C * x * ax;
    float ay = (y < 0.0f) ? -y : y;
    return 0.225f * (y * ay - y) + y;     /* one refinement pass */
}
static float fcos_(float x) { return fsin_(x + COURSE_PI * 0.5f); }

float course_length(const CourseDef *c) {
    if (!c || c->count <= 0) return 0.0f;
    return c->loop ? (float)c->count : (float)(c->count - 1);
}

/* Catmull-Rom through p1,p2 using neighbors p0,p3, at local t in [0,1]. */
static float cr(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t, t3 = t2 * t;
    return 0.5f * ((2.0f * p1)
                 + (-p0 + p2) * t
                 + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2
                 + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

void course_sample(const CourseDef *c, float s, CourseNode *out) {
    int n = c->count;
    if (n <= 0) { CourseNode z = {0,0,0,0,0,0,0}; *out = z; return; }
    if (n == 1) { *out = c->node[0]; return; }

    int i; float t;
    int i0, i1, i2, i3;
    if (c->loop) {
        float len = (float)n;
        while (s < 0.0f) s += len;
        while (s >= len) s -= len;
        i = (int)s; t = s - (float)i;
        i0 = (i - 1 + n) % n; i1 = i % n; i2 = (i + 1) % n; i3 = (i + 2) % n;
    } else {
        if (s < 0.0f) s = 0.0f;
        if (s > (float)(n - 1)) s = (float)(n - 1);
        i = (int)s; if (i > n - 2) i = n - 2; t = s - (float)i;
        i0 = (i - 1 < 0) ? 0 : i - 1;
        i1 = i; i2 = i + 1;
        i3 = (i + 2 > n - 1) ? n - 1 : i + 2;
    }
    const CourseNode *a = &c->node[i0], *b = &c->node[i1], *d = &c->node[i2], *e = &c->node[i3];
    out->x     = cr(a->x,     b->x,     d->x,     e->x,     t);
    out->z     = cr(a->z,     b->z,     d->z,     e->z,     t);
    out->y     = cr(a->y,     b->y,     d->y,     e->y,     t);
    out->width = cr(a->width, b->width, d->width, e->width, t);
    out->wall  = cr(a->wall,  b->wall,  d->wall,  e->wall,  t);
    out->ceil  = cr(a->ceil,  b->ceil,  d->ceil,  e->ceil,  t);
    out->lava  = cr(a->lava,  b->lava,  d->lava,  e->lava,  t);
}

static uint8_t clamp_u8(float v) {
    if (v < 0.0f) return 0;
    if (v > 255.0f) return 255;
    return (uint8_t)v;
}

void course_bake(const CourseDef *c, uint8_t *floor, uint8_t *ceiling,
                 uint8_t *material, int mapsz) {
    int total = mapsz * mapsz;
    unsigned mask = (unsigned)(mapsz - 1);
    for (int i = 0; i < total; i++) {
        floor[i]    = (uint8_t)COURSE_WALL_H;
        ceiling[i]  = (uint8_t)COURSE_OPEN_CEIL;
        material[i] = COURSE_MAT_ROCK;
    }
    if (!c || c->count <= 0) return;

    float len = course_length(c);
    /* Dense walk so the stamped corridor never gaps (one-time cost). */
    int nsamp = mapsz * 16;
    for (int k = 0; k <= nsamp; k++) {
        float s = (nsamp > 0) ? len * (float)k / (float)nsamp : 0.0f;
        CourseNode nd;
        course_sample(c, s, &nd);

        int rad = (int)nd.width;
        if (rad < 1) rad = 1;
        float w2 = nd.width * nd.width;
        float lava2 = nd.lava * nd.lava;
        int cx0 = (int)nd.x, cz0 = (int)nd.z;
        for (int dz = -rad; dz <= rad; dz++) {
            for (int dx = -rad; dx <= rad; dx++) {
                float d2 = (float)(dx * dx + dz * dz);
                if (d2 > w2) continue;
                unsigned cell = (((unsigned)(cz0 + dz) & mask) * (unsigned)mapsz)
                              +  ((unsigned)(cx0 + dx) & mask);
                float u2 = (w2 > 0.0f) ? d2 / w2 : 0.0f;          /* 0 centre .. 1 edge */
                uint8_t fl = clamp_u8(nd.y + u2 * nd.wall);        /* floor rises to walls */
                if (fl < floor[cell]) {
                    floor[cell] = fl;
                    material[cell] = (nd.lava > 0.0f && d2 < lava2) ? COURSE_MAT_LAVA
                                   : (fl > 170)                     ? COURSE_MAT_SNOW
                                   :                                  COURSE_MAT_ROCK;
                }
                if (nd.ceil > 0.0f) {
                    uint8_t cl = clamp_u8(nd.y + nd.ceil);
                    if (cl < ceiling[cell]) ceiling[cell] = cl;
                }
            }
        }
    }
}

/* ---- arc-length -------------------------------------------- */

float course_total_distance(const CourseDef *c) {
    if (!c || c->count <= 0) return 0.0f;
    float plen = course_length(c);
    int N = COURSE_ARC_SAMPLES;
    CourseNode p0; course_sample(c, 0.0f, &p0);
    float tot = 0.0f;
    for (int i = 1; i <= N; i++) {
        CourseNode p;
        course_sample(c, plen * (float)i / (float)N, &p);
        float dx = p.x - p0.x, dy = p.y - p0.y, dz = p.z - p0.z;
        tot += fsqrt_(dx*dx + dy*dy + dz*dz);
        p0 = p;
    }
    return tot;
}

void course_build_arc(const CourseDef *c, CourseArc *a) {
    int N = COURSE_ARC_SAMPLES;
    a->plen = course_length(c);
    a->cum[0] = 0.0f;
    if (!c || c->count <= 0) {
        for (int i = 1; i <= N; i++) a->cum[i] = 0.0f;
        a->total = 0.0f;
        return;
    }
    CourseNode p0; course_sample(c, 0.0f, &p0);
    for (int i = 1; i <= N; i++) {
        CourseNode p;
        course_sample(c, a->plen * (float)i / (float)N, &p);
        float dx = p.x - p0.x, dy = p.y - p0.y, dz = p.z - p0.z;
        a->cum[i] = a->cum[i-1] + fsqrt_(dx*dx + dy*dy + dz*dz);
        p0 = p;
    }
    a->total = a->cum[N];
}

float course_param_at_distance(const CourseArc *a, float dist) {
    int N = COURSE_ARC_SAMPLES;
    if (dist <= 0.0f)      return 0.0f;
    if (dist >= a->total)  return a->plen;
    int lo = 0, hi = N;                       /* binary search the cumulative table */
    while (hi - lo > 1) {
        int mid = (lo + hi) >> 1;
        if (a->cum[mid] <= dist) lo = mid; else hi = mid;
    }
    float seg = a->cum[hi] - a->cum[lo];
    float t = (seg > 0.0f) ? (dist - a->cum[lo]) / seg : 0.0f;
    return ((float)lo + t) / (float)N * a->plen;
}

/* ---- procedural generation --------------------------------- */

void course_generate(uint32_t seed, float duration_sec, float velocity,
                     float mapsz, CourseDef *out) {
    const float spacing = 24.0f;               /* ~distance between control nodes */
    float target = velocity * duration_sec;    /* desired total arc-length        */
    int count = (int)(target / spacing) + 2;
    if (count < 4) count = 4;
    if (count > COURSE_MAX_NODES) count = COURSE_MAX_NODES;
    out->count = count;
    out->loop  = false;

    uint32_t rng = seed ? seed : 0x9E3779B9u;
    float margin = 30.0f, lo = margin, hi = mapsz - margin;
    float x = mapsz * 0.5f, z = margin + 6.0f; /* start near one edge */
    float heading = 1.30f;                     /* radians, angled into the map */

    for (int i = 0; i < count; i++) {
        rng = rng*1664525u + 1013904223u; float r1 = (float)((rng >> 8) & 0xFFFF) / 65535.0f;
        rng = rng*1664525u + 1013904223u; float r2 = (float)((rng >> 8) & 0xFFFF) / 65535.0f;
        rng = rng*1664525u + 1013904223u; float r3 = (float)((rng >> 8) & 0xFFFF) / 65535.0f;
        int open = (r1 < 0.25f);               /* open stretch, no lava */

        /* Floor stays ~level vs the rim so the camera (just above the rim)
         * keeps the sky in view through bends. A true descent needs the
         * rim to descend with the floor — a baker enhancement (TODO). */
        out->node[i].x     = x;
        out->node[i].z     = z;
        out->node[i].y     = 40.0f + r2 * 3.0f;                 /* ~level vs the rim */
        out->node[i].width = 30.0f + r3 * 12.0f;                /* corridor 30..42 */
        out->node[i].wall  = open ? (18.0f + r3 * 8.0f) : (24.0f + r3 * 8.0f);  /* edge ~ rim 70 */
        out->node[i].ceil  = 0.0f;                              /* open (tunnels: TODO) */
        out->node[i].lava  = open ? 0.0f : (4.0f + r2 * 5.0f);

        rng = rng*1664525u + 1013904223u;
        float turn = ((float)((rng >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 0.38f;
        heading += turn;
        x += fcos_(heading) * spacing;
        z += fsin_(heading) * spacing;
        if (x < lo) { x = lo; heading = COURSE_PI - heading; }  /* reflect, stay in-bounds */
        if (x > hi) { x = hi; heading = COURSE_PI - heading; }
        if (z < lo) { z = lo; heading = -heading; }
        if (z > hi) { z = hi; heading = -heading; }
    }
}

/* ---- streaming: long ribbon + strip bake ------------------- */

void course_generate_long(uint32_t seed, float length_x, float mapsz, CourseDef *out) {
    const float spacing = 24.0f;
    int count = (int)(length_x / spacing) + 2;
    if (count < 4) count = 4;
    if (count > COURSE_MAX_NODES) count = COURSE_MAX_NODES;
    out->count = count;
    out->loop  = false;

    uint32_t rng = seed ? seed : 0x9E3779B9u;
    float zlo = 48.0f, zhi = mapsz - 48.0f;
    float z = mapsz * 0.5f, zvel = 0.0f;

    for (int i = 0; i < count; i++) {
        rng = rng*1664525u + 1013904223u; float r1 = (float)((rng >> 8) & 0xFFFF) / 65535.0f;
        rng = rng*1664525u + 1013904223u; float r2 = (float)((rng >> 8) & 0xFFFF) / 65535.0f;
        rng = rng*1664525u + 1013904223u; float r3 = (float)((rng >> 8) & 0xFFFF) / 65535.0f;
        int open = (r1 < 0.25f);

        out->node[i].x     = 24.0f + spacing * (float)i;       /* monotonic flight axis */
        out->node[i].z     = z;
        out->node[i].y     = 40.0f + r2 * 3.0f;                /* ~level vs the rim */
        out->node[i].width = 30.0f + r3 * 12.0f;
        out->node[i].wall  = open ? (18.0f + r3 * 8.0f) : (24.0f + r3 * 8.0f);
        out->node[i].ceil  = 0.0f;
        out->node[i].lava  = open ? 0.0f : (4.0f + r2 * 5.0f);

        rng = rng*1664525u + 1013904223u;
        zvel += ((float)((rng >> 8) & 0xFFFF) / 65535.0f - 0.5f) * 1.5f;   /* very faint steer */
        zvel += (mapsz * 0.5f - z) * 0.06f;            /* strong pull to centre: ~straight */
        if (zvel >  6.0f) zvel =  6.0f;
        if (zvel < -6.0f) zvel = -6.0f;
        z += zvel;
        if (z < zlo) z = zlo;                          /* clamp position only — NO velocity */
        if (z > zhi) z = zhi;                          /* flip, so the path never kinks      */
    }
}

void course_bake_strip(const CourseDef *c, uint8_t *floor, uint8_t *ceiling,
                       uint8_t *material, int mapsz, int x_lo, int x_hi) {
    unsigned mask = (unsigned)(mapsz - 1);

    /* clear the strip's columns (toroidal in x) to rim / open / rock */
    for (int xc = x_lo; xc < x_hi; xc++) {
        unsigned xm = (unsigned)xc & mask;
        for (int z = 0; z < mapsz; z++) {
            unsigned idx = (unsigned)z * (unsigned)mapsz + xm;
            floor[idx]    = (uint8_t)COURSE_WALL_H;
            ceiling[idx]  = (uint8_t)COURSE_OPEN_CEIL;
            material[idx] = COURSE_MAT_ROCK;
        }
    }
    if (!c || c->count <= 0) return;

    float len = course_length(c);
    int nsamp = c->count * 64;                 /* fine enough the corridor never gaps */
    const float WMAX = 52.0f;                  /* widest corridor reach (sample gate) */
    for (int k = 0; k <= nsamp; k++) {
        float s = (nsamp > 0) ? len * (float)k / (float)nsamp : 0.0f;
        CourseNode nd;
        course_sample(c, s, &nd);
        if (nd.x < (float)x_lo - WMAX || nd.x > (float)x_hi + WMAX) continue;  /* not in strip */

        int rad = (int)nd.width;
        if (rad < 1) rad = 1;
        float w2 = nd.width * nd.width;
        float lava2 = nd.lava * nd.lava;
        int cx0 = (int)nd.x, cz0 = (int)nd.z;
        for (int dz = -rad; dz <= rad; dz++) {
            for (int dx = -rad; dx <= rad; dx++) {
                float d2 = (float)(dx * dx + dz * dz);
                if (d2 > w2) continue;
                unsigned cell = (((unsigned)(cz0 + dz) & mask) * (unsigned)mapsz)
                              +  ((unsigned)(cx0 + dx) & mask);
                float u2 = (w2 > 0.0f) ? d2 / w2 : 0.0f;
                uint8_t fl = clamp_u8(nd.y + u2 * nd.wall);
                if (fl < floor[cell]) {
                    floor[cell] = fl;
                    material[cell] = (nd.lava > 0.0f && d2 < lava2) ? COURSE_MAT_LAVA
                                   : (fl > 170)                     ? COURSE_MAT_SNOW
                                   :                                  COURSE_MAT_ROCK;
                }
                if (nd.ceil > 0.0f) {
                    uint8_t cl = clamp_u8(nd.y + nd.ceil);
                    if (cl < ceiling[cell]) ceiling[cell] = cl;
                }
            }
        }
    }
}

/* Stamp one node's corridor into the maps (toroidal in both axes). */
static void stamp_corridor(uint8_t *floor, uint8_t *ceiling, uint8_t *material,
                           int mapsz, unsigned mask, const CourseNode *nd) {
    int rad = (int)nd->width;
    if (rad < 1) rad = 1;
    float w2 = nd->width * nd->width;
    float lava2 = nd->lava * nd->lava;
    int cx0 = (int)nd->x, cz0 = (int)nd->z;
    for (int dz = -rad; dz <= rad; dz++) {
        for (int dx = -rad; dx <= rad; dx++) {
            float d2 = (float)(dx * dx + dz * dz);
            if (d2 > w2) continue;
            unsigned cell = (((unsigned)(cz0 + dz) & mask) * (unsigned)mapsz)
                          +  ((unsigned)(cx0 + dx) & mask);
            float u2 = (w2 > 0.0f) ? d2 / w2 : 0.0f;
            uint8_t fl = clamp_u8(nd->y + u2 * nd->wall);
            if (fl < floor[cell]) {
                floor[cell] = fl;
                material[cell] = (nd->lava > 0.0f && d2 < lava2) ? COURSE_MAT_LAVA
                               : (fl > 170)                      ? COURSE_MAT_SNOW
                               :                                   COURSE_MAT_ROCK;
            }
            if (nd->ceil > 0.0f) {
                uint8_t cl = clamp_u8(nd->y + nd->ceil);
                if (cl < ceiling[cell]) ceiling[cell] = cl;
            }
        }
    }
}

void course_eval_long(uint32_t seed, float x, CourseNode *out) {
    /* seed -> phase offsets so different seeds give different runs */
    float p1 = (float)(seed         & 0xFF) * 0.0246f;
    float p2 = (float)((seed >> 8)  & 0xFF) * 0.0246f;
    float p3 = (float)((seed >> 16) & 0xFF) * 0.0246f;

    out->x     = x;
    out->z     = 128.0f;                                   /* dead straight — a half-pipe to fly down */
    out->y     = 72.0f  + 12.0f * fsin_(x * 0.0016f + p3); /* gentle roll (big swings creep the horizon) */
    out->width = 64.0f  + 6.0f * fsin_(x * 0.013f + p1);   /* WIDE half-pipe (58..70 half-width) */
    out->wall  = 54.0f  + 4.0f * fsin_(x * 0.021f + p2);   /* TALL walls curving up to the rim */
    out->ceil  = 0.0f;                                     /* open (tunnels: TODO) */
    float gate = fsin_(x * 0.006f + p3);                   /* slow open/lava alternation */
    out->lava  = (gate > 0.25f) ? (5.0f + 3.0f * fsin_(x * 0.05f)) : 0.0f;
}

void course_bake_strip_proc(uint32_t seed, uint8_t *floor, uint8_t *ceiling,
                            uint8_t *material, int mapsz, int x_lo, int x_hi) {
    unsigned mask = (unsigned)(mapsz - 1);

    const float RIM = 52.0f;                               /* canyon depth: rim above the floor (tall half-pipe) */
    for (int xc = x_lo; xc < x_hi; xc++) {                 /* clear columns to a rim that TRACKS
                                                            * the floor, so it descends with it */
        unsigned xm = (unsigned)xc & mask;
        CourseNode r;
        course_eval_long(seed, (float)xc, &r);
        uint8_t rim = clamp_u8(r.y + RIM);
        for (int z = 0; z < mapsz; z++) {
            unsigned idx = (unsigned)z * (unsigned)mapsz + xm;
            floor[idx]    = rim;
            ceiling[idx]  = (uint8_t)COURSE_OPEN_CEIL;
            material[idx] = COURSE_MAT_ROCK;
        }
    }
    const float WMAX = 52.0f;
    for (float x = (float)x_lo - WMAX; x <= (float)x_hi + WMAX; x += 0.5f) {
        CourseNode nd;
        course_eval_long(seed, x, &nd);
        stamp_corridor(floor, ceiling, material, mapsz, mask, &nd);
    }
}
