/* Tests for slab_stack allocator.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "memory/slab_stack.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Big enough buffer for any reasonable test config. */
static uint8_t g_region[64 * 1024];

/* ============================================================
 *  Helpers
 * ============================================================ */

static SlabConfig small_config(void) {
    /* Small allocator: a handful of bins, modest counts. */
    SlabConfig cfg = {0};
    cfg.bucket_counts[0] = 4;   /* 32B   */
    cfg.bucket_counts[1] = 4;   /* 64B   */
    cfg.bucket_counts[2] = 4;   /* 128B  */
    cfg.bucket_counts[3] = 2;   /* 256B  */
    return cfg;
}

/* ============================================================
 *  Init / sizing
 * ============================================================ */

static void test_init_basic(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    SlabResult r = slab_init(&a, g_region, sizeof(g_region), &cfg,
                              slab_null_locker);
    ASSERT_EQ_INT(SLAB_OK, (int)r);
    /* Total managed: 4*32 + 4*64 + 4*128 + 2*256 = 128+256+512+512 = 1408. */
    ASSERT_EQ_INT(1408, (int)a.total_bytes_managed);
    ASSERT_EQ_INT(0, (int)a.total_bytes_in_use);
    slab_destroy(&a);
}

static void test_init_null_handle_fails(void) {
    SlabConfig cfg = small_config();
    SlabResult r = slab_init(NULL, g_region, sizeof(g_region), &cfg,
                              slab_null_locker);
    ASSERT_EQ_INT(SLAB_ERR_INVALID_ARG, (int)r);
}

static void test_init_null_region_fails(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    SlabResult r = slab_init(&a, NULL, 1024, &cfg, slab_null_locker);
    ASSERT_EQ_INT(SLAB_ERR_INVALID_ARG, (int)r);
}

static void test_init_null_config_fails(void) {
    SlabAllocator a;
    SlabResult r = slab_init(&a, g_region, sizeof(g_region), NULL,
                              slab_null_locker);
    ASSERT_EQ_INT(SLAB_ERR_INVALID_ARG, (int)r);
}

static void test_init_region_too_small_fails(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    /* 100 bytes is way too small. */
    SlabResult r = slab_init(&a, g_region, 100, &cfg, slab_null_locker);
    ASSERT_EQ_INT(SLAB_ERR_NO_SPACE, (int)r);
}

static void test_required_bytes_matches_actual(void) {
    SlabConfig cfg = small_config();
    size_t need = slab_required_bytes(&cfg);
    /* Should be enough for the init to succeed at exactly that size. */
    SlabAllocator a;
    SlabResult r = slab_init(&a, g_region, need, &cfg, slab_null_locker);
    ASSERT_EQ_INT(SLAB_OK, (int)r);
}

static void test_required_bytes_null_returns_zero(void) {
    ASSERT_EQ_INT(0, (int)slab_required_bytes(NULL));
}

static void test_empty_config_works(void) {
    /* All bucket counts zero. */
    SlabConfig cfg = {0};
    SlabAllocator a;
    SlabResult r = slab_init(&a, g_region, sizeof(g_region), &cfg,
                              slab_null_locker);
    ASSERT_EQ_INT(SLAB_OK, (int)r);
    ASSERT_EQ_INT(0, (int)a.total_bytes_managed);
    /* Any alloc should fail since no bins exist. */
    void *p = slab_alloc(&a, 16);
    ASSERT_NULL(p);
}

/* ============================================================
 *  Basic allocation
 * ============================================================ */

static void test_alloc_returns_nonnull(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p = slab_alloc(&a, 16);
    ASSERT_NOT_NULL(p);
    slab_destroy(&a);
}

static void test_alloc_rounds_up_to_smallest_bin(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    /* Asking for 1 byte should take a 32B block (bin 0). */
    void *p = slab_alloc(&a, 1);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(1, (int)a.bins[0].blocks_in_use);
    ASSERT_EQ_INT(0, (int)a.bins[1].blocks_in_use);
    slab_destroy(&a);
}

