/* Tests for math/bits.h — clz / ctz / popcount (builtin + fallback).
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "math/bits.h"

#include <stdint.h>

/* ---- clz ------------------------------------------------------- */

static void test_clz32(void) {
    ASSERT_EQ_INT(32, bits_clz32(0));            /* defined: full width */
    ASSERT_EQ_INT(31, bits_clz32(1));
    ASSERT_EQ_INT(0,  bits_clz32(0x80000000u));
    ASSERT_EQ_INT(0,  bits_clz32(0xFFFFFFFFu));
    ASSERT_EQ_INT(24, bits_clz32(0x80u));        /* bit 7 set */
    ASSERT_EQ_INT(16, bits_clz32(0x8000u));      /* bit 15 set */
    for (int b = 0; b < 32; b++)
        ASSERT_EQ_INT(31 - b, bits_clz32(1u << b));
}

static void test_clz64(void) {
    ASSERT_EQ_INT(64, bits_clz64(0));
    ASSERT_EQ_INT(63, bits_clz64(1));
    ASSERT_EQ_INT(0,  bits_clz64(0x8000000000000000ull));
    ASSERT_EQ_INT(31, bits_clz64(0x100000000ull)); /* bit 32 */
    ASSERT_EQ_INT(32, bits_clz64(0x80000000ull));  /* bit 31 */
    for (int b = 0; b < 64; b++)
        ASSERT_EQ_INT(63 - b, bits_clz64(1ull << b));
}

/* ---- ctz ------------------------------------------------------- */

static void test_ctz32(void) {
    ASSERT_EQ_INT(32, bits_ctz32(0));            /* defined: full width */
    ASSERT_EQ_INT(0,  bits_ctz32(1));
    ASSERT_EQ_INT(0,  bits_ctz32(0xFFFFFFFFu));
    ASSERT_EQ_INT(31, bits_ctz32(0x80000000u));
    ASSERT_EQ_INT(8,  bits_ctz32(0x100u));       /* bit 8 lowest */
    ASSERT_EQ_INT(4,  bits_ctz32(0xF0u));        /* lowest set is bit 4 */
    for (int b = 0; b < 32; b++)
        ASSERT_EQ_INT(b, bits_ctz32(1u << b));
}

static void test_ctz64(void) {
    ASSERT_EQ_INT(64, bits_ctz64(0));
    ASSERT_EQ_INT(0,  bits_ctz64(1));
    ASSERT_EQ_INT(63, bits_ctz64(0x8000000000000000ull));
    ASSERT_EQ_INT(32, bits_ctz64(0x100000000ull));
    for (int b = 0; b < 64; b++)
        ASSERT_EQ_INT(b, bits_ctz64(1ull << b));
}

/* ---- popcount -------------------------------------------------- */

static void test_popcount(void) {
    ASSERT_EQ_INT(0,  bits_popcount32(0));
    ASSERT_EQ_INT(32, bits_popcount32(0xFFFFFFFFu));
    ASSERT_EQ_INT(1,  bits_popcount32(0x80000000u));
    ASSERT_EQ_INT(16, bits_popcount32(0xAAAAAAAAu));
    ASSERT_EQ_INT(16, bits_popcount32(0x55555555u));
    ASSERT_EQ_INT(16, bits_popcount32(0x0F0F0F0Fu)); /* 4 bytes x 4 bits */
    ASSERT_EQ_INT(8,  bits_popcount32(0x000000FFu));

    ASSERT_EQ_INT(0,  bits_popcount64(0));
    ASSERT_EQ_INT(64, bits_popcount64(0xFFFFFFFFFFFFFFFFull));
    ASSERT_EQ_INT(32, bits_popcount64(0xAAAAAAAAAAAAAAAAull));
    ASSERT_EQ_INT(33, bits_popcount64(0xFFFFFFFF80000000ull)); /* 32 + 1 */
}

/* ---- a tiny bitmap-scan use, the way the allocator will use it -- */

static void test_find_first_free_word_scan(void) {
    /* 1 = free; find the lowest free index across a 2-word bitmap. */
    uint32_t words[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
    /* allocate bits 0..47 by clearing them */
    for (int i = 0; i < 48; i++) words[i >> 5] &= ~(1u << (i & 31));
    /* first free should be index 48 (word 1, bit 16) */
    int found = -1;
    for (int w = 0; w < 2 && found < 0; w++)
        if (words[w] != 0) found = w * 32 + bits_ctz32(words[w]);
    ASSERT_EQ_INT(48, found);
}

int main(void) {
    RUN(test_clz32);
    RUN(test_clz64);
    RUN(test_ctz32);
    RUN(test_ctz64);
    RUN(test_popcount);
    RUN(test_find_first_free_word_scan);
    return TEST_SUITE_RESULT();
}
