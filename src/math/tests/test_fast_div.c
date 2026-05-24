/* ============================================================
 *  Tests for fast_div.
 *
 *  Strategy: property-based. For each format (u32, s32, u64, s64),
 *  pick a representative set of divisors covering interesting cases
 *  (1, 2, small primes, powers of 2, max). For each divisor, pick
 *  a representative set of dividends. Verify that fdiv result
 *  matches plain C `/` and `%`. If any one disagrees, the test fails.
 *
 *  The strength of these tests is breadth, not depth: any algorithmic
 *  bug in the magic-number computation will show up as soon as the
 *  test hits a divisor/dividend combination that triggers it.
 * ============================================================ */

#include "test_runner.h"
#include "math/fast_div.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ============================================================
 *  Unsigned 32-bit
 * ============================================================ */

static void verify_u32_one(uint32_t d, uint32_t n) {
    div_u32_t dd = fdiv_u32_prepare(d);

    uint32_t got_q = fdiv_u32(n, &dd);
    uint32_t exp_q = n / d;
    if (got_q != exp_q) {
        FAIL("fdiv_u32(%u / %u): expected %u, got %u", n, d, exp_q, got_q);
    }

    fdiv_u32_result_t qr = fdiv_u32_qr(n, &dd);
    if (qr.quot != exp_q) {
        FAIL("fdiv_u32_qr quot(%u / %u): expected %u, got %u", n, d, exp_q, qr.quot);
    }
    uint32_t exp_r = n % d;
    if (qr.rem != exp_r) {
        FAIL("fdiv_u32_qr rem(%u %% %u): expected %u, got %u", n, d, exp_r, qr.rem);
    }
}

/* A representative set of dividends covering: small, near boundaries
 * within a divisor's range, near uint32 max, prime-ish, exact multiples. */
static const uint32_t U32_DIVIDENDS[] = {
    0u, 1u, 2u, 3u, 7u, 100u, 999u, 1000u, 1023u, 1024u, 1025u,
    65535u, 65536u, 65537u,
    1000000u, 1000001u,
    2147483647u, 2147483648u, 2147483649u,
    4294967290u, 4294967294u, 4294967295u,
};
#define U32_DIVIDENDS_COUNT (sizeof(U32_DIVIDENDS) / sizeof(U32_DIVIDENDS[0]))

static void verify_u32_divisor(uint32_t d) {
    for (size_t i = 0; i < U32_DIVIDENDS_COUNT; i++) {
        verify_u32_one(d, U32_DIVIDENDS[i]);
        if (d > 1) verify_u32_one(d, U32_DIVIDENDS[i] - 1);
        verify_u32_one(d, U32_DIVIDENDS[i] + 1);

        /* Exact multiples of d. */
        if (U32_DIVIDENDS[i] / d > 0) verify_u32_one(d, U32_DIVIDENDS[i] / d * d);
    }
}

static void test_u32_pow2_divisors(void) {
    /* All powers of 2 from 1 to 2^31. */
    for (int k = 0; k < 32; k++) {
        verify_u32_divisor((uint32_t)1u << k);
    }
}

static void test_u32_small_divisors(void) {
    for (uint32_t d = 1; d < 32; d++) {
        verify_u32_divisor(d);
    }
}

static void test_u32_prime_divisors(void) {
    static const uint32_t primes[] = {
        2u, 3u, 5u, 7u, 11u, 13u, 17u, 19u, 23u, 29u, 31u, 37u, 41u, 43u,
        47u, 53u, 59u, 61u, 67u, 71u, 73u, 79u, 83u, 89u, 97u,
        101u, 103u, 107u, 109u, 113u,
        65537u, 1000003u,
    };
    for (size_t i = 0; i < sizeof(primes)/sizeof(primes[0]); i++) {
        verify_u32_divisor(primes[i]);
    }
}

static void test_u32_large_divisors(void) {
    static const uint32_t larges[] = {
        100u, 1000u, 10000u, 100000u, 1000000u,
        16777216u,   /* 2^24 */
        16777217u,   /* 2^24 + 1 */
        2147483647u, /* INT32_MAX */
        2147483648u, /* 2^31 */
        2147483649u, /* 2^31 + 1 */
        3000000000u,
        4000000000u,
        4294967294u,
        4294967295u, /* UINT32_MAX */
    };
    for (size_t i = 0; i < sizeof(larges)/sizeof(larges[0]); i++) {
        verify_u32_divisor(larges[i]);
    }
}

/* ============================================================
 *  Signed 32-bit
 * ============================================================ */