static void test_alloc_picks_correct_bin(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    /* Asking for 50 bytes -> 50 + 8 header = 58, fits in 64B bin (bin 1). */
    void *p = slab_alloc(&a, 50);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(1, (int)a.bins[1].blocks_in_use);
    ASSERT_EQ_INT(0, (int)a.bins[0].blocks_in_use);
    slab_destroy(&a);
}

static void test_alloc_zero_returns_null(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);
    void *p = slab_alloc(&a, 0);
    ASSERT_NULL(p);
    slab_destroy(&a);
}

static void test_alloc_too_big_returns_null(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);
    /* 2 MB > 1 MB max */
    void *p = slab_alloc(&a, 2 * 1024 * 1024);
    ASSERT_NULL(p);
    ASSERT_EQ_INT(1, (int)a.failed_alloc_count);
    slab_destroy(&a);
}

static void test_alloc_no_fallback_to_larger_bin(void) {
    /* Only big bins configured; small allocs should fail. */
    SlabConfig cfg = {0};
    cfg.bucket_counts[5] = 2;   /* 1KB bin only */
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    /* 32-byte alloc would fit in bin 0, but bin 0 doesn't exist.
     * No fallback to bin 5 — must fail. */
    void *p = slab_alloc(&a, 16);
    ASSERT_NULL(p);
    slab_destroy(&a);
}

static void test_alloc_returns_distinct_pointers(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p1 = slab_alloc(&a, 16);
    void *p2 = slab_alloc(&a, 16);
    void *p3 = slab_alloc(&a, 16);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT_NOT_NULL(p3);
    ASSERT(p1 != p2);
    ASSERT(p2 != p3);
    ASSERT(p1 != p3);
    slab_destroy(&a);
}

static void test_alloc_pointers_8_byte_aligned(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    for (int i = 0; i < 4; i++) {
        void *p = slab_alloc(&a, 32);
        ASSERT_NOT_NULL(p);
        ASSERT_EQ_INT(0, (int)((uintptr_t)p & 0x7));
    }
    slab_destroy(&a);
}

/* ============================================================
 *  Bin exhaustion
 * ============================================================ */

static void test_alloc_exhausts_bin(void) {
    SlabConfig cfg = {0};
    cfg.bucket_counts[0] = 3;   /* exactly 3 blocks in bin 0 */
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p1 = slab_alloc(&a, 16);
    void *p2 = slab_alloc(&a, 16);
    void *p3 = slab_alloc(&a, 16);
    ASSERT_NOT_NULL(p1);
    ASSERT_NOT_NULL(p2);
    ASSERT_NOT_NULL(p3);
    /* 4th alloc should fail. */
    void *p4 = slab_alloc(&a, 16);
    ASSERT_NULL(p4);
    ASSERT_EQ_INT(1, (int)a.failed_alloc_count);
    slab_destroy(&a);
}

static void test_alloc_after_free_succeeds(void) {
    SlabConfig cfg = {0};
    cfg.bucket_counts[0] = 1;
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p1 = slab_alloc(&a, 16);
    ASSERT_NOT_NULL(p1);
    void *p2 = slab_alloc(&a, 16);
    ASSERT_NULL(p2);   /* exhausted */

    slab_free(&a, p1);
    void *p3 = slab_alloc(&a, 16);
    ASSERT_NOT_NULL(p3);
    slab_destroy(&a);
}

/* ============================================================
 *  Free
 * ============================================================ */

static void test_free_null_safe(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);
    SlabResult r = slab_free(&a, NULL);
    ASSERT_EQ_INT(SLAB_OK, (int)r);
    slab_destroy(&a);
}

