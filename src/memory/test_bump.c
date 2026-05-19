/* Tests for bump allocator (caller-provided-region path).
 *
 * The slab-backed path (bump_init_from_slab) is exercised when
 * slab_stack is built; for now we test the standalone path. */

#include "test_runner.h"
#include "memory/bump.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Slab function stubs. The bump module references these symbols
 * for its bump_init_from_slab / bump_destroy paths, but the tests
 * here never call those, so the stubs are never executed. They
 * exist only to satisfy the linker. When slab_stack.c is built
 * alongside this in a real project, those definitions take
 * precedence (or rather, the linker chooses one — these stubs are
 * only present in this test executable). */
struct SlabAllocator;
void *slab_alloc(struct SlabAllocator *s, size_t n) {
    (void)s; (void)n;
    return NULL;   /* never called in these tests */
}
void slab_free(struct SlabAllocator *s, void *p) {
    (void)s; (void)p;
}

/* Working buffer for most tests. */
static uint8_t g_buf[4096];

/* ============================================================
 *  Init / lifecycle
 * ============================================================ */

static void test_init_basic(void) {
    BumpAllocator b;
    BumpResult r = bump_init(&b, g_buf, sizeof(g_buf));
    ASSERT_EQ_INT(BUMP_OK, (int)r);
    ASSERT_EQ_INT((int)sizeof(g_buf), (int)bump_capacity(&b));
    ASSERT_EQ_INT(0, (int)bump_used(&b));
    ASSERT_EQ_INT((int)sizeof(g_buf), (int)bump_remaining(&b));
}

static void test_init_null_handle_fails(void) {
    BumpResult r = bump_init(NULL, g_buf, sizeof(g_buf));
    ASSERT_EQ_INT(BUMP_ERR_INVALID_ARG, (int)r);
}

static void test_init_null_region_fails(void) {
    BumpAllocator b;
    BumpResult r = bump_init(&b, NULL, 1024);
    ASSERT_EQ_INT(BUMP_ERR_INVALID_ARG, (int)r);
}

static void test_init_zero_size_fails(void) {
    BumpAllocator b;
    BumpResult r = bump_init(&b, g_buf, 0);
    ASSERT_EQ_INT(BUMP_ERR_INVALID_ARG, (int)r);
}

static void test_destroy_null_safe(void) {
    bump_destroy(NULL);   /* must not crash */
}

static void test_destroy_zeros_caller_region_fields(void) {
    /* Caller-provided region should NOT be freed by destroy.
     * The struct fields should be cleared so reuse-after-destroy
     * is detectable. */
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));
    bump_alloc(&b, 32);

    bump_destroy(&b);
    /* After destroy, capacity and used should be 0 (struct cleared). */
    ASSERT_EQ_INT(0, (int)bump_capacity(&b));
    ASSERT_EQ_INT(0, (int)bump_used(&b));

    /* The original buffer should be unmodified at the user level;
     * we wrote 32 bytes of garbage but the caller still owns the
     * buffer and didn't lose anything. */
}

/* ============================================================
 *  Basic allocation
 * ============================================================ */

static void test_alloc_single(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));

    void *p = bump_alloc(&b, 64);
    ASSERT_NOT_NULL(p);
    ASSERT(bump_used(&b) >= 64);
    ASSERT(bump_used(&b) < 64 + BUMP_DEFAULT_ALIGNMENT);
}

static void test_alloc_advances_position(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));

    void *p1 = bump_alloc(&b, 100);
    void *p2 = bump_alloc(&b, 100);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT(p2 > p1);
    /* p2 should be at least 100 bytes past p1. */
    ASSERT((uintptr_t)p2 - (uintptr_t)p1 >= 100);
}

static void test_alloc_zero_size_returns_null(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));
    void *p = bump_alloc(&b, 0);
    ASSERT_NULL(p);
}

static void test_alloc_too_big_returns_null(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, 256);
    void *p = bump_alloc(&b, 1000);
    ASSERT_NULL(p);
    /* Failed alloc shouldn't have changed the position. */
    ASSERT_EQ_INT(0, (int)bump_used(&b));
}

