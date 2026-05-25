/* Tests for avlhash (hash table with AVL-tree buckets, one node pool). */

#include "test_runner.h"
#include "containers/avlhash.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { uint32_t key; uint32_t val; } Ent;

static int ent_cmp(const void *a, const void *b) {
    uint32_t x = ((const Ent *)a)->key, y = ((const Ent *)b)->key;
    return (x > y) - (x < y);
}
/* Knuth multiplicative hash on the key. */
static uint32_t ent_hash(const void *k) {
    return ((const Ent *)k)->key * 2654435761u;
}
/* Pathological hash: everything lands in bucket 0 (forces the AVL
 * collision path to carry the whole table in one tree). */
static uint32_t hash_zero(const void *k) { (void)k; return 0; }

/* ============================================================
 *  Sizing + init
 * ============================================================ */

static void test_pool_bytes_macro_matches_function(void) {
    ASSERT_EQ_INT((int)AVLHASH_POOL_BYTES(64, sizeof(Ent)),
                  (int)avlhash_pool_bytes(64, sizeof(Ent)));
}

static void test_init_rejects_bad_args(void) {
    uint32_t buckets[8];
    uint8_t  pool[AVLHASH_POOL_BYTES(8, sizeof(Ent))];
    AvlHash  h;
    ASSERT(!avlhash_init(NULL, buckets, 8, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent)));
    ASSERT(!avlhash_init(&h, NULL, 8, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent)));
    ASSERT(!avlhash_init(&h, buckets, 0, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent)));
    ASSERT(!avlhash_init(&h, buckets, 8, NULL, ent_cmp, pool, sizeof pool, sizeof(Ent)));
    ASSERT(!avlhash_init(&h, buckets, 8, ent_hash, NULL, pool, sizeof pool, sizeof(Ent)));
    ASSERT(!avlhash_init(&h, buckets, 8, ent_hash, ent_cmp, NULL, sizeof pool, sizeof(Ent)));
    ASSERT(!avlhash_init(&h, buckets, 8, ent_hash, ent_cmp, pool, 0, sizeof(Ent)));
    ASSERT(!avlhash_init(&h, buckets, 8, ent_hash, ent_cmp, pool, sizeof pool, 0));
    /* a valid init succeeds */
    ASSERT(avlhash_init(&h, buckets, 8, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent)));
    ASSERT(avlhash_empty(&h));
}

/* ============================================================
 *  Basic insert / find / contains
 * ============================================================ */

static void test_insert_find(void) {
    uint32_t buckets[8];
    uint8_t  pool[AVLHASH_POOL_BYTES(32, sizeof(Ent))];
    AvlHash  h;
    avlhash_init(&h, buckets, 8, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent));

    for (uint32_t i = 0; i < 10; i++) {
        Ent e = { i, i * 100u };
        ASSERT(avlhash_insert(&h, &e));
    }
    ASSERT_EQ_INT(10, (int)avlhash_count(&h));

    for (uint32_t i = 0; i < 10; i++) {
        Ent q = { i, 0 }, out;
        ASSERT(avlhash_contains(&h, &q));
        ASSERT(avlhash_find(&h, &q, &out));
        ASSERT_EQ_INT((int)(i * 100u), (int)out.val);   /* value retrieved */
    }
    Ent miss = { 999, 0 };
    ASSERT(!avlhash_contains(&h, &miss));
    ASSERT(!avlhash_find(&h, &miss, NULL));
}

static void test_duplicate_rejected(void) {
    uint32_t buckets[4];
    uint8_t  pool[AVLHASH_POOL_BYTES(8, sizeof(Ent))];
    AvlHash  h;
    avlhash_init(&h, buckets, 4, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent));

    Ent a = { 5, 1 };
    ASSERT(avlhash_insert(&h, &a));
    Ent b = { 5, 2 };                 /* same key, different value */
    ASSERT(!avlhash_insert(&h, &b));  /* set semantics: rejected */
    ASSERT_EQ_INT(1, (int)avlhash_count(&h));
    Ent out;
    ASSERT(avlhash_find(&h, &a, &out));
    ASSERT_EQ_INT(1, (int)out.val);   /* original value kept */
}

/* ============================================================
 *  Remove
 * ============================================================ */

static void test_remove(void) {
    uint32_t buckets[8];
    uint8_t  pool[AVLHASH_POOL_BYTES(32, sizeof(Ent))];
    AvlHash  h;
    avlhash_init(&h, buckets, 8, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent));

    for (uint32_t i = 0; i < 12; i++) { Ent e = { i, i }; avlhash_insert(&h, &e); }

    Ent q = { 7, 0 }, out;
    ASSERT(avlhash_remove(&h, &q, &out));
    ASSERT_EQ_INT(7, (int)out.val);
    ASSERT_EQ_INT(11, (int)avlhash_count(&h));
    ASSERT(!avlhash_contains(&h, &q));
    ASSERT(!avlhash_remove(&h, &q, NULL));   /* already gone */

    /* everything else still present */
    for (uint32_t i = 0; i < 12; i++) {
        Ent k = { i, 0 };
        if (i == 7) ASSERT(!avlhash_contains(&h, &k));
        else        ASSERT(avlhash_contains(&h, &k));
    }
}