static void test_free_updates_stats(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p = slab_alloc(&a, 16);
    ASSERT_EQ_INT(1, (int)a.bins[0].blocks_in_use);
    ASSERT_EQ_INT(32, (int)a.total_bytes_in_use);

    slab_free(&a, p);
    ASSERT_EQ_INT(0, (int)a.bins[0].blocks_in_use);
    ASSERT_EQ_INT(0, (int)a.total_bytes_in_use);
    ASSERT_EQ_INT(1, (int)a.free_count);
    slab_destroy(&a);
}

#ifndef SLAB_NO_MAGIC
static void test_double_free_detected(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p = slab_alloc(&a, 16);
    SlabResult r1 = slab_free(&a, p);
    ASSERT_EQ_INT(SLAB_OK, (int)r1);

    SlabResult r2 = slab_free(&a, p);
    ASSERT_EQ_INT(SLAB_ERR_DOUBLE_FREE, (int)r2);
    slab_destroy(&a);
}

static void test_free_foreign_pointer_rejected(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    int local_var = 42;
    SlabResult r = slab_free(&a, &local_var);
    ASSERT_EQ_INT(SLAB_ERR_FOREIGN_POINTER, (int)r);
    slab_destroy(&a);
}
#endif

/* ============================================================
 *  Realloc
 * ============================================================ */

static void test_realloc_null_is_alloc(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p = slab_realloc(&a, NULL, 20);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(32, (int)slab_block_size(&a, p));  /* 20 -> 32B bin */
    slab_destroy(&a);
}

static void test_realloc_zero_is_free(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p = slab_alloc(&a, 20);
    ASSERT_NOT_NULL(p);
    void *q = slab_realloc(&a, p, 0);
    ASSERT_NULL(q);
    /* block returned to its bin */
    ASSERT_EQ_INT(0, (int)a.total_bytes_in_use);
    slab_destroy(&a);
}

static void test_realloc_within_bin_keeps_pointer(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p = slab_alloc(&a, 20);          /* 32B bin */
    memset(p, 0xAB, 20);
    void *p2 = slab_realloc(&a, p, 30);    /* still <= 32 */
    ASSERT_EQ_PTR(p, p2);                  /* no move */
    ASSERT_EQ_INT(0xAB, ((uint8_t *)p2)[0]);
    ASSERT_EQ_INT(0xAB, ((uint8_t *)p2)[19]);
    slab_destroy(&a);
}

static void test_realloc_shrink_keeps_pointer(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p = slab_alloc(&a, 30);          /* 32B bin */
    void *p2 = slab_realloc(&a, p, 8);     /* shrink, still in 32B bin */
    ASSERT_EQ_PTR(p, p2);                  /* shrink stays in place */
    slab_destroy(&a);
}

static void test_realloc_grow_past_bin_moves_and_copies(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p = slab_alloc(&a, 20);          /* 32B bin */
    memset(p, 0xAB, 20);
    void *p2 = slab_realloc(&a, p, 100);   /* needs 128B bin */
    ASSERT_NOT_NULL(p2);
    ASSERT(p2 != p);                       /* moved */
    ASSERT_EQ_INT(128, (int)slab_block_size(&a, p2));
    /* old payload preserved (copied the old 32B capacity) */
    ASSERT_EQ_INT(0xAB, ((uint8_t *)p2)[0]);
    ASSERT_EQ_INT(0xAB, ((uint8_t *)p2)[19]);
    slab_destroy(&a);
}