static void test_alloc_fills_arena(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, 256);
    /* Fill exactly. */
    void *p = bump_alloc_aligned(&b, 256, 1);
    ASSERT_NOT_NULL(p);
    /* Should be out of room now. */
    void *p2 = bump_alloc_aligned(&b, 1, 1);
    ASSERT_NULL(p2);
}

static void test_alloc_just_past_capacity_fails(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, 256);
    /* 257 doesn't fit. */
    void *p = bump_alloc_aligned(&b, 257, 1);
    ASSERT_NULL(p);
    ASSERT_EQ_INT(0, (int)bump_used(&b));
}

/* ============================================================
 *  Alignment
 * ============================================================ */

static void test_default_alignment_is_8(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));

    /* Each bump_alloc should give an 8-byte-aligned address. */
    for (int i = 0; i < 10; i++) {
        void *p = bump_alloc(&b, 7);   /* odd size */
        ASSERT_NOT_NULL(p);
        ASSERT_EQ_INT(0, (int)((uintptr_t)p & 0x7));
    }
}

static void test_alloc_aligned_to_specific_boundary(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));

    /* Use 1-byte alignment first to advance to odd offset. */
    bump_alloc_aligned(&b, 1, 1);

    /* Now request 16-byte alignment. */
    void *p = bump_alloc_aligned(&b, 32, 16);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(0, (int)((uintptr_t)p & 0xF));
}

static void test_alloc_aligned_32_byte_boundary(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));
    void *p = bump_alloc_aligned(&b, 100, 32);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(0, (int)((uintptr_t)p & 0x1F));
}

static void test_non_power_of_two_alignment_rejected(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));
    void *p = bump_alloc_aligned(&b, 32, 7);   /* 7 is not pow2 */
    ASSERT_NULL(p);
    void *q = bump_alloc_aligned(&b, 32, 12);  /* 12 is not pow2 */
    ASSERT_NULL(q);
}

/* ============================================================
 *  Reset behavior
 * ============================================================ */

static void test_reset_returns_position_to_zero(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));
    bump_alloc(&b, 100);
    bump_alloc(&b, 200);
    ASSERT(bump_used(&b) >= 300);

    bump_reset(&b);
    ASSERT_EQ_INT(0, (int)bump_used(&b));
}

static void test_reset_allows_full_reuse(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, 256);
    /* Fill it. */
    while (bump_alloc_aligned(&b, 1, 1)) {}
    /* Out of space now. */
    ASSERT_NULL(bump_alloc_aligned(&b, 1, 1));

    bump_reset(&b);
    /* Should be allocatable again. */
    void *p = bump_alloc_aligned(&b, 1, 1);
    ASSERT_NOT_NULL(p);
}

static void test_reset_clears_peak(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));
    bump_alloc(&b, 500);
    ASSERT(bump_peak(&b) >= 500);
    bump_reset(&b);
    ASSERT_EQ_INT(0, (int)bump_peak(&b));
}

/* ============================================================
 *  Peak tracking
 * ============================================================ */

static void test_peak_tracks_high_water_mark(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));

    bump_alloc(&b, 100);
    size_t peak1 = bump_peak(&b);
    ASSERT(peak1 >= 100);

    bump_alloc(&b, 500);
    size_t peak2 = bump_peak(&b);
    ASSERT(peak2 > peak1);
    ASSERT(peak2 >= 600);
}

static void test_peak_persists_until_reset(void) {
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));

    bump_alloc(&b, 1000);
    size_t peak_after = bump_peak(&b);
    ASSERT(peak_after >= 1000);

    /* Peak shouldn't change just because no new alloc happened. */
    ASSERT_EQ_INT((int)peak_after, (int)bump_peak(&b));
}

/* ============================================================
 *  Content integrity
 * ============================================================ */

