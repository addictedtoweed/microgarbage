/* Tests for fixed_point. */

#include "test_runner.h"
#include "math/fixed_point.h"

#include <math.h>
#include <stdio.h>

/* Small float-comparison helper for the conversion tests. */
#define ASSERT_FLOAT_NEAR(expected, actual, tol) do { \
    float _e = (float)(expected); \
    float _a = (float)(actual); \
    float _t = (float)(tol); \
    float _diff = (_e > _a) ? (_e - _a) : (_a - _e); \
    if (_diff > _t) { \
        TR_FAIL_AT(__FILE__, __LINE__, \
            "expected %g, got %g (diff %g > tol %g)", \
            (double)_e, (double)_a, (double)_diff, (double)_t); \
        return; \
    } \
} while (0)

/* ============================================================
 *  Q16.16 — most commonly used, most thoroughly tested
 * ============================================================ */

static void test_q16_int_roundtrip(void) {
    for (int i = -100; i <= 100; i++) {
        ASSERT_EQ_INT(i, q16_to_int(q16_from_int(i)));
    }
}

static void test_q16_float_roundtrip(void) {
    /* Values exactly representable in Q16.16 round-trip exactly. */
    ASSERT_FLOAT_NEAR(0.5,    q16_to_float(q16_from_float(0.5f)),    1e-6);
    ASSERT_FLOAT_NEAR(0.25,   q16_to_float(q16_from_float(0.25f)),   1e-6);
    ASSERT_FLOAT_NEAR(-3.75,  q16_to_float(q16_from_float(-3.75f)),  1e-6);
    ASSERT_FLOAT_NEAR(100.0,  q16_to_float(q16_from_float(100.0f)),  1e-6);
}

static void test_q16_one(void) {
    ASSERT_EQ_INT(1, q16_to_int(Q16_ONE));
    ASSERT_FLOAT_NEAR(1.0, q16_to_float(Q16_ONE), 1e-6);
}

static void test_q16_add(void) {
    q16_16_t a = q16_from_float(1.5f);
    q16_16_t b = q16_from_float(2.25f);
    q16_16_t c = q16_add(a, b);
    ASSERT_FLOAT_NEAR(3.75, q16_to_float(c), 1e-6);
}

static void test_q16_sub(void) {
    q16_16_t a = q16_from_float(5.0f);
    q16_16_t b = q16_from_float(2.5f);
    ASSERT_FLOAT_NEAR(2.5, q16_to_float(q16_sub(a, b)), 1e-6);
}

static void test_q16_mul(void) {
    /* 1.5 * 2.0 = 3.0 */
    q16_16_t a = q16_from_float(1.5f);
    q16_16_t b = q16_from_float(2.0f);
    ASSERT_FLOAT_NEAR(3.0, q16_to_float(q16_mul(a, b)), 1e-4);

    /* 0.5 * 0.5 = 0.25 */
    a = q16_from_float(0.5f);
    b = q16_from_float(0.5f);
    ASSERT_FLOAT_NEAR(0.25, q16_to_float(q16_mul(a, b)), 1e-4);

    /* -3.0 * 2.0 = -6.0 */
    a = q16_from_float(-3.0f);
    b = q16_from_float(2.0f);
    ASSERT_FLOAT_NEAR(-6.0, q16_to_float(q16_mul(a, b)), 1e-4);
}

static void test_q16_div(void) {
    /* 6.0 / 2.0 = 3.0 */
    q16_16_t a = q16_from_float(6.0f);
    q16_16_t b = q16_from_float(2.0f);
    ASSERT_FLOAT_NEAR(3.0, q16_to_float(q16_div(a, b)), 1e-4);

    /* 1.0 / 4.0 = 0.25 */
    a = q16_from_float(1.0f);
    b = q16_from_float(4.0f);
    ASSERT_FLOAT_NEAR(0.25, q16_to_float(q16_div(a, b)), 1e-4);
}

static void test_q16_mac(void) {
    /* 1.0 + 2.0 * 3.0 = 7.0 */
    q16_16_t a = q16_from_float(1.0f);
    q16_16_t b = q16_from_float(2.0f);
    q16_16_t c = q16_from_float(3.0f);
    ASSERT_FLOAT_NEAR(7.0, q16_to_float(q16_mac(a, b, c)), 1e-4);
}