/* ============================================================
 *  Collision path: force the whole table into one AVL bucket.
 * ============================================================ */

static void test_single_bucket_avl(void) {
    enum { N = 200 };
    uint32_t buckets[1];
    static uint8_t pool[AVLHASH_POOL_BYTES(N, sizeof(Ent))];
    AvlHash  h;
    /* hash_zero -> every key collides into bucket 0's AVL tree. */
    avlhash_init(&h, buckets, 1, hash_zero, ent_cmp, pool, sizeof pool, sizeof(Ent));

    for (uint32_t i = 0; i < N; i++) { Ent e = { i, i + 1u }; ASSERT(avlhash_insert(&h, &e)); }
    ASSERT_EQ_INT(N, (int)avlhash_count(&h));
    for (uint32_t i = 0; i < N; i++) {
        Ent k = { i, 0 }, out;
        ASSERT(avlhash_find(&h, &k, &out));
        ASSERT_EQ_INT((int)(i + 1u), (int)out.val);
    }
    /* remove the evens; odds remain, count halves. */
    for (uint32_t i = 0; i < N; i += 2) { Ent k = { i, 0 }; ASSERT(avlhash_remove(&h, &k, NULL)); }
    ASSERT_EQ_INT(N / 2, (int)avlhash_count(&h));
    for (uint32_t i = 0; i < N; i++) {
        Ent k = { i, 0 };
        if (i % 2 == 0) ASSERT(!avlhash_contains(&h, &k));
        else            ASSERT(avlhash_contains(&h, &k));
    }
}

/* ============================================================
 *  Pool exhaustion is shared across buckets
 * ============================================================ */

static void test_pool_exhaustion(void) {
    uint32_t buckets[8];
    uint8_t  pool[AVLHASH_POOL_BYTES(4, sizeof(Ent))];   /* 4 total nodes */
    AvlHash  h;
    avlhash_init(&h, buckets, 8, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent));

    for (uint32_t i = 0; i < 4; i++) { Ent e = { i, i }; ASSERT(avlhash_insert(&h, &e)); }
    ASSERT(avlhash_full(&h));
    Ent over = { 100, 0 };
    ASSERT(!avlhash_insert(&h, &over));      /* shared pool exhausted */

    Ent k0 = { 0, 0 };
    ASSERT(avlhash_remove(&h, &k0, NULL));   /* free one */
    ASSERT(avlhash_insert(&h, &over));       /* now fits (any bucket) */
    ASSERT_EQ_INT(4, (int)avlhash_count(&h));
}

/* ============================================================
 *  foreach visits every entry exactly once
 * ============================================================ */

typedef struct { int n; uint64_t key_sum; } Acc;
static void accumulate(const void *elem, void *user) {
    Acc *a = (Acc *)user;
    a->n++;
    a->key_sum += ((const Ent *)elem)->key;
}

static void test_foreach_visits_all(void) {
    uint32_t buckets[8];
    uint8_t  pool[AVLHASH_POOL_BYTES(32, sizeof(Ent))];
    AvlHash  h;
    avlhash_init(&h, buckets, 8, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent));

    uint64_t expect = 0;
    for (uint32_t i = 0; i < 20; i++) { Ent e = { i, i }; avlhash_insert(&h, &e); expect += i; }

    Acc acc = { 0, 0 };
    avlhash_foreach(&h, accumulate, &acc);
    ASSERT_EQ_INT(20, acc.n);
    ASSERT(acc.key_sum == expect);
}

static void test_clear_resets(void) {
    uint32_t buckets[8];
    uint8_t  pool[AVLHASH_POOL_BYTES(16, sizeof(Ent))];
    AvlHash  h;
    avlhash_init(&h, buckets, 8, ent_hash, ent_cmp, pool, sizeof pool, sizeof(Ent));
    for (uint32_t i = 0; i < 10; i++) { Ent e = { i, i }; avlhash_insert(&h, &e); }
    avlhash_clear(&h);
    ASSERT(avlhash_empty(&h));
    ASSERT_EQ_INT(0, (int)avlhash_count(&h));
    Ent e = { 42, 1 };
    ASSERT(avlhash_insert(&h, &e));          /* usable after clear */
    Ent k = { 42, 0 };
    ASSERT(avlhash_contains(&h, &k));
}

int main(void) {
    RUN(test_pool_bytes_macro_matches_function);
    RUN(test_init_rejects_bad_args);
    RUN(test_insert_find);
    RUN(test_duplicate_rejected);
    RUN(test_remove);
    RUN(test_single_bucket_avl);
    RUN(test_pool_exhaustion);
    RUN(test_foreach_visits_all);
    RUN(test_clear_resets);
    return TEST_SUITE_RESULT();
}
