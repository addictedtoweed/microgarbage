/* test_mat_q16.c — headless tests for mat2 / affine2 / mat3 (and the
 * vec2 ops they build on).
 *
 *   cc ... -o build/tests/mat_q16 \
 *      src/math/tests/test_mat_q16.c src/math/trig_q16.c
 *
 * Public domain (CC0). No warranty.
 */
#include "test_runner.h"
#include "math/mat_q16.h"

static int close(double a, double b, double tol) {
    double d = a - b; if (d < 0.0) d = -d; return d < tol;
}
#define D(x) q16_to_double(x)

static void test_vec2(void) {
    vec2_q16 a = vec2_q16_from_int(3, 4);
    ASSERT(close(D(vec2_q16_length(a)), 5.0, 0.01));
    vec2_q16 n = vec2_q16_normalize(a);
    ASSERT(close(D(n.x), 0.6, 0.01));
    ASSERT(close(D(n.y), 0.8, 0.01));
    ASSERT(close(D(vec2_q16_dot(vec2_q16_from_int(1,0), vec2_q16_from_int(0,1))), 0.0, 0.001));
    ASSERT(close(D(vec2_q16_cross(vec2_q16_from_int(1,0), vec2_q16_from_int(0,1))), 1.0, 0.001));
}

static void test_mat2(void) {
    /* identity leaves a vector alone */
    vec2_q16 v = vec2_q16_from_int(7, -3);
    vec2_q16 iv = mat2_q16_mul_vec2(mat2_q16_identity(), v);
    ASSERT_EQ_INT(v.x, iv.x);
    ASSERT_EQ_INT(v.y, iv.y);

    /* rotate (1,0) by +90deg -> (0,1) */
    mat2_q16 rot = mat2_q16_rotation(Q16_HALF_PI);
    vec2_q16 r = mat2_q16_mul_vec2(rot, vec2_q16_from_int(1, 0));
    ASSERT(close(D(r.x), 0.0, 0.005));
    ASSERT(close(D(r.y), 1.0, 0.005));

    /* two 45deg rotations compose to 90deg */
    mat2_q16 r45 = mat2_q16_rotation(q16_from_double(0.7853981));
    mat2_q16 r90 = mat2_q16_mul(r45, r45);
    vec2_q16 q = mat2_q16_mul_vec2(r90, vec2_q16_from_int(1, 0));
    ASSERT(close(D(q.x), 0.0, 0.01));
    ASSERT(close(D(q.y), 1.0, 0.01));

    /* scale+rotation: scale (2,2) only (angle 0) doubles length */
    mat2_q16 sc = mat2_q16_scale_rotation(0, q16_from_int(2), q16_from_int(2));
    vec2_q16 s = mat2_q16_mul_vec2(sc, vec2_q16_from_int(3, 0));
    ASSERT(close(D(s.x), 6.0, 0.01));
}

static void test_affine2(void) {
    /* rotate 90deg about origin, then translate by (10,0) */
    affine2_q16 a = affine2_q16_identity();
    a.m = mat2_q16_rotation(Q16_HALF_PI);
    a.tx = q16_from_int(10);
    vec2_q16 p = affine2_q16_apply(a, vec2_q16_from_int(1, 0));   /* ->(0,1)+(10,0) */
    ASSERT(close(D(p.x), 10.0, 0.01));
    ASSERT(close(D(p.y), 1.0,  0.01));

    /* compose with identity is a no-op */
    affine2_q16 c = affine2_q16_compose(a, affine2_q16_identity());
    vec2_q16 p2 = affine2_q16_apply(c, vec2_q16_from_int(1, 0));
    ASSERT(close(D(p2.x), 10.0, 0.01));
    ASSERT(close(D(p2.y), 1.0,  0.01));
}

static void test_mat3_basis(void) {
    /* identity passthrough */
    vec3_q16 v = vec3_q16_from_int(2, 3, 5);
    vec3_q16 iv = mat3_q16_mul_vec3(mat3_q16_identity(), v);
    ASSERT_EQ_INT(v.x, iv.x);
    ASSERT_EQ_INT(v.z, iv.z);

    /* basis columns = (right, up, fwd); basis * (nx,ny,1) must equal
     * right*nx + up*ny + fwd — the raycaster's per-pixel ray dir. */
    vec3_q16 right = vec3_q16_from_int(1, 0, 0);
    vec3_q16 up    = vec3_q16_from_int(0, 1, 0);
    vec3_q16 fwd   = vec3_q16_from_int(0, 0, 4);
    mat3_q16 basis = mat3_q16_from_basis_cols(right, up, fwd);

    q16_16_t nx = q16_from_double(0.5), ny = q16_from_double(-0.25);
    vec3_q16 d  = mat3_q16_mul_vec3(basis, vec3_q16_make(nx, ny, Q16_ONE));
    vec3_q16 expect = vec3_q16_add(vec3_q16_add(vec3_q16_scale(right, nx),
                                                vec3_q16_scale(up, ny)), fwd);
    ASSERT(close(D(d.x), D(expect.x), 0.005));
    ASSERT(close(D(d.y), D(expect.y), 0.005));
    ASSERT(close(D(d.z), D(expect.z), 0.005));
}

int main(void) {
    TEST_SUITE("mat_q16");
    RUN(test_vec2);
    RUN(test_mat2);
    RUN(test_affine2);
    RUN(test_mat3_basis);
    return TEST_SUITE_RESULT();
}