static void test_q16_neg_abs(void) {
    ASSERT_FLOAT_NEAR(-3.0, q16_to_float(q16_neg(q16_from_float(3.0f))), 1e-6);
    ASSERT_FLOAT_NEAR(3.0,  q16_to_float(q16_abs(q16_from_float(-3.0f))), 1e-6);
    ASSERT_FLOAT_NEAR(3.0,  q16_to_float(q16_abs(q16_from_float(3.0f))),  1e-6);
}

static void test_q16_sat_add_saturates(void) {
    /* Max + 1 saturates to Max. */
    q16_16_t r = q16_sat_add(Q16_MAX, q16_from_int(1));
    ASSERT_EQ_INT(Q16_MAX, r);

    /* Min + (-1) saturates to Min. */
    r = q16_sat_add(Q16_MIN, q16_from_int(-1));
    ASSERT_EQ_INT(Q16_MIN, r);
}

static void test_q16_sat_neg_min(void) {
    /* -Q16_MIN can't be represented; saturates to Q16_MAX. */
    ASSERT_EQ_INT(Q16_MAX, q16_sat_neg(Q16_MIN));
}

static void test_q16_reciprocal(void) {
    /* 1 / 4.0 = 0.25 */
    q16_16_t r = q16_reciprocal(q16_from_float(4.0f));
    ASSERT_FLOAT_NEAR(0.25, q16_to_float(r), 1e-3);
}

static void test_q16_int_round(void) {
    /* 1.4 → 1, 1.6 → 2, -1.4 → -1, -1.6 → -2 */
    ASSERT_EQ_INT(1, q16_to_int_round(q16_from_float(1.4f)));
    ASSERT_EQ_INT(2, q16_to_int_round(q16_from_float(1.6f)));
}

/* ============================================================
 *  Q15 — audio sample format
 * ============================================================ */

static void test_q15_float_roundtrip(void) {
    ASSERT_FLOAT_NEAR(0.5,   q15_to_float(q15_from_float(0.5f)),   1e-4);
    ASSERT_FLOAT_NEAR(-0.5,  q15_to_float(q15_from_float(-0.5f)),  1e-4);
    ASSERT_FLOAT_NEAR(0.123, q15_to_float(q15_from_float(0.123f)), 1e-4);
}

static void test_q15_mul(void) {
    /* 0.5 * 0.5 = 0.25 */
    q15_t a = q15_from_float(0.5f);
    q15_t b = q15_from_float(0.5f);
    ASSERT_FLOAT_NEAR(0.25, q15_to_float(q15_mul(a, b)), 1e-3);
}

static void test_q15_sat_mul(void) {
    /* Q15_MIN * Q15_MIN should saturate (would otherwise overflow). */
    q15_t r = q15_sat_mul(Q15_MIN, Q15_MIN);
    ASSERT_EQ_INT(Q15_MAX, r);
}

/* ============================================================
 *  Q31
 * ============================================================ */

static void test_q31_float_roundtrip(void) {
    ASSERT_FLOAT_NEAR(0.5,   q31_to_float(q31_from_float(0.5f)),   1e-7);
    ASSERT_FLOAT_NEAR(-0.5,  q31_to_float(q31_from_float(-0.5f)),  1e-7);
}

static void test_q31_mul(void) {
    q31_t a = q31_from_float(0.5f);
    q31_t b = q31_from_float(0.5f);
    ASSERT_FLOAT_NEAR(0.25, q31_to_float(q31_mul(a, b)), 1e-6);
}

/* ============================================================
 *  Q32.32 — phase accumulator format
 * ============================================================ */

static void test_q32_int_roundtrip(void) {
    ASSERT_EQ_INT(100, (int)q32_to_int(q32_from_int(100)));
    ASSERT_EQ_INT(-100, (int)q32_to_int(q32_from_int(-100)));
}

static void test_q32_float_roundtrip(void) {
    ASSERT_FLOAT_NEAR(0.5,   q32_to_float(q32_from_float(0.5f)),  1e-7);
    ASSERT_FLOAT_NEAR(100.0, q32_to_float(q32_from_float(100.0f)), 1e-4);
}