static void test_realloc_grow_failure_leaves_original_intact(void) {
    /* tiny config: exactly two 128B blocks, so we can exhaust them */
    SlabConfig cfg = {0};
    cfg.bucket_counts[1] = 1;   /* one 64B  */
    cfg.bucket_counts[2] = 2;   /* two 128B */
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *x = slab_alloc(&a, 100);   /* take 128B #1 */
    void *y = slab_alloc(&a, 100);   /* take 128B #2 — bin now full */
    ASSERT_NOT_NULL(x);
    ASSERT_NOT_NULL(y);

    void *small = slab_alloc(&a, 50); /* a 64B block */
    memset(small, 0xCD, 50);
    void *grown = slab_realloc(&a, small, 100); /* needs 128B: exhausted */
    ASSERT_NULL(grown);                          /* fails */
    /* original still valid + data intact */
    ASSERT_EQ_INT(64, (int)slab_block_size(&a, small));
    ASSERT_EQ_INT(0xCD, ((uint8_t *)small)[0]);
    ASSERT_EQ_INT(0xCD, ((uint8_t *)small)[49]);
    slab_destroy(&a);
}

static void test_realloc_invalid_pointer_fails(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    int local = 7;
    void *r = slab_realloc(&a, &local, 64);  /* foreign pointer */
    ASSERT_NULL(r);
    slab_destroy(&a);
}

/* ============================================================
 *  Stats: peak tracking
 * ============================================================ */

static void test_peak_bytes_in_use(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p1 = slab_alloc(&a, 16);  /* +32 */
    void *p2 = slab_alloc(&a, 16);  /* +32 = 64 total */
    void *p3 = slab_alloc(&a, 100); /* +128 = 192 total */
    ASSERT_EQ_INT(192, (int)a.peak_bytes_in_use);

    slab_free(&a, p2);
    ASSERT_EQ_INT(192, (int)a.peak_bytes_in_use); /* peak should persist */
    ASSERT_EQ_INT(160, (int)a.total_bytes_in_use);

    /* Allocate more, exceeding old peak. */
    void *p4 = slab_alloc(&a, 200);  /* +256 = 416 */
    ASSERT_EQ_INT(416, (int)a.peak_bytes_in_use);

    slab_free(&a, p1);
    slab_free(&a, p3);
    slab_free(&a, p4);
    slab_destroy(&a);
}

static void test_per_bin_peak(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    /* Allocate 3 from bin 0. Free 1. Peak should be 3. */
    void *p1 = slab_alloc(&a, 16);
    void *p2 = slab_alloc(&a, 16);
    void *p3 = slab_alloc(&a, 16);
    ASSERT_EQ_INT(3, (int)a.bins[0].peak_in_use);
    slab_free(&a, p2);
    ASSERT_EQ_INT(3, (int)a.bins[0].peak_in_use);
    ASSERT_EQ_INT(2, (int)a.bins[0].blocks_in_use);

    (void)p1; (void)p3;
    slab_destroy(&a);
}

static void test_counters(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *p1 = slab_alloc(&a, 16);
    void *p2 = slab_alloc(&a, 16);
    slab_free(&a, p1);
    slab_alloc(&a, 99999999);   /* should fail */

    ASSERT_EQ_INT(2, (int)a.alloc_count);
    ASSERT_EQ_INT(1, (int)a.free_count);
    ASSERT(a.failed_alloc_count >= 1);

    (void)p2;
    slab_destroy(&a);
}

/* ============================================================
 *  Content integrity (allocations don't trample each other)
 * ============================================================ */

static void test_writes_dont_overlap(void) {
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    uint8_t *p1 = slab_alloc(&a, 32);
    uint8_t *p2 = slab_alloc(&a, 32);
    uint8_t *p3 = slab_alloc(&a, 32);

    memset(p1, 0xAA, 32);
    memset(p2, 0xBB, 32);
    memset(p3, 0xCC, 32);

    for (int i = 0; i < 32; i++) {
        ASSERT_EQ_INT(0xAA, p1[i]);
        ASSERT_EQ_INT(0xBB, p2[i]);
        ASSERT_EQ_INT(0xCC, p3[i]);
    }
    slab_destroy(&a);
}