static void verify_s32_one(int32_t d, int32_t n) {
    div_s32_t dd = fdiv_s32_prepare(d);

    int32_t got_q = fdiv_s32(n, &dd);
    int32_t exp_q = n / d;
    if (got_q != exp_q) {
        FAIL("fdiv_s32(%d / %d): expected %d, got %d", n, d, exp_q, got_q);
    }

    fdiv_s32_result_t qr = fdiv_s32_qr(n, &dd);
    if (qr.quot != exp_q) {
        FAIL("fdiv_s32_qr quot(%d / %d): expected %d, got %d", n, d, exp_q, qr.quot);
    }
    int32_t exp_r = n % d;
    if (qr.rem != exp_r) {
        FAIL("fdiv_s32_qr rem(%d %% %d): expected %d, got %d", n, d, exp_r, qr.rem);
    }
}

static const int32_t S32_DIVIDENDS[] = {
    0, 1, -1, 2, -2, 7, -7, 100, -100,
    1023, 1024, 1025, -1023, -1024, -1025,
    1000000, -1000000,
    2147483646, 2147483647, -2147483647,
};
#define S32_DIVIDENDS_COUNT (sizeof(S32_DIVIDENDS) / sizeof(S32_DIVIDENDS[0]))

static void verify_s32_divisor(int32_t d) {
    for (size_t i = 0; i < S32_DIVIDENDS_COUNT; i++) {
        verify_s32_one(d, S32_DIVIDENDS[i]);
    }
}

static void test_s32_small_positive_divisors(void) {
    for (int32_t d = 1; d < 20; d++) verify_s32_divisor(d);
}

static void test_s32_small_negative_divisors(void) {
    for (int32_t d = -1; d > -20; d--) verify_s32_divisor(d);
}

static void test_s32_pow2_divisors(void) {
    for (int k = 0; k < 31; k++) {
        verify_s32_divisor((int32_t)1 << k);
        verify_s32_divisor(-((int32_t)1 << k));
    }
}

static void test_s32_misc_divisors(void) {
    static const int32_t ds[] = {
        3, -3, 7, -7, 100, -100, 1000, -1000,
        65537, -65537, 1000003, -1000003,
        2147483647, -2147483647,
    };
    for (size_t i = 0; i < sizeof(ds)/sizeof(ds[0]); i++) {
        verify_s32_divisor(ds[i]);
    }
}

/* ============================================================
 *  Unsigned 64-bit
 * ============================================================ */

static void verify_u64_one(uint64_t d, uint64_t n) {
    div_u64_t dd = fdiv_u64_prepare(d);

    uint64_t got_q = fdiv_u64(n, &dd);
    uint64_t exp_q = n / d;
    if (got_q != exp_q) {
        FAIL("fdiv_u64(%llu / %llu): expected %llu, got %llu",
             (unsigned long long)n, (unsigned long long)d,
             (unsigned long long)exp_q, (unsigned long long)got_q);
    }

    fdiv_u64_result_t qr = fdiv_u64_qr(n, &dd);
    if (qr.quot != exp_q || qr.rem != n % d) {
        FAIL("fdiv_u64_qr(%llu / %llu): expected q=%llu r=%llu, got q=%llu r=%llu",
             (unsigned long long)n, (unsigned long long)d,
             (unsigned long long)exp_q, (unsigned long long)(n%d),
             (unsigned long long)qr.quot, (unsigned long long)qr.rem);
    }
}

static const uint64_t U64_DIVIDENDS[] = {
    0ull, 1ull, 2ull, 7ull, 100ull, 1024ull,
    1000000ull, 1000001ull,
    (uint64_t)UINT32_MAX,
    (uint64_t)UINT32_MAX + 1,
    (uint64_t)1 << 40,
    (uint64_t)1 << 48,
    (uint64_t)1 << 62,
    (uint64_t)1 << 63,
    ((uint64_t)1 << 63) + 1,
    UINT64_MAX - 1,
    UINT64_MAX,
};
#define U64_DIVIDENDS_COUNT (sizeof(U64_DIVIDENDS) / sizeof(U64_DIVIDENDS[0]))

static void verify_u64_divisor(uint64_t d) {
    for (size_t i = 0; i < U64_DIVIDENDS_COUNT; i++) {
        verify_u64_one(d, U64_DIVIDENDS[i]);
    }
}

static void test_u64_pow2_divisors(void) {
    for (int k = 0; k < 64; k++) {
        verify_u64_divisor((uint64_t)1 << k);
    }
}

static void test_u64_small_divisors(void) {
    for (uint64_t d = 1; d < 20; d++) {
        verify_u64_divisor(d);
    }
}

static void test_u64_misc_divisors(void) {
    static const uint64_t ds[] = {
        3ull, 7ull, 11ull, 13ull, 100ull, 1000ull, 1000003ull,
        (uint64_t)UINT32_MAX,
        (uint64_t)UINT32_MAX + 1,
        ((uint64_t)1 << 40) + 7,
        ((uint64_t)1 << 50) + 13,
        ((uint64_t)1 << 60) + 17,
        ((uint64_t)1 << 63) + 19,
        UINT64_MAX - 1,
    };
    for (size_t i = 0; i < sizeof(ds)/sizeof(ds[0]); i++) {
        verify_u64_divisor(ds[i]);
    }
}

