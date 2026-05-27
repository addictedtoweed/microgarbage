/* ============================================================
 *  test_audio_pool.c — audio block pool allocator tests
 *
 *  Uses a small block size (set via -DAUDIO_POOL_BLOCK_SIZE=64 on
 *  the compile line) so multi-block chains, boundary-spanning
 *  reads/writes, and exhaustion are exercised without megabytes.
 *
 *  Build:
 *    cc -std=c11 -DAUDIO_POOL_BLOCK_SIZE=64 -DAUDIO_POOL_MAX_OBJECTS=8 \
 *       -Iinclude -o t \
 *       src/audio/tests/test_audio_pool.c src/audio/audio_pool.c
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_pool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

/* The tests assume a 64-byte block and a modest region. */
#define REGION_BLOCKS  16
#define REGION_BYTES   (REGION_BLOCKS * AUDIO_POOL_BLOCK_SIZE)

static AudioPool   g_pool;
static uint8_t    *g_region;

static void setup(void) {
    g_region = malloc(REGION_BYTES);
    AudioPoolResult r = audio_pool_init(&g_pool, g_region, REGION_BYTES);
    if (r != AUDIO_POOL_OK) { printf("  setup init failed: %d\n", r); exit(2); }
}
static void teardown(void) {
    audio_pool_destroy(&g_pool);
    free(g_region);
}

/* ---- geometry ---- */
static void test_geometry(void) {
    CHECK(audio_pool_block_count(&g_pool) == REGION_BLOCKS, "block_count = region/block");
    CHECK(audio_pool_free_blocks(&g_pool) == REGION_BLOCKS, "all blocks free at init");
}

/* ---- single-block alloc/free + refcount ---- */
static void test_alloc_single_block(void) {
    AudioObjHandle h;
    AudioPoolResult r = audio_pool_alloc(&g_pool, 32, 1, &h);   /* < 1 block */
    CHECK(r == AUDIO_POOL_OK, "alloc small object OK");
    CHECK(h != AUDIO_POOL_HANDLE_NONE, "handle is not NONE");
    CHECK(audio_pool_handle_valid(&g_pool, h), "handle valid");
    CHECK(audio_pool_free_blocks(&g_pool) == REGION_BLOCKS - 1, "one block consumed");
    CHECK(audio_pool_refcount(&g_pool, h) == 1, "initial refcount 1");
    CHECK(audio_pool_object_size(&g_pool, h) == 32, "object size recorded");

    bool freed = false;
    r = audio_pool_unref(&g_pool, h, &freed);
    CHECK(r == AUDIO_POOL_OK && freed, "unref frees at refcount 0");
    CHECK(audio_pool_free_blocks(&g_pool) == REGION_BLOCKS, "block reclaimed");
    CHECK(!audio_pool_handle_valid(&g_pool, h), "freed handle now invalid");
}

/* ---- multi-block chain alloc + boundary-spanning round-trip ---- */
static void test_multiblock_roundtrip(void) {
    /* 3.5 blocks worth -> ceil = 4 blocks */
    uint32_t size = AUDIO_POOL_BLOCK_SIZE * 3 + AUDIO_POOL_BLOCK_SIZE / 2;
    AudioObjHandle h;
    CHECK(audio_pool_alloc(&g_pool, size, 1, &h) == AUDIO_POOL_OK, "alloc multiblock OK");
    uint32_t want_blocks = (size + AUDIO_POOL_BLOCK_SIZE - 1) / AUDIO_POOL_BLOCK_SIZE;
    CHECK(audio_pool_free_blocks(&g_pool) == REGION_BLOCKS - want_blocks,
          "consumed ceil(size/block) blocks");

    /* Write a known pattern across the whole object, then read it
     * back — this crosses block boundaries (non-contiguous chain). */
    uint8_t *pattern = malloc(size);
    for (uint32_t i = 0; i < size; i++) pattern[i] = (uint8_t)(i * 31 + 7);

    uint32_t wrote = 0;
    audio_pool_write(&g_pool, h, 0, pattern, size, &wrote);
    CHECK(wrote == size, "write full object across blocks");

    uint8_t *readback = malloc(size);
    memset(readback, 0, size);
    uint32_t got = 0;
    audio_pool_read(&g_pool, h, 0, readback, size, &got);
    CHECK(got == size, "read full object across blocks");
    CHECK(memcmp(pattern, readback, size) == 0, "round-trip data intact across chain");

    /* Partial read straddling a block boundary. */
    uint32_t off = AUDIO_POOL_BLOCK_SIZE - 10;   /* spans boundary */
    uint32_t len = 40;
    uint8_t partial[64];
    uint32_t pc = 0;
    audio_pool_read(&g_pool, h, off, partial, len, &pc);
    CHECK(pc == len, "boundary-straddling partial read length");
    CHECK(memcmp(partial, pattern + off, len) == 0, "boundary-straddling data correct");

    /* Read clamps at object end. */
    uint8_t tail[64];
    uint32_t tc = 0;
    audio_pool_read(&g_pool, h, size - 5, tail, 100, &tc);
    CHECK(tc == 5, "read clamps at object end");

    free(pattern); free(readback);
    audio_pool_unref(&g_pool, h, NULL);
    CHECK(audio_pool_free_blocks(&g_pool) == REGION_BLOCKS, "all blocks reclaimed");
}