static void test_full_block_size_writable(void) {
    /* Verify we can actually use the full bin size, not just the
     * requested amount. */
    SlabConfig cfg = small_config();
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    /* Request 1 byte from bin 0 (32-byte block). We should be able
     * to write 32 bytes — that's what the bin gives us. */
    uint8_t *p = slab_alloc(&a, 1);
    memset(p, 0xEE, 32);   /* should not corrupt anything */
    /* Allocate another and verify the first is intact. */
    uint8_t *p2 = slab_alloc(&a, 1);
    memset(p2, 0xFF, 32);
    for (int i = 0; i < 32; i++) {
        ASSERT_EQ_INT(0xEE, p[i]);
        ASSERT_EQ_INT(0xFF, p2[i]);
    }
    slab_destroy(&a);
}

/* ============================================================
 *  Multi-bin stress
 * ============================================================ */

static void test_alloc_free_many(void) {
    /* Many allocations across multiple bins. */
    SlabConfig cfg = {0};
    cfg.bucket_counts[0] = 20;
    cfg.bucket_counts[1] = 10;
    cfg.bucket_counts[2] = 5;
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    void *ptrs[35];
    int idx = 0;
    for (int i = 0; i < 20; i++) ptrs[idx++] = slab_alloc(&a, 16);
    for (int i = 0; i < 10; i++) ptrs[idx++] = slab_alloc(&a, 50);
    for (int i = 0; i <  5; i++) ptrs[idx++] = slab_alloc(&a, 100);

    for (int i = 0; i < 35; i++) ASSERT_NOT_NULL(ptrs[i]);

    /* Free them all. */
    for (int i = 0; i < 35; i++) {
        SlabResult r = slab_free(&a, ptrs[i]);
        ASSERT_EQ_INT(SLAB_OK, (int)r);
    }
    ASSERT_EQ_INT(0, (int)a.total_bytes_in_use);
    slab_destroy(&a);
}

static void test_alloc_free_interleaved(void) {
    SlabConfig cfg = {0};
    cfg.bucket_counts[0] = 4;
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    /* Cycle through alloc/free to exercise the stack push/pop. */
    void *p1, *p2, *p3, *p4;
    for (int round = 0; round < 100; round++) {
        p1 = slab_alloc(&a, 16);
        p2 = slab_alloc(&a, 16);
        p3 = slab_alloc(&a, 16);
        p4 = slab_alloc(&a, 16);
        ASSERT_NOT_NULL(p1);
        ASSERT_NOT_NULL(p2);
        ASSERT_NOT_NULL(p3);
        ASSERT_NOT_NULL(p4);
        slab_free(&a, p2);
        slab_free(&a, p4);
        slab_free(&a, p1);
        slab_free(&a, p3);
    }
    ASSERT_EQ_INT(0, (int)a.bins[0].blocks_in_use);
    slab_destroy(&a);
}

/* ============================================================
 *  Locker custom callback
 * ============================================================ */

static int g_lock_count = 0;
static int g_unlock_count = 0;

static uintptr_t counting_lock(void *ctx) {
    (void)ctx;
    g_lock_count++;
    return 0xDEAD0000u + g_lock_count;
}
static void counting_unlock(void *ctx, uintptr_t saved) {
    (void)ctx;
    /* Verify the saved value round-trips. */
    if ((saved & 0xFFFF0000u) != 0xDEAD0000u) {
        /* won't fail the test directly, just don't increment */
        return;
    }
    g_unlock_count++;
}

static void test_locker_called(void) {
    g_lock_count = 0;
    g_unlock_count = 0;

    SlabConfig cfg = small_config();
    SlabAllocator a;
    SlabLocker locker = { counting_lock, counting_unlock, NULL };
    slab_init(&a, g_region, sizeof(g_region), &cfg, locker);

    void *p = slab_alloc(&a, 16);
    slab_free(&a, p);

    /* Each alloc and free should have invoked lock/unlock once. */
    ASSERT(g_lock_count >= 2);
    ASSERT_EQ_INT(g_lock_count, g_unlock_count);
    slab_destroy(&a);
}

