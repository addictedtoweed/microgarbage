/* Tests for the generic hash table module.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "containers/hashtable.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Module-local state for the value-free callback tests. */
static int g_free_calls = 0;
static void counting_free(void *p) { g_free_calls++; free(p); }

static void test_create_and_destroy(void) {
    HashTable *ht = ht_create(0);
    ASSERT_NOT_NULL(ht);
    ASSERT_EQ_INT(0, (int)ht_count(ht));
    ht_destroy(ht, NULL);
}

static void test_destroy_null(void) {
    /* Should not crash. */
    ht_destroy(NULL, NULL);
    ht_destroy(NULL, counting_free);
}

static void test_get_missing_returns_null(void) {
    HashTable *ht = ht_create(0);
    ASSERT_NOT_NULL(ht);
    ASSERT_NULL(ht_get(ht, "nope"));
    ASSERT_NULL(ht_get(ht, ""));
    ht_destroy(ht, NULL);
}

static void test_insert_and_retrieve(void) {
    HashTable *ht = ht_create(0);
    ASSERT_NOT_NULL(ht);

    int x = 42;
    ASSERT_EQ_INT(0, ht_set(ht, "answer", &x));
    ASSERT_EQ_INT(1, (int)ht_count(ht));
    ASSERT_EQ_PTR(&x, ht_get(ht, "answer"));

    ht_destroy(ht, NULL);
}

static void test_overwrite_keeps_count(void) {
    HashTable *ht = ht_create(0);
    int a = 1, b = 2;
    ht_set(ht, "k", &a);
    ASSERT_EQ_INT(1, (int)ht_count(ht));
    ht_set(ht, "k", &b);
    ASSERT_EQ_INT(1, (int)ht_count(ht));
    ASSERT_EQ_PTR(&b, ht_get(ht, "k"));
    ht_destroy(ht, NULL);
}

static void test_delete_present(void) {
    HashTable *ht = ht_create(0);
    int x = 1;
    ht_set(ht, "k", &x);
    ASSERT_EQ_INT(1, ht_delete(ht, "k", NULL));
    ASSERT_EQ_INT(0, (int)ht_count(ht));
    ASSERT_NULL(ht_get(ht, "k"));
    ht_destroy(ht, NULL);
}

static void test_delete_absent(void) {
    HashTable *ht = ht_create(0);
    ASSERT_EQ_INT(0, ht_delete(ht, "nope", NULL));
    int x = 1;
    ht_set(ht, "k", &x);
    ASSERT_EQ_INT(0, ht_delete(ht, "other", NULL));  /* different key, ditto */
    ht_destroy(ht, NULL);
}

static void test_collision_chain_delete(void) {
    /* Small bucket count + many keys = guaranteed collisions.
     * Delete every other one to exercise head/middle/tail
     * unlinking of chain entries. */
    HashTable *ht = ht_create(4);
    ASSERT_NOT_NULL(ht);

    char keybuf[16];
    static int values[20];
    for (int i = 0; i < 10; i++) {
        snprintf(keybuf, sizeof keybuf, "key_%d", i);
        values[i] = i * 10;
        ASSERT_EQ_INT(0, ht_set(ht, keybuf, &values[i]));
    }

    for (int i = 0; i < 10; i += 2) {
        snprintf(keybuf, sizeof keybuf, "key_%d", i);
        ASSERT_EQ_INT(1, ht_delete(ht, keybuf, NULL));
    }

    for (int i = 0; i < 10; i++) {
        snprintf(keybuf, sizeof keybuf, "key_%d", i);
        void *v = ht_get(ht, keybuf);
        if (i % 2 == 0) {
            ASSERT_NULL(v);
        } else {
            ASSERT_EQ_PTR(&values[i], v);
        }
    }
    ASSERT_EQ_INT(5, (int)ht_count(ht));

    ht_destroy(ht, NULL);
}

static void test_resize_preserves_entries(void) {
    /* 1000 items starting from 4 buckets → ~8 doublings, all
     * entries must remain findable. */
    HashTable *ht = ht_create(4);
    ASSERT_NOT_NULL(ht);

    static int values[1000];
    char keybuf[32];

    for (int i = 0; i < 1000; i++) {
        snprintf(keybuf, sizeof keybuf, "item_%06d", i);
        values[i] = i;
        ASSERT_EQ_INT(0, ht_set(ht, keybuf, &values[i]));
    }
    ASSERT_EQ_INT(1000, (int)ht_count(ht));

    for (int i = 0; i < 1000; i++) {
        snprintf(keybuf, sizeof keybuf, "item_%06d", i);
        int *v = ht_get(ht, keybuf);
        ASSERT_NOT_NULL(v);
        ASSERT_EQ_INT(i, *v);
    }

    ht_destroy(ht, NULL);
}

static void test_value_free_callback(void) {
    HashTable *ht = ht_create(0);
    ASSERT_NOT_NULL(ht);

    g_free_calls = 0;

    /* 50 heap-allocated values. */
    for (int i = 0; i < 50; i++) {
        char keybuf[16];
        snprintf(keybuf, sizeof keybuf, "k%d", i);
        int *p = malloc(sizeof *p);
        ASSERT_NOT_NULL(p);
        *p = i;
        ASSERT_EQ_INT(0, ht_set(ht, keybuf, p));
    }

    /* Delete 10 via the callback. */
    for (int i = 0; i < 10; i++) {
        char keybuf[16];
        snprintf(keybuf, sizeof keybuf, "k%d", i);
        ASSERT_EQ_INT(1, ht_delete(ht, keybuf, counting_free));
    }
    ASSERT_EQ_INT(10, g_free_calls);

    /* Destroy frees the remaining 40 via the same callback. */
    ht_destroy(ht, counting_free);
    ASSERT_EQ_INT(50, g_free_calls);
}

