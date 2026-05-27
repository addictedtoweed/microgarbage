/* Integration test: bump allocator backed by slab_stack.
 *
 * Verifies that bump_init_from_slab correctly requests memory from
 * a slab allocator, allocates from the resulting region, and frees
 * the region back on destroy.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "memory/bump.h"
#include "memory/slab_stack.h"

#include <stdint.h>
#include <string.h>

static uint8_t g_region[64 * 1024];

static void test_bump_from_slab_basic(void) {
    /* Set up a slab with a 1KB bin (bin 5). */
    SlabConfig cfg = {0};
    cfg.bucket_counts[5] = 2;   /* two 1024-byte blocks */
    SlabAllocator slab;
    slab_init(&slab, g_region, sizeof(g_region), &cfg, slab_null_locker);

    /* Request 800 bytes (rounds up to bin 5's 1024). */
    BumpAllocator b;
    BumpResult r = bump_init_from_slab(&b, &slab, 800);
    ASSERT_EQ_INT(BUMP_OK, (int)r);
    ASSERT_EQ_INT(800, (int)bump_capacity(&b));

    /* Slab should report one block in use (the 1KB one). */
    ASSERT_EQ_INT(1, (int)slab.bins[5].blocks_in_use);

    /* Allocate from bump. */
    void *p1 = bump_alloc(&b, 100);
    ASSERT_NOT_NULL(p1);
    void *p2 = bump_alloc(&b, 200);
    ASSERT_NOT_NULL(p2);
    /* Pointer should be inside the bump region. */
    ASSERT(b.region != NULL);
    ASSERT((uint8_t *)p1 >= b.region);
    ASSERT((uint8_t *)p1 + 100 <= b.region + b.region_bytes);

    /* Destroy — should return the chunk to the slab. */
    bump_destroy(&b);
    ASSERT_EQ_INT(0, (int)slab.bins[5].blocks_in_use);

    slab_destroy(&slab);
}

static void test_bump_from_slab_exhausted(void) {
    /* Slab has only one 1KB block. First bump grabs it; second fails. */
    SlabConfig cfg = {0};
    cfg.bucket_counts[5] = 1;
    SlabAllocator slab;
    slab_init(&slab, g_region, sizeof(g_region), &cfg, slab_null_locker);

    BumpAllocator b1, b2;
    ASSERT_EQ_INT(BUMP_OK, (int)bump_init_from_slab(&b1, &slab, 800));
    ASSERT_EQ_INT(BUMP_ERR_NO_SPACE, (int)bump_init_from_slab(&b2, &slab, 800));

    /* After destroy of b1, slab has the block back. */
    bump_destroy(&b1);
    ASSERT_EQ_INT(BUMP_OK, (int)bump_init_from_slab(&b2, &slab, 800));

    bump_destroy(&b2);
    slab_destroy(&slab);
}

static void test_bump_from_slab_request_too_large(void) {
    /* Asking for more than the biggest configured bin → fail. */
    SlabConfig cfg = {0};
    cfg.bucket_counts[2] = 4;   /* 128-byte bin only */
    SlabAllocator slab;
    slab_init(&slab, g_region, sizeof(g_region), &cfg, slab_null_locker);

    /* Ask for 1000 bytes; slab can only do 128. */
    BumpAllocator b;
    BumpResult r = bump_init_from_slab(&b, &slab, 1000);
    ASSERT_EQ_INT(BUMP_ERR_NO_SPACE, (int)r);

    slab_destroy(&slab);
}

static void test_bump_from_slab_reset_doesnt_release_chunk(void) {
    /* bump_reset should NOT return the chunk to slab. */
    SlabConfig cfg = {0};
    cfg.bucket_counts[5] = 2;
    SlabAllocator slab;
    slab_init(&slab, g_region, sizeof(g_region), &cfg, slab_null_locker);

    BumpAllocator b;
    bump_init_from_slab(&b, &slab, 800);
    ASSERT_EQ_INT(1, (int)slab.bins[5].blocks_in_use);

    bump_alloc(&b, 100);
    bump_reset(&b);
    /* Slab should still see the chunk as in use. */
    ASSERT_EQ_INT(1, (int)slab.bins[5].blocks_in_use);

    bump_destroy(&b);
    ASSERT_EQ_INT(0, (int)slab.bins[5].blocks_in_use);
    slab_destroy(&slab);
}

static void test_bump_from_slab_many_cycles(void) {
    /* Create/destroy bump arena many times. Slab should return to
     * empty state after each cycle. */
    SlabConfig cfg = {0};
    cfg.bucket_counts[6] = 3;   /* 2KB bins */
    SlabAllocator slab;
    slab_init(&slab, g_region, sizeof(g_region), &cfg, slab_null_locker);

    for (int cycle = 0; cycle < 50; cycle++) {
        BumpAllocator b;
        ASSERT_EQ_INT(BUMP_OK, (int)bump_init_from_slab(&b, &slab, 1500));

        /* Use the arena. */
        for (int i = 0; i < 10; i++) {
            void *p = bump_alloc(&b, 100);
            ASSERT_NOT_NULL(p);
        }

        bump_destroy(&b);
    }

    /* All blocks freed; slab should report zero in use. */
    ASSERT_EQ_INT(0, (int)slab.bins[6].blocks_in_use);
    /* Counters should reflect all those alloc/free cycles. */
    ASSERT_EQ_INT(50, (int)slab.alloc_count);
    ASSERT_EQ_INT(50, (int)slab.free_count);

    slab_destroy(&slab);
}

int main(void) {
    TEST_SUITE("bump_on_slab integration");

    RUN(test_bump_from_slab_basic);
    RUN(test_bump_from_slab_exhausted);
    RUN(test_bump_from_slab_request_too_large);
    RUN(test_bump_from_slab_reset_doesnt_release_chunk);
    RUN(test_bump_from_slab_many_cycles);

    return TEST_SUITE_RESULT();
}