static void test_allocations_dont_overlap(void) {
    /* Write distinct patterns to each allocation and verify they
     * don't trample each other. */
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));

    void *p1 = bump_alloc(&b, 100);
    void *p2 = bump_alloc(&b, 100);
    void *p3 = bump_alloc(&b, 100);

    memset(p1, 0xAA, 100);
    memset(p2, 0xBB, 100);
    memset(p3, 0xCC, 100);

    /* Verify each region still has its own pattern. */
    for (int i = 0; i < 100; i++) {
        ASSERT_EQ_INT(0xAA, ((uint8_t *)p1)[i]);
        ASSERT_EQ_INT(0xBB, ((uint8_t *)p2)[i]);
        ASSERT_EQ_INT(0xCC, ((uint8_t *)p3)[i]);
    }
}

static void test_allocation_within_region(void) {
    /* Sanity: returned pointers are within the region. */
    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));

    for (int i = 0; i < 20; i++) {
        void *p = bump_alloc(&b, 50);
        ASSERT_NOT_NULL(p);
        ASSERT((uint8_t *)p >= g_buf);
        ASSERT((uint8_t *)p + 50 <= g_buf + sizeof(g_buf));
    }
}

/* ============================================================
 *  Typical use case integration
 * ============================================================ */

static void test_typical_use_pattern(void) {
    /* Simulate "allocate a bunch of small structs, use them,
     * reset for next batch". */
    typedef struct { int a, b, c; char name[20]; } Item;

    BumpAllocator b;
    bump_init(&b, g_buf, sizeof(g_buf));

    /* Allocate 20 items. */
    Item *items[20];
    for (int i = 0; i < 20; i++) {
        items[i] = bump_alloc(&b, sizeof(Item));
        ASSERT_NOT_NULL(items[i]);
        items[i]->a = i;
        items[i]->b = i * 2;
        items[i]->c = i * 3;
        snprintf(items[i]->name, sizeof(items[i]->name), "item_%d", i);
    }

    /* Verify all items intact. */
    for (int i = 0; i < 20; i++) {
        ASSERT_EQ_INT(i,     items[i]->a);
        ASSERT_EQ_INT(i * 2, items[i]->b);
        ASSERT_EQ_INT(i * 3, items[i]->c);
        char expected[20];
        snprintf(expected, sizeof(expected), "item_%d", i);
        ASSERT_EQ_INT(0, strcmp(items[i]->name, expected));
    }

    /* Reset; now do another batch. */
    bump_reset(&b);

    Item *new_items[10];
    for (int i = 0; i < 10; i++) {
        new_items[i] = bump_alloc(&b, sizeof(Item));
        ASSERT_NOT_NULL(new_items[i]);
        new_items[i]->a = -i;
    }
    /* First new item should occupy the same address as the first
     * old item — reset returns to position 0. */
    ASSERT(new_items[0] == items[0]);
}

/* ============================================================
 *  Main
 * ============================================================ */

int main(void) {
    TEST_SUITE("bump");

    /* Init */
    RUN(test_init_basic);
    RUN(test_init_null_handle_fails);
    RUN(test_init_null_region_fails);
    RUN(test_init_zero_size_fails);
    RUN(test_destroy_null_safe);
    RUN(test_destroy_zeros_caller_region_fields);

    /* Allocation */
    RUN(test_alloc_single);
    RUN(test_alloc_advances_position);
    RUN(test_alloc_zero_size_returns_null);
    RUN(test_alloc_too_big_returns_null);
    RUN(test_alloc_fills_arena);
    RUN(test_alloc_just_past_capacity_fails);

    /* Alignment */
    RUN(test_default_alignment_is_8);
    RUN(test_alloc_aligned_to_specific_boundary);
    RUN(test_alloc_aligned_32_byte_boundary);
    RUN(test_non_power_of_two_alignment_rejected);

    /* Reset */
    RUN(test_reset_returns_position_to_zero);
    RUN(test_reset_allows_full_reuse);
    RUN(test_reset_clears_peak);

    /* Peak tracking */
    RUN(test_peak_tracks_high_water_mark);
    RUN(test_peak_persists_until_reset);

    /* Content integrity */
    RUN(test_allocations_dont_overlap);
    RUN(test_allocation_within_region);

    /* Integration */
    RUN(test_typical_use_pattern);

    return TEST_SUITE_RESULT();
}