/* ---- refcount: shared object survives until last drop ---- */
static void test_refcount_sharing(void) {
    AudioObjHandle h;
    audio_pool_alloc(&g_pool, 10, 1, &h);
    CHECK(audio_pool_refcount(&g_pool, h) == 1, "refcount 1 after alloc");
    audio_pool_ref(&g_pool, h);    /* VM B takes a ref */
    audio_pool_ref(&g_pool, h);    /* a voice takes a ref */
    CHECK(audio_pool_refcount(&g_pool, h) == 3, "refcount 3 after two refs");

    bool freed = false;
    audio_pool_unref(&g_pool, h, &freed);
    CHECK(!freed && audio_pool_refcount(&g_pool, h) == 2, "still alive after 1 unref");
    audio_pool_unref(&g_pool, h, &freed);
    CHECK(!freed && audio_pool_refcount(&g_pool, h) == 1, "still alive after 2 unref");
    audio_pool_unref(&g_pool, h, &freed);
    CHECK(freed, "freed only on last unref");
    CHECK(audio_pool_free_blocks(&g_pool) == REGION_BLOCKS, "blocks back after last drop");
}

/* ---- stale handle detection (generation) ---- */
static void test_stale_handle(void) {
    AudioObjHandle h1;
    audio_pool_alloc(&g_pool, 10, 1, &h1);
    audio_pool_unref(&g_pool, h1, NULL);            /* frees slot, bumps gen */
    CHECK(!audio_pool_handle_valid(&g_pool, h1), "old handle invalid after free");

    /* Reallocate — likely reuses the same slot with a new gen. */
    AudioObjHandle h2;
    audio_pool_alloc(&g_pool, 10, 1, &h2);
    CHECK(h2 != h1, "reused slot yields a different handle (gen bump)");
    CHECK(audio_pool_handle_valid(&g_pool, h2), "new handle valid");
    CHECK(!audio_pool_handle_valid(&g_pool, h1), "stale handle still rejected");
    /* Operations on the stale handle fail cleanly. */
    CHECK(audio_pool_ref(&g_pool, h1) == AUDIO_POOL_ERR_BAD_HANDLE, "ref stale fails");
    CHECK(audio_pool_unref(&g_pool, h1, NULL) == AUDIO_POOL_ERR_BAD_HANDLE, "unref stale fails");
    audio_pool_unref(&g_pool, h2, NULL);
}

/* ---- exhaustion: ENOSPC when blocks run out ---- */
static void test_block_exhaustion(void) {
    /* Allocate one object that takes ALL blocks. */
    AudioObjHandle big;
    AudioPoolResult r = audio_pool_alloc(&g_pool, REGION_BYTES, 1, &big);
    CHECK(r == AUDIO_POOL_OK, "alloc consuming all blocks OK");
    CHECK(audio_pool_free_blocks(&g_pool) == 0, "zero free blocks");

    AudioObjHandle h;
    r = audio_pool_alloc(&g_pool, 1, 1, &h);
    CHECK(r == AUDIO_POOL_ERR_NO_SPACE, "alloc on full pool -> NO_SPACE");
    CHECK(h == AUDIO_POOL_HANDLE_NONE, "failed alloc yields NONE handle");

    audio_pool_unref(&g_pool, big, NULL);
    CHECK(audio_pool_free_blocks(&g_pool) == REGION_BLOCKS, "all back after free");
}