static void test_null_value_is_legal(void) {
    /* NULL is a valid stored value. ht_get can't distinguish
     * "stored as NULL" from "absent" — caller has to use count
     * or delete-return for presence. */
    HashTable *ht = ht_create(0);
    ASSERT_EQ_INT(0, ht_set(ht, "k", NULL));
    ASSERT_EQ_INT(1, (int)ht_count(ht));
    ASSERT_NULL(ht_get(ht, "k"));
    ASSERT_EQ_INT(1, ht_delete(ht, "k", NULL));   /* confirms presence */
    ASSERT_EQ_INT(0, (int)ht_count(ht));
    ht_destroy(ht, NULL);
}

static void test_empty_string_key(void) {
    HashTable *ht = ht_create(0);
    int x = 7;
    ASSERT_EQ_INT(0, ht_set(ht, "", &x));
    ASSERT_EQ_PTR(&x, ht_get(ht, ""));
    ASSERT_EQ_INT(1, ht_delete(ht, "", NULL));
    ht_destroy(ht, NULL);
}

/* ============================================================
 *  Custom allocator
 * ============================================================ */

/* A tracking allocator that counts alloc/free calls. Lets us
 * verify the table is actually using the allocator we gave it
 * and that destroy frees everything. */
static int alloc_calls = 0;
static int free_calls_tracked = 0;

static void *tracking_alloc(size_t n) {
    alloc_calls++;
    return malloc(n);
}
static void tracking_free(void *p) {
    if (p) free_calls_tracked++;   /* don't count free(NULL) */
    free(p);
}

static void test_custom_allocator_routes_through(void) {
    alloc_calls = 0;
    free_calls_tracked = 0;

    HashTable *ht = ht_create_with_allocator(0, tracking_alloc, tracking_free);
    ASSERT_NOT_NULL(ht);
    ASSERT(alloc_calls >= 2);   /* at least: HashTable struct + bucket array */

    int x = 42;
    int before = alloc_calls;
    ASSERT_EQ_INT(0, ht_set(ht, "key", &x));
    ASSERT(alloc_calls > before);   /* set caused at least one alloc */

    ASSERT_EQ_PTR(&x, ht_get(ht, "key"));

    int allocs_before_destroy = alloc_calls;
    ht_destroy(ht, NULL);
    /* Destroy didn't alloc anything new */
    ASSERT_EQ_INT(allocs_before_destroy, alloc_calls);
    /* Destroy freed everything: the alloc count and free count
     * should now match. */
    ASSERT_EQ_INT(alloc_calls, free_calls_tracked);
}

static void test_null_allocator_uses_default(void) {
    /* Passing NULL for both should be equivalent to ht_create. */
    HashTable *ht = ht_create_with_allocator(0, NULL, NULL);
    ASSERT_NOT_NULL(ht);
    int x = 1;
    ASSERT_EQ_INT(0, ht_set(ht, "k", &x));
    ASSERT_EQ_PTR(&x, ht_get(ht, "k"));
    ht_destroy(ht, NULL);
}

/* A "bump" allocator that never frees — the canonical embedded
 * pattern. Confirms ht_set works even when ht_free is a no-op. */
static uint8_t bump_pool[16384];
static size_t  bump_used = 0;

static void *bump_alloc(size_t n) {
    /* Align allocations to 8 bytes to match what most allocators
     * give callers (and what some platforms require for pointers). */
    bump_used = (bump_used + 7u) & ~(size_t)7u;
    if (bump_used + n > sizeof bump_pool) return NULL;
    void *p = &bump_pool[bump_used];
    bump_used += n;
    return p;
}
static void bump_free(void *p) { (void)p; }   /* no-op */

static void test_bump_allocator_no_free(void) {
    bump_used = 0;
    HashTable *ht = ht_create_with_allocator(8, bump_alloc, bump_free);
    ASSERT_NOT_NULL(ht);

    /* Insert 50 items. None of the inserts should fail — pool is
     * big enough — and destroy must complete cleanly even though
     * bump_free does nothing. */
    int values[50];
    char keybuf[16];
    for (int i = 0; i < 50; i++) {
        snprintf(keybuf, sizeof keybuf, "k_%d", i);
        values[i] = i;
        ASSERT_EQ_INT(0, ht_set(ht, keybuf, &values[i]));
    }
    ASSERT_EQ_INT(50, (int)ht_count(ht));

    /* All entries still findable. */
    for (int i = 0; i < 50; i++) {
        snprintf(keybuf, sizeof keybuf, "k_%d", i);
        int *v = ht_get(ht, keybuf);
        ASSERT_NOT_NULL(v);
        ASSERT_EQ_INT(i, *v);
    }

    /* Destroy must not crash. The pool memory stays valid until
     * the caller resets bump_used, which is exactly the contract
     * the bump allocator implies. */
    ht_destroy(ht, NULL);
}

int main(void) {
    TEST_SUITE("hashtable");
    RUN(test_create_and_destroy);
    RUN(test_destroy_null);
    RUN(test_get_missing_returns_null);
    RUN(test_insert_and_retrieve);
    RUN(test_overwrite_keeps_count);
    RUN(test_delete_present);
    RUN(test_delete_absent);
    RUN(test_collision_chain_delete);
    RUN(test_resize_preserves_entries);
    RUN(test_value_free_callback);
    RUN(test_null_value_is_legal);
    RUN(test_empty_string_key);
    RUN(test_custom_allocator_routes_through);
    RUN(test_null_allocator_uses_default);
    RUN(test_bump_allocator_no_free);
    return TEST_SUITE_RESULT();
}