/* ============================================================
 *  Integration: real-world use pattern
 * ============================================================ */

static void test_typical_use_pattern(void) {
    /* Imagine a small allocator that services many small structs.
     * Item is 32 bytes; with 8B header that's 40 total, which
     * lands in bin 1 (64-byte bin). */
    typedef struct { int id; char name[28]; } Item;

    SlabConfig cfg = {0};
    cfg.bucket_counts[1] = 16;  /* 64-byte bin */
    SlabAllocator a;
    slab_init(&a, g_region, sizeof(g_region), &cfg, slab_null_locker);

    Item *items[10];
    for (int i = 0; i < 10; i++) {
        items[i] = slab_alloc(&a, sizeof(Item));
        ASSERT_NOT_NULL(items[i]);
        items[i]->id = i;
        snprintf(items[i]->name, sizeof(items[i]->name), "item_%d", i);
    }

    /* Verify everything intact. */
    for (int i = 0; i < 10; i++) {
        ASSERT_EQ_INT(i, items[i]->id);
        char expected[28];
        snprintf(expected, sizeof(expected), "item_%d", i);
        ASSERT_EQ_INT(0, strcmp(items[i]->name, expected));
    }

    /* Free them all. */
    for (int i = 0; i < 10; i++) {
        slab_free(&a, items[i]);
    }
    ASSERT_EQ_INT(0, (int)a.total_bytes_in_use);
    ASSERT(a.peak_bytes_in_use >= 10 * 64);
    slab_destroy(&a);
}

/* ============================================================
 *  Main
 * ============================================================ */

int main(void) {
    TEST_SUITE("slab_stack");

    /* Init / sizing */
    RUN(test_init_basic);
    RUN(test_init_null_handle_fails);
    RUN(test_init_null_region_fails);
    RUN(test_init_null_config_fails);
    RUN(test_init_region_too_small_fails);
    RUN(test_required_bytes_matches_actual);
    RUN(test_required_bytes_null_returns_zero);
    RUN(test_empty_config_works);

    /* Alloc */
    RUN(test_alloc_returns_nonnull);
    RUN(test_alloc_rounds_up_to_smallest_bin);
    RUN(test_alloc_picks_correct_bin);
    RUN(test_alloc_zero_returns_null);
    RUN(test_alloc_too_big_returns_null);
    RUN(test_alloc_no_fallback_to_larger_bin);
    RUN(test_alloc_returns_distinct_pointers);
    RUN(test_alloc_pointers_8_byte_aligned);

    /* Exhaustion */
    RUN(test_alloc_exhausts_bin);
    RUN(test_alloc_after_free_succeeds);

    /* Free */
    RUN(test_free_null_safe);
    RUN(test_free_updates_stats);
#ifndef SLAB_NO_MAGIC
    RUN(test_double_free_detected);
    RUN(test_free_foreign_pointer_rejected);
#endif

    /* Realloc */
    RUN(test_realloc_null_is_alloc);
    RUN(test_realloc_zero_is_free);
    RUN(test_realloc_within_bin_keeps_pointer);
    RUN(test_realloc_shrink_keeps_pointer);
    RUN(test_realloc_grow_past_bin_moves_and_copies);
    RUN(test_realloc_grow_failure_leaves_original_intact);
    RUN(test_realloc_invalid_pointer_fails);

    /* Stats */
    RUN(test_peak_bytes_in_use);
    RUN(test_per_bin_peak);
    RUN(test_counters);

    /* Content integrity */
    RUN(test_writes_dont_overlap);
    RUN(test_full_block_size_writable);

    /* Stress */
    RUN(test_alloc_free_many);
    RUN(test_alloc_free_interleaved);

    /* Locker */
    RUN(test_locker_called);

    /* Integration */
    RUN(test_typical_use_pattern);

    return TEST_SUITE_RESULT();
}
