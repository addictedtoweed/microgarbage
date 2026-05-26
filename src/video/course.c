/* ============================================================
 *  course.c — spline sampling + map baking for flight courses.
 *  See include/video/course.h. Portable: no stdio, no libm
 *  (radius tests use squared distance, so no sqrt needed).
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "video/course.h"

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