/* ============================================================
 *  Signed 64-bit
 * ============================================================ */

static void verify_s64_one(int64_t d, int64_t n) {
    div_s64_t dd = fdiv_s64_prepare(d);

    int64_t got_q = fdiv_s64(n, &dd);
    int64_t exp_q = n / d;
    if (got_q != exp_q) {
        FAIL("fdiv_s64(%lld / %lld): expected %lld, got %lld",
             (long long)n, (long long)d, (long long)exp_q, (long long)got_q);
    }

    fdiv_s64_result_t qr = fdiv_s64_qr(n, &dd);
    if (qr.quot != exp_q || qr.rem != n % d) {
        FAIL("fdiv_s64_qr(%lld / %lld): expected q=%lld r=%lld, got q=%lld r=%lld",
             (long long)n, (long long)d,
             (long long)exp_q, (long long)(n%d),
             (long long)qr.quot, (long long)qr.rem);
    }
}

static const int64_t S64_DIVIDENDS[] = {
    0, 1, -1, 7, -7, 100, -100, 1024, -1024,
    1000000, -1000000,
    (int64_t)INT32_MAX, (int64_t)INT32_MIN,
    (int64_t)1 << 40, -((int64_t)1 << 40),
    (int64_t)1 << 60, -((int64_t)1 << 60),
    INT64_MAX, INT64_MAX - 1,
    -INT64_MAX,
};
#define S64_DIVIDENDS_COUNT (sizeof(S64_DIVIDENDS) / sizeof(S64_DIVIDENDS[0]))

static void verify_s64_divisor(int64_t d) {
    for (size_t i = 0; i < S64_DIVIDENDS_COUNT; i++) {
        verify_s64_one(d, S64_DIVIDENDS[i]);
    }
}

static void test_s64_small_divisors(void) {
    for (int64_t d = 1; d < 16; d++) {
        verify_s64_divisor(d);
        verify_s64_divisor(-d);
    }
}

static void test_s64_misc_divisors(void) {
    static const int64_t ds[] = {
        3, -3, 7, -7, 13, -13, 100, -100,
        (int64_t)1 << 20, -((int64_t)1 << 20),
        (int64_t)1 << 40, -((int64_t)1 << 40),
        INT64_MAX, -INT64_MAX,
    };
    for (size_t i = 0; i < sizeof(ds)/sizeof(ds[0]); i++) {
        verify_s64_divisor(ds[i]);
    }
}

/* ============================================================
 *  Edge cases
 * ============================================================ */

static void test_divide_by_one(void) {
    /* Trivial but important — pow2 path with shift=0. */
    div_u32_t d1 = fdiv_u32_prepare(1);
    ASSERT_EQ_INT(0, (int)fdiv_u32(0, &d1));
    ASSERT_EQ_INT(42, (int)fdiv_u32(42, &d1));
    ASSERT_EQ_INT(-1, (int)fdiv_u32(UINT32_MAX, &d1));  /* sign-bit display */

    div_s32_t d1s = fdiv_s32_prepare(1);
    ASSERT_EQ_INT(0, fdiv_s32(0, &d1s));
    ASSERT_EQ_INT(42, fdiv_s32(42, &d1s));
    ASSERT_EQ_INT(-42, fdiv_s32(-42, &d1s));
}

static void test_n_equals_d(void) {
    /* n == d should always give q=1, r=0. */
    div_u32_t d = fdiv_u32_prepare(13);
    fdiv_u32_result_t qr = fdiv_u32_qr(13, &d);
    ASSERT_EQ_INT(1, (int)qr.quot);
    ASSERT_EQ_INT(0, (int)qr.rem);

    /* For a hard divisor that exercises the add-fixup path: */
    div_u32_t d2 = fdiv_u32_prepare(3000000000u);
    fdiv_u32_result_t qr2 = fdiv_u32_qr(3000000000u, &d2);
    ASSERT_EQ_INT(1, (int)qr2.quot);
    ASSERT_EQ_INT(0, (int)qr2.rem);
}

int main(void) {
    TEST_SUITE("fast_div");

    /* Unsigned 32-bit */
    RUN(test_u32_pow2_divisors);
    RUN(test_u32_small_divisors);
    RUN(test_u32_prime_divisors);
    RUN(test_u32_large_divisors);

    /* Signed 32-bit */
    RUN(test_s32_small_positive_divisors);
    RUN(test_s32_small_negative_divisors);
    RUN(test_s32_pow2_divisors);
    RUN(test_s32_misc_divisors);

    /* Unsigned 64-bit */
    RUN(test_u64_pow2_divisors);
    RUN(test_u64_small_divisors);
    RUN(test_u64_misc_divisors);

    /* Signed 64-bit */
    RUN(test_s64_small_divisors);
    RUN(test_s64_misc_divisors);

    /* Edge cases */
    RUN(test_divide_by_one);
    RUN(test_n_equals_d);

    return TEST_SUITE_RESULT();
}