/* ---- descriptor table full -> NO_OBJECTS ---- */
static void test_object_table_full(void) {
    /* This test only makes sense when the descriptor table is the
     * binding limit, i.e. there are at least as many blocks as object
     * slots (each 1-block object). With the small test config
     * (MAX_OBJECTS=8, 16 blocks) that holds; at the production config
     * (256 objects, 16 blocks) blocks bind first, so skip — the
     * exhaustion path is covered by test_block_exhaustion. */
    if (REGION_BLOCKS < AUDIO_POOL_MAX_OBJECTS) {
        /* Can't fill the object table before blocks run out; nothing
         * meaningful to assert here for this config. */
        return;
    }

    AudioObjHandle hs[AUDIO_POOL_MAX_OBJECTS];
    uint32_t i;
    for (i = 0; i < AUDIO_POOL_MAX_OBJECTS; i++) {
        if (audio_pool_alloc(&g_pool, 1, 1, &hs[i]) != AUDIO_POOL_OK) break;
    }
    CHECK(i == AUDIO_POOL_MAX_OBJECTS, "filled the descriptor table");

    AudioObjHandle extra;
    AudioPoolResult r = audio_pool_alloc(&g_pool, 1, 1, &extra);
    CHECK(r == AUDIO_POOL_ERR_NO_OBJECTS, "alloc past table -> NO_OBJECTS");

    for (uint32_t k = 0; k < i; k++) audio_pool_unref(&g_pool, hs[k], NULL);
    CHECK(audio_pool_free_blocks(&g_pool) == REGION_BLOCKS, "blocks back after freeing all objects");
}

/* ---- VM orphan sweep ---- */
static void test_vm_sweep(void) {
    AudioObjHandle a1, a2, b1, shared;
    audio_pool_alloc(&g_pool, 10, 1, &a1);     /* vm 1 */
    audio_pool_alloc(&g_pool, 10, 1, &a2);     /* vm 1 */
    audio_pool_alloc(&g_pool, 10, 2, &b1);     /* vm 2 */
    audio_pool_alloc(&g_pool, 10, 1, &shared); /* vm 1 created */
    audio_pool_ref(&g_pool, shared);           /* but vm 2 also holds it (refcount 2) */

    uint32_t freed = 0;
    audio_pool_sweep_vm(&g_pool, 1, &freed);   /* vm 1 dies */
    /* a1, a2 (refcount 1, owner 1) -> freed. shared: owner 1, was
     * refcount 2, sweep drops 1 -> still alive. b1 untouched. */
    CHECK(freed == 2, "sweep frees vm 1's solely-owned objects");
    CHECK(!audio_pool_handle_valid(&g_pool, a1), "a1 freed");
    CHECK(!audio_pool_handle_valid(&g_pool, a2), "a2 freed");
    CHECK(audio_pool_handle_valid(&g_pool, b1), "vm 2's object survives");
    CHECK(audio_pool_handle_valid(&g_pool, shared), "shared object survives (vm2 ref)");
    CHECK(audio_pool_refcount(&g_pool, shared) == 1, "shared refcount dropped to 1");

    /* Now vm 2 cleans up. shared was created by vm1 (owner_vm=1), so
     * a sweep of vm 2 won't catch it — vm 2 must explicitly unref the
     * handle it holds. */
    audio_pool_unref(&g_pool, shared, NULL);
    audio_pool_unref(&g_pool, b1, NULL);
    CHECK(audio_pool_free_blocks(&g_pool) == REGION_BLOCKS, "all reclaimed after both VMs gone");
}

int main(void) {
    struct { const char *name; void (*fn)(void); } tests[] = {
        { "geometry",            test_geometry },
        { "alloc_single_block",  test_alloc_single_block },
        { "multiblock_roundtrip",test_multiblock_roundtrip },
        { "refcount_sharing",    test_refcount_sharing },
        { "stale_handle",        test_stale_handle },
        { "block_exhaustion",    test_block_exhaustion },
        { "object_table_full",   test_object_table_full },
        { "vm_sweep",            test_vm_sweep },
    };
    for (size_t i = 0; i < sizeof(tests)/sizeof(tests[0]); i++) {
        setup();
        tests[i].fn();
        teardown();
    }
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
