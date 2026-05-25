/* Tests for bitset (fixed-size bit set over caller word storage). */

#include "test_runner.h"
#include "containers/bitset.h"

#include <stdint.h>

static void test_sizing_macros_match_functions(void) {
    ASSERT_EQ_INT((int)BITSET_WORDS(70), (int)bitset_words(70));   /* 3 */
    ASSERT_EQ_INT((int)BITSET_BYTES(70), (int)bitset_bytes(70));   /* 12 */
    ASSERT_EQ_INT(1, (int)bitset_words(1));
    ASSERT_EQ_INT(1, (int)bitset_words(32));
    ASSERT_EQ_INT(2, (int)bitset_words(33));
}

static void test_clear_all_and_set_all(void) {
    uint32_t w[BITSET_WORDS(64)];
    Bitset bs; bitset_init(&bs, w, 64);

    bitset_clear_all(&bs);
    ASSERT_EQ_INT(0, (int)bitset_popcount(&bs));
    ASSERT(bitset_find_first_set(&bs) == BITSET_NPOS);
    ASSERT_EQ_INT(0, (int)bitset_find_first_clear(&bs));

    bitset_set_all(&bs);
    ASSERT_EQ_INT(64, (int)bitset_popcount(&bs));
    ASSERT_EQ_INT(0, (int)bitset_find_first_set(&bs));
    ASSERT(bitset_find_first_clear(&bs) == BITSET_NPOS);   /* full */
}

static void test_set_clear_test(void) {
    uint32_t w[BITSET_WORDS(100)];
    Bitset bs; bitset_init(&bs, w, 100);
    bitset_clear_all(&bs);

    bitset_set(&bs, 0);
    bitset_set(&bs, 42);
    bitset_set(&bs, 99);
    ASSERT(bitset_test(&bs, 0));
    ASSERT(bitset_test(&bs, 42));
    ASSERT(bitset_test(&bs, 99));
    ASSERT(!bitset_test(&bs, 1));
    ASSERT(!bitset_test(&bs, 41));
    ASSERT_EQ_INT(3, (int)bitset_popcount(&bs));

    bitset_clear(&bs, 42);
    ASSERT(!bitset_test(&bs, 42));
    ASSERT_EQ_INT(2, (int)bitset_popcount(&bs));

    /* out of range: ignored / reads false */
    bitset_set(&bs, 100);
    bitset_set(&bs, 5000);
    ASSERT(!bitset_test(&bs, 100));
    ASSERT_EQ_INT(2, (int)bitset_popcount(&bs));
}

/* Allocate-by-find pattern: find a clear bit, mark it used, repeat. */
static void test_allocate_pattern(void) {
    uint32_t w[BITSET_WORDS(10)];
    Bitset bs; bitset_init(&bs, w, 10);
    bitset_clear_all(&bs);

    for (int i = 0; i < 10; i++) {
        size_t slot = bitset_find_first_clear(&bs);
        ASSERT_EQ_INT(i, (int)slot);     /* lowest free, in order */
        bitset_set(&bs, slot);
    }
    ASSERT_EQ_INT(10, (int)bitset_popcount(&bs));
    ASSERT(bitset_find_first_clear(&bs) == BITSET_NPOS);   /* exhausted */

    /* free a middle slot -> next allocate returns exactly it (hint rewinds) */
    bitset_clear(&bs, 4);
    ASSERT_EQ_INT(4, (int)bitset_find_first_clear(&bs));
    bitset_set(&bs, 4);
    ASSERT(bitset_find_first_clear(&bs) == BITSET_NPOS);
}

/* Cross-word boundary find. */
static void test_cross_word(void) {
    uint32_t w[BITSET_WORDS(70)];
    Bitset bs; bitset_init(&bs, w, 70);
    bitset_clear_all(&bs);

    for (int i = 0; i < 64; i++) bitset_set(&bs, (size_t)i);  /* fill word 0+1 lower */
    ASSERT_EQ_INT(64, (int)bitset_find_first_clear(&bs));     /* first free is bit 64 */
    ASSERT_EQ_INT(64, (int)bitset_popcount(&bs));
}

/* Tail bits beyond nbits must never read as a free/used slot. */
static void test_tail_bits_ignored(void) {
    uint32_t w[BITSET_WORDS(40)];     /* 2 words; 24 tail bits in word 1 */
    Bitset bs; bitset_init(&bs, w, 40);

    bitset_set_all(&bs);
    ASSERT_EQ_INT(40, (int)bitset_popcount(&bs));            /* not 64 */
    ASSERT(bitset_find_first_clear(&bs) == BITSET_NPOS);     /* truly full, tail not "free" */

    bitset_clear_all(&bs);
    for (int i = 0; i < 40; i++) {
        size_t s = bitset_find_first_clear(&bs);
        ASSERT(s < 40);                                      /* never a tail index */
        bitset_set(&bs, s);
    }
    ASSERT_EQ_INT(40, (int)bitset_popcount(&bs));
    ASSERT(bitset_find_first_clear(&bs) == BITSET_NPOS);
}

int main(void) {
    RUN(test_sizing_macros_match_functions);
    RUN(test_clear_all_and_set_all);
    RUN(test_set_clear_test);
    RUN(test_allocate_pattern);
    RUN(test_cross_word);
    RUN(test_tail_bits_ignored);
    return TEST_SUITE_RESULT();
}
