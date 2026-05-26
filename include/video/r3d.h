/* ============================================================
 *  r3d.h — tiny fixed-point polygon renderer (the Star-Fox path).
 *
 *  Transforms meshes through an affine3 model->world->view pipeline,
 *  projects with an explicit divide-by-z, flat-shades each face by
 *  normal*light into a palette ramp, and rasterizes triangles into an
 *  8bpp framebuffer (the same buffer the Mode-7 path stretches to
 *  256x224 — so the present/DMA story is identical to the raycaster).
 *
 *  Unlike the heightfield raycaster this stores GEOMETRY, not a dense
 *  grid: world RAM is vertices+indices (KB), scratch is sized to the
 *  scene, and "draw distance" is just how much geometry you submit.
 *
 *  Backface cull + a per-pixel z-buffer (depth = 1/z, interpolated from
 *  per-triangle gradients), so terrain and placed objects occlude each
 *  other correctly regardless of size or draw order — no depth sorting
 *  needed for opaque geometry. Near-crossing triangles are dropped
 *  (near-plane clipping is a follow-up); a depth sort would only return
 *  for translucent passes (e.g. glowing lava).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VIDEO_R3D_H
#define VIDEO_R3D_H

#include <stdint.h>
#include "math/vec3_q16.h"
#include "math/mat_q16.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A mesh: model-space vertices + triangle indices (3 per face). */
typedef struct {
    const vec3_q16 *verts;
    int             nverts;
    const uint16_t *tris;       /* 3 vertex indices per triangle */
    int             ntris;
    const uint8_t  *tri_base;   /* per-face palette base index, or NULL => scene.base */
} R3dMesh;

/* A placed instance of a mesh. */
typedef struct {
    const R3dMesh *mesh;
    affine3_q16    xform;       /* model -> world */
} R3dObject;

/* The scene the renderer consumes for one frame. */
typedef struct {
    affine3_q16      view;      /* world -> view (build via affine3_q16_view) */
    q16_16_t         focal;     /* projection focal length, in framebuffer pixels */
    q16_16_t         near_z;    /* near clip: faces with any vertex closer are dropped */
    vec3_q16         light;     /* unit light direction, in VIEW space            */
    q16_16_t         ambient;   /* base lighting (Q16, ~0..1)                     */
    q16_16_t         diffuse;   /* directional lighting weight (Q16, ~0..1)       */
    int              base;      /* default palette base for a face's shade ramp   */
    int              ramp;      /* ramp length (shade = base + 0..ramp-1)         */
    const R3dObject *objs;
    int              nobjs;
} R3dScene;

/* Render the scene into fb (fbw*fbh, 8bpp palette indices; index 0 = sky).
 * Returns the number of per-pixel edge tests performed — the dominant fill
 * term, a proxy for estimating on-cart render cycles. */
long r3d_render(const R3dScene *s, uint8_t *fb, int fbw, int fbh);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_R3D_H */
