/* test_vec3_q16.c — headless tests for q16_sqrt / q16_rsqrt and the
 * Q16.16 3D vector ops. No libm: references are exact constants or
 * self-consistency identities, compared within a fixed tolerance.
 *
 *   cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
 *      -o build/tests/vec3_q16 src/math/tests/test_vec3_q16.c
 *
 * Public domain (CC0). No warranty.
 */
#include "test_runner.h"
#include "math/vec3_q16.h"

/* |a-b| < tol, no libm. */
static int close(double a, double b, double tol) {
    double d = a - b;
    if (d < 0.0) d = -d;
    return d < tol;
}

/* ---- scalar sqrt / rsqrt ----------------------------------- */

static void test_q16_sqrt(void) {
    /* perfect squares are exact */
    ASSERT_EQ_INT(q16_from_int(2), q16_sqrt(q16_from_int(4)));
    ASSERT_EQ_INT(q16_from_int(3), q16_sqrt(q16_from_int(9)));
    ASSERT_EQ_INT(q16_from_int(12), q16_sqrt(q16_from_int(144)));
    ASSERT_EQ_INT(Q16_ONE, q16_sqrt(Q16_ONE));            /* sqrt(1)=1 */
    ASSERT_EQ_INT(0, q16_sqrt(0));
    ASSERT_EQ_INT(0, q16_sqrt(-5));                       /* guard */

    /* irrational values within tolerance */
    ASSERT(close(q16_to_double(q16_sqrt(q16_from_int(2))), 1.41421356, 0.001));
    ASSERT(close(q16_to_double(q16_sqrt(q16_from_float(0.25f))), 0.5, 0.001));

    /* self-consistency: sqrt(x)^2 ~= x across a range */
    for (int i = 1; i <= 20000; i += 137) {
        q16_16_t x = q16_from_int(i);
        q16_16_t r = q16_sqrt(x);
        ASSERT(close(q16_to_double(q16_mul(r, r)), (double)i, (double)i * 0.001 + 0.01));
    }
}

static void test_q16_rsqrt(void) {
    ASSERT(close(q16_to_double(q16_rsqrt(Q16_ONE)), 1.0, 0.001));
    ASSERT(close(q16_to_double(q16_rsqrt(q16_from_int(4))), 0.5, 0.001));
    ASSERT(close(q16_to_double(q16_rsqrt(q16_from_float(0.25f))), 2.0, 0.002));
    ASSERT(close(q16_to_double(q16_rsqrt(q16_from_int(2))), 0.70710678, 0.002));

    /* rsqrt(x) ~= 1 / sqrt(x) */
    for (int i = 1; i <= 20000; i += 211) {
        q16_16_t x = q16_from_int(i);
        double got  = q16_to_double(q16_rsqrt(x));
        double want = q16_to_double(q16_reciprocal(q16_sqrt(x)));
        ASSERT(close(got, want, want * 0.02 + 0.0005));
    }
}

/* ---- vector ops -------------------------------------------- */

static void test_vec_arith(void) {
    vec3_q16 a = vec3_q16_from_int(1, 2, 3);
    vec3_q16 b = vec3_q16_from_int(4, 5, 6);

    vec3_q16 s = vec3_q16_add(a, b);
    ASSERT_EQ_INT(q16_from_int(5), s.x);
    ASSERT_EQ_INT(q16_from_int(7), s.y);
    ASSERT_EQ_INT(q16_from_int(9), s.z);

    vec3_q16 d = vec3_q16_sub(b, a);
    ASSERT_EQ_INT(q16_from_int(3), d.x);

    vec3_q16 h = vec3_q16_scale(a, q16_from_float(0.5f));
    ASSERT(close(q16_to_double(h.y), 1.0, 0.001));        /* 2 * 0.5 */

    /* dot = 1*4 + 2*5 + 3*6 = 32 */
    ASSERT(close(q16_to_double(vec3_q16_dot(a, b)), 32.0, 0.01));

    /* cross(x,y) = z */
    vec3_q16 cx = vec3_q16_cross(vec3_q16_from_int(1, 0, 0), vec3_q16_from_int(0, 1, 0));
    ASSERT(close(q16_to_double(cx.z), 1.0, 0.001));
    ASSERT(close(q16_to_double(cx.x), 0.0, 0.001));
}

static void test_vec_length_normalize(void) {
    /* 3-4-5 right triangle: length 5 */
    vec3_q16 v = vec3_q16_from_int(3, 4, 0);
    ASSERT(close(q16_to_double(vec3_q16_length(v)), 5.0, 0.01));

    /* normalize a small vector -> unit length, direction preserved */
    vec3_q16 n = vec3_q16_normalize(v);
    ASSERT(close(q16_to_double(vec3_q16_length(n)), 1.0, 0.01));
    ASSERT(close(q16_to_double(n.x), 0.6, 0.01));         /* 3/5 */
    ASSERT(close(q16_to_double(n.y), 0.8, 0.01));         /* 4/5 */

    /* the renderer's hard case: a steep heightfield normal whose
     * components (and length) far exceed the Q16.16 range -> int64 path.
     * length() would overflow here, but normalize() must stay unit. */
    vec3_q16 big = vec3_q16_from_int(210, 3, -180);
    vec3_q16 bn  = vec3_q16_normalize(big);
    double len = q16_to_double(bn.x) * q16_to_double(bn.x)
               + q16_to_double(bn.y) * q16_to_double(bn.y)
               + q16_to_double(bn.z) * q16_to_double(bn.z);
    ASSERT(close(len, 1.0, 0.02));                        /* |unit|^2 ~= 1 */

    /* zero vector is returned unchanged (no divide-by-zero) */
    vec3_q16 z = vec3_q16_normalize(vec3_q16_from_int(0, 0, 0));
    ASSERT_EQ_INT(0, z.x);
}

int main(void) {
    TEST_SUITE("vec3_q16");
    RUN(test_q16_sqrt);
    RUN(test_q16_rsqrt);
    RUN(test_vec_arith);
    RUN(test_vec_length_normalize);
    return TEST_SUITE_RESULT();
}
