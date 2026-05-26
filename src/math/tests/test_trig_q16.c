/* test_trig_q16.c — headless tests for the CORDIC trig.
 *
 *   cc ... -o build/tests/trig_q16 \
 *      src/math/tests/test_trig_q16.c src/math/trig_q16.c
 *
 * Public domain (CC0). No warranty.
 */
#include "test_runner.h"
#include "math/trig_q16.h"

static int close(double a, double b, double tol) {
    double d = a - b; if (d < 0.0) d = -d; return d < tol;
}
#define R(x)  q16_from_double(x)
#define D(x)  q16_to_double(x)

static void test_sin_cos_known(void) {
    ASSERT(close(D(q16_sin(0)),            0.0,  0.002));
    ASSERT(close(D(q16_cos(0)),            1.0,  0.002));
    ASSERT(close(D(q16_sin(Q16_HALF_PI)),  1.0,  0.002));
    ASSERT(close(D(q16_cos(Q16_HALF_PI)),  0.0,  0.002));
    ASSERT(close(D(q16_sin(Q16_PI)),       0.0,  0.003));
    ASSERT(close(D(q16_cos(Q16_PI)),      -1.0,  0.003));
    ASSERT(close(D(q16_sin(R(0.7853981))), 0.7071, 0.003));   /* pi/4 */
    ASSERT(close(D(q16_cos(R(0.7853981))), 0.7071, 0.003));
    ASSERT(close(D(q16_sin(R(-0.7853981))),-0.7071, 0.003));
}

static void test_pythagorean(void) {
    /* sin^2 + cos^2 == 1 across a full turn, including wrap */
    for (int i = -400; i <= 400; i += 7) {
        q16_16_t ang = (q16_16_t)((int64_t)i * Q16_TWO_PI / 100);  /* -4pi..4pi */
        q16_16_t s, c; q16_sincos(ang, &s, &c);
        double ss = D(s), cc = D(c);
        ASSERT(close(ss*ss + cc*cc, 1.0, 0.01));
    }
}

static void test_tan(void) {
    ASSERT(close(D(q16_tan(R(0.7853981))), 1.0, 0.005));   /* tan(pi/4) */
    ASSERT(close(D(q16_tan(0)),            0.0, 0.002));
}

static void test_atan2(void) {
    ASSERT(close(D(q16_atan2(0, q16_from_int(1))),  0.0,        0.003));
    ASSERT(close(D(q16_atan2(q16_from_int(1), 0)),  1.5707963,  0.003));   /* +pi/2 */
    ASSERT(close(D(q16_atan2(q16_from_int(-1), 0)),-1.5707963,  0.003));   /* -pi/2 */
    ASSERT(close(D(q16_atan2(0, q16_from_int(-1))), 3.1415926,  0.004));   /* pi */
    ASSERT(close(D(q16_atan2(q16_from_int(1),  q16_from_int(1))),  0.7853981, 0.003));
    ASSERT(close(D(q16_atan2(q16_from_int(1),  q16_from_int(-1))), 2.3561944, 0.004));
    ASSERT(close(D(q16_atan2(q16_from_int(-1), q16_from_int(-1))),-2.3561944, 0.004));
    ASSERT(close(D(q16_atan2(q16_from_int(-1), q16_from_int(1))), -0.7853981, 0.003));

    /* round trip: atan2(sin a, cos a) == a, for a in (-pi, pi) */
    for (int i = -300; i <= 300; i += 11) {
        q16_16_t a = R((double)i / 100.0);     /* -3..3 rad */
        q16_16_t s, c; q16_sincos(a, &s, &c);
        ASSERT(close(D(q16_atan2(s, c)), (double)i / 100.0, 0.01));
    }
}

int main(void) {
    TEST_SUITE("trig_q16");
    RUN(test_sin_cos_known);
    RUN(test_pythagorean);
    RUN(test_tan);
    RUN(test_atan2);
    return TEST_SUITE_RESULT();
}