static void test_q32_add_wraps(void) {
    /* Phase accumulator usage: at max + 1 we wrap (no saturate). */
    q32_32_t a = Q32_MAX;
    q32_32_t b = 1;
    q32_32_t r = q32_add(a, b);
    /* Should be negative (wrapped to min-ish). */
    ASSERT(r < 0);
}

static void test_q32_sat_add(void) {
    /* Max + 1 saturates. */
    ASSERT_EQ_INT(Q32_MAX, q32_sat_add(Q32_MAX, 1));
}

static void test_q32_scale_q16(void) {
    /* Scale a Q32.32 of 10.0 by Q16.16 0.5 → 5.0. */
    q32_32_t a = q32_from_int(10);
    q16_16_t s = q16_from_float(0.5f);
    q32_32_t r = q32_scale_q16(a, s);
    ASSERT_FLOAT_NEAR(5.0, q32_to_float(r), 1e-4);
}

/* ============================================================
 *  Q48.16 — long counter format
 * ============================================================ */

static void test_q48_int_roundtrip(void) {
    /* Q48 holds very large integers — test with a big value. */
    int64_t big = (int64_t)1 << 40;
    ASSERT(q48_to_int(q48_from_int(big)) == big);
}

static void test_q48_add(void) {
    q48_16_t a = q48_from_int(1000);
    q48_16_t b = q48_from_int(2000);
    ASSERT(q48_to_int(q48_add(a, b)) == 3000);
}

static void test_q48_float_roundtrip(void) {
    ASSERT_FLOAT_NEAR(0.5,  q48_to_float(q48_from_float(0.5f)),  1e-4);
    ASSERT_FLOAT_NEAR(1234.5, q48_to_float(q48_from_float(1234.5f)), 1e-3);
}

/* ============================================================
 *  Cross-format conversions
 * ============================================================ */

static void test_q15_to_q16(void) {
    q15_t s = q15_from_float(0.5f);
    q16_16_t r = q15_to_q16(s);
    ASSERT_FLOAT_NEAR(0.5, q16_to_float(r), 1e-4);
}

static void test_q16_to_q32(void) {
    q16_16_t a = q16_from_int(42);
    q32_32_t b = q16_to_q32(a);
    ASSERT_EQ_INT(42, (int)q32_to_int(b));
}

static void test_q32_to_q16_saturates(void) {
    /* A Q32.32 holding a very large value, converted to Q16.16,
     * should saturate. */
    q32_32_t big = q32_from_int(100000);  /* > Q16 max */
    q16_16_t r = q32_to_q16(big);
    ASSERT_EQ_INT(Q16_MAX, r);
}

int main(void) {
    TEST_SUITE("fixed_point");

    /* Q16.16 */
    RUN(test_q16_int_roundtrip);
    RUN(test_q16_float_roundtrip);
    RUN(test_q16_one);
    RUN(test_q16_add);
    RUN(test_q16_sub);
    RUN(test_q16_mul);
    RUN(test_q16_div);
    RUN(test_q16_mac);
    RUN(test_q16_neg_abs);
    RUN(test_q16_sat_add_saturates);
    RUN(test_q16_sat_neg_min);
    RUN(test_q16_reciprocal);
    RUN(test_q16_int_round);

    /* Q15 */
    RUN(test_q15_float_roundtrip);
    RUN(test_q15_mul);
    RUN(test_q15_sat_mul);

    /* Q31 */
    RUN(test_q31_float_roundtrip);
    RUN(test_q31_mul);

    /* Q32.32 */
    RUN(test_q32_int_roundtrip);
    RUN(test_q32_float_roundtrip);
    RUN(test_q32_add_wraps);
    RUN(test_q32_sat_add);
    RUN(test_q32_scale_q16);

    /* Q48.16 */
    RUN(test_q48_int_roundtrip);
    RUN(test_q48_add);
    RUN(test_q48_float_roundtrip);

    /* Cross-format */
    RUN(test_q15_to_q16);
    RUN(test_q16_to_q32);
    RUN(test_q32_to_q16_saturates);

    return TEST_SUITE_RESULT();
}
