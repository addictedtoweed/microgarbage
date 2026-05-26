/* ============================================================
 *  r3d.c — fixed-point polygon renderer. See r3d.h.
 *  Q16.16 transform + project + backface cull + flat shade +
 *  z-buffered edge-function raster.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "video/r3d.h"
#include "math/fixed_point.h"

/* Scratch sized to the scene's working set (not to the world). */
#define R3D_MAX_VERTS  4096
#define R3D_MAX_TRIS   4096
#define R3D_MAX_PIXELS (128 * 128)   /* z-buffer cap (the low-res Mode-7 framebuffer) */

static vec3_q16 g_vv[R3D_MAX_VERTS];   /* view-space vertices         */
static int      g_sx[R3D_MAX_VERTS];   /* projected screen x (pixels) */
static int      g_sy[R3D_MAX_VERTS];   /* projected screen y (pixels) */
static uint8_t  g_behind[R3D_MAX_VERTS];
static int32_t  g_zbuf[R3D_MAX_PIXELS];/* per-pixel 1/z (Q16); larger = nearer */

typedef struct { int a, b, c; uint8_t color; } R3dTri;
static R3dTri   g_tris[R3D_MAX_TRIS];

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int min3(int a, int b, int c) { int m = a < b ? a : b; return m < c ? m : c; }
static inline int max3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }

void r3d_render(const R3dScene *s, uint8_t *fb, int fbw, int fbh) {
    const int npix  = fbw * fbh;
    const int use_z = (npix <= R3D_MAX_PIXELS);            /* else fall back to overwrite */
    for (int i = 0; i < npix; i++) fb[i] = 0;              /* clear to sky */
    if (use_z) for (int i = 0; i < npix; i++) g_zbuf[i] = 0;   /* 0 = infinitely far */

    const q16_16_t cx = q16_from_int(fbw) >> 1;            /* screen centre (Q16) */
    const q16_16_t cy = q16_from_int(fbh) >> 1;

    /* ---- transform + project every object's vertices, gather faces ---- */
    int nv = 0, nt = 0;
    for (int o = 0; o < s->nobjs; o++) {
        const R3dObject *obj = &s->objs[o];
        const R3dMesh   *m   = obj->mesh;
        affine3_q16 mv = affine3_q16_compose(s->view, obj->xform);   /* model -> view */
        int base_v = nv;

        for (int i = 0; i < m->nverts && nv < R3D_MAX_VERTS; i++, nv++) {
            vec3_q16 v = affine3_q16_apply(mv, m->verts[i]);
            g_vv[nv] = v;
            if (v.z < s->near_z) {                          /* behind near plane */
                g_behind[nv] = 1; g_sx[nv] = 0; g_sy[nv] = 0;
            } else {
                g_behind[nv] = 0;
                g_sx[nv] = q16_to_int(cx + q16_mul(q16_div(v.x, v.z), s->focal));
                g_sy[nv] = q16_to_int(cy - q16_mul(q16_div(v.y, v.z), s->focal));
            }
        }

        for (int ti = 0; ti < m->ntris && nt < R3D_MAX_TRIS; ti++) {
            int a = base_v + m->tris[ti * 3 + 0];
            int b = base_v + m->tris[ti * 3 + 1];
            int c = base_v + m->tris[ti * 3 + 2];
            if (g_behind[a] || g_behind[b] || g_behind[c]) continue;  /* v1: drop near-crossing */

            /* outward face normal (consistent CCW winding) */
            vec3_q16 n = vec3_q16_normalize(vec3_q16_cross(
                vec3_q16_sub(g_vv[b], g_vv[a]), vec3_q16_sub(g_vv[c], g_vv[a])));
            /* backface cull: ctr (= 3*centre) is the view direction to the face;
             * front-facing => n . ctr < 0. */
            vec3_q16 ctr = vec3_q16_add(vec3_q16_add(g_vv[a], g_vv[b]), g_vv[c]);
            if (vec3_q16_dot(n, ctr) >= 0) continue;

            q16_16_t ndl = vec3_q16_dot(n, s->light);
            if (ndl < 0) ndl = 0;
            q16_16_t bright = s->ambient + q16_mul(s->diffuse, ndl);
            int shade = q16_to_int(q16_mul(bright, q16_from_int(s->ramp - 1)) + (Q16_ONE >> 1));
            shade = clampi(shade, 0, s->ramp - 1);
            int base = m->tri_base ? (int)m->tri_base[ti] : s->base;

            g_tris[nt].a = a; g_tris[nt].b = b; g_tris[nt].c = c;
            g_tris[nt].color = (uint8_t)(base + shade);
            nt++;
        }
    }

    /* ---- rasterize, z-tested. Depth = 1/z is linear in screen space, so we
     * interpolate it from per-triangle gradients (two divides per triangle,
     * an add per pixel — no per-pixel divide). ---- */
    for (int i = 0; i < nt; i++) {
        const R3dTri *t = &g_tris[i];
        int ax = g_sx[t->a], ay = g_sy[t->a];
        int bx = g_sx[t->b], by = g_sy[t->b];
        int cx2 = g_sx[t->c], cy2 = g_sy[t->c];
        int area = (bx - ax) * (cy2 - ay) - (by - ay) * (cx2 - ax);
        if (area == 0) continue;

        int32_t za = q16_div(Q16_ONE, g_vv[t->a].z);        /* 1/z, Q16 */
        int32_t zb = q16_div(Q16_ONE, g_vv[t->b].z);
        int32_t zc = q16_div(Q16_ONE, g_vv[t->c].z);
        int64_t dzdx = ((int64_t)(zb - za) * (cy2 - ay) - (int64_t)(zc - za) * (by - ay)) / area;
        int64_t dzdy = ((int64_t)(bx - ax) * (zc - za) - (int64_t)(cx2 - ax) * (zb - za)) / area;

        int minx = clampi(min3(ax, bx, cx2), 0, fbw - 1);
        int maxx = clampi(max3(ax, bx, cx2), 0, fbw - 1);
        int miny = clampi(min3(ay, by, cy2), 0, fbh - 1);
        int maxy = clampi(max3(ay, by, cy2), 0, fbh - 1);

        for (int y = miny; y <= maxy; y++) {
            int64_t depth = (int64_t)za + dzdx * (int64_t)(minx - ax) + dzdy * (int64_t)(y - ay);
            for (int x = minx; x <= maxx; x++, depth += dzdx) {
                int w0 = (bx - ax) * (y - ay) - (by - ay) * (x - ax);
                int w1 = (cx2 - bx) * (y - by) - (cy2 - by) * (x - bx);
                int w2 = (ax - cx2) * (y - cy2) - (ay - cy2) * (x - cx2);
                int inside = area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0)
                                      : (w0 <= 0 && w1 <= 0 && w2 <= 0);
                if (!inside) continue;
                int idx = y * fbw + x;
                if (use_z) {
                    int32_t d = (int32_t)depth;            /* inside => within the vertex 1/z range */
                    if (d > g_zbuf[idx]) { g_zbuf[idx] = d; fb[idx] = t->color; }
                } else {
                    fb[idx] = t->color;
                }
            }
        }
    }
}
