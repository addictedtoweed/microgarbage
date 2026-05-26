/* ============================================================
 *  mat_q16.h — Q16.16 matrices, scoped to what the renderer uses.
 *
 *    mat2_q16    2x2            sprite/Mode-7 rotate+scale
 *    affine2_q16 2x2 + offset   Mode 7 transform (matrix + center)
 *    mat3_q16    3x3            camera basis / 3D rotation
 *
 *  Deliberately NO 4x4 homogeneous stack: a raycaster marches rays
 *  from a basis, it doesn't push vertex lists through a projection,
 *  so a GL-style 4x4 would be unused machinery.
 *
 *  Header-only, static inline. Rotation constructors call into
 *  trig_q16 (CORDIC), so TUs that use them link src/math/trig_q16.c.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MAT_Q16_H
#define MAT_Q16_H

#include "math/fixed_point.h"
#include "math/vec2_q16.h"
#include "math/vec3_q16.h"
#include "math/trig_q16.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 2x2 :  [ a b ]  acting on column (x,y) ---------------- */
/*            [ c d ]                                            */
typedef struct { q16_16_t a, b, c, d; } mat2_q16;

static inline mat2_q16 mat2_q16_identity(void) {
    mat2_q16 m = { Q16_ONE, 0, 0, Q16_ONE }; return m;
}
static inline vec2_q16 mat2_q16_mul_vec2(mat2_q16 m, vec2_q16 v) {
    return vec2_q16_make(q16_mul(m.a, v.x) + q16_mul(m.b, v.y),
                         q16_mul(m.c, v.x) + q16_mul(m.d, v.y));
}
static inline mat2_q16 mat2_q16_mul(mat2_q16 A, mat2_q16 B) {
    mat2_q16 r;
    r.a = q16_mul(A.a, B.a) + q16_mul(A.b, B.c);
    r.b = q16_mul(A.a, B.b) + q16_mul(A.b, B.d);
    r.c = q16_mul(A.c, B.a) + q16_mul(A.d, B.c);
    r.d = q16_mul(A.c, B.b) + q16_mul(A.d, B.d);
    return r;
}
/* Rotation by `radians` (CCW): [ cos -sin ; sin cos ]. */
static inline mat2_q16 mat2_q16_rotation(q16_16_t radians) {
    q16_16_t s, c; q16_sincos(radians, &s, &c);
    mat2_q16 m = { c, -s, s, c }; return m;
}
/* Scale (sx,sy) then rotate by `radians` — the Mode-7 building block. */
static inline mat2_q16 mat2_q16_scale_rotation(q16_16_t radians, q16_16_t sx, q16_16_t sy) {
    q16_16_t s, c; q16_sincos(radians, &s, &c);
    mat2_q16 m = { q16_mul(c, sx), q16_mul(-s, sy),
                   q16_mul(s, sx), q16_mul(c, sy) };
    return m;
}

/* ---- 2D affine : 2x2 matrix + translation ------------------ */
typedef struct { mat2_q16 m; q16_16_t tx, ty; } affine2_q16;

static inline affine2_q16 affine2_q16_identity(void) {
    affine2_q16 a = { { Q16_ONE, 0, 0, Q16_ONE }, 0, 0 }; return a;
}
static inline vec2_q16 affine2_q16_apply(affine2_q16 a, vec2_q16 v) {
    vec2_q16 r = mat2_q16_mul_vec2(a.m, v);
    return vec2_q16_make(r.x + a.tx, r.y + a.ty);
}
/* Compose: apply B, then A  ->  (A o B)(v) = A(B(v)). */
static inline affine2_q16 affine2_q16_compose(affine2_q16 A, affine2_q16 B) {
    affine2_q16 r;
    r.m = mat2_q16_mul(A.m, B.m);
    vec2_q16 t = mat2_q16_mul_vec2(A.m, vec2_q16_make(B.tx, B.ty));
    r.tx = t.x + A.tx;
    r.ty = t.y + A.ty;
    return r;
}

/* ---- 3x3 (row-major) : camera basis / 3D rotation ---------- */
typedef struct { q16_16_t m[9]; } mat3_q16;   /* m[0..2]=row0, etc. */

static inline mat3_q16 mat3_q16_identity(void) {
    mat3_q16 r = { { Q16_ONE,0,0, 0,Q16_ONE,0, 0,0,Q16_ONE } }; return r;
}
static inline vec3_q16 mat3_q16_mul_vec3(mat3_q16 M, vec3_q16 v) {
    return vec3_q16_make(
        q16_mul(M.m[0], v.x) + q16_mul(M.m[1], v.y) + q16_mul(M.m[2], v.z),
        q16_mul(M.m[3], v.x) + q16_mul(M.m[4], v.y) + q16_mul(M.m[5], v.z),
        q16_mul(M.m[6], v.x) + q16_mul(M.m[7], v.y) + q16_mul(M.m[8], v.z));
}
static inline mat3_q16 mat3_q16_mul(mat3_q16 A, mat3_q16 B) {
    mat3_q16 r;
    for (int row = 0; row < 3; row++)
        for (int col = 0; col < 3; col++)
            r.m[row*3 + col] = q16_mul(A.m[row*3+0], B.m[0*3+col])
                             + q16_mul(A.m[row*3+1], B.m[1*3+col])
                             + q16_mul(A.m[row*3+2], B.m[2*3+col]);
    return r;
}
/* Basis matrix whose COLUMNS are (right, up, fwd). Then
 * mat3_q16_mul_vec3(basis, {nx,ny,1}) == right*nx + up*ny + fwd —
 * exactly the per-pixel ray direction of the canyon raycaster. */
static inline mat3_q16 mat3_q16_from_basis_cols(vec3_q16 right, vec3_q16 up, vec3_q16 fwd) {
    mat3_q16 r = { {
        right.x, up.x, fwd.x,
        right.y, up.y, fwd.y,
        right.z, up.z, fwd.z
    } };
    return r;
}

#ifdef __cplusplus
}
#endif

#endif /* MAT_Q16_H */
