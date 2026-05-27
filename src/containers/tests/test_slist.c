/* Tests for slist (singly-linked list over a node pool).
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "containers/slist.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void test_pool_bytes_macro_matches_function(void) {
    ASSERT_EQ_INT((int)SLIST_POOL_BYTES(64, sizeof(int)),
                  (int)slist_pool_bytes(64, sizeof(int)));
    ASSERT_EQ_INT((int)SLIST_POOL_BYTES(10, 40),
                  (int)slist_pool_bytes(10, 40));
}

static void test_node_smaller_than_dlist(void) {
    /* The whole point: slist node = 4 + elem; dlist node = 8 + elem.
     * For the same capacity, slist's pool is smaller. */
    ASSERT(SLIST_POOL_BYTES(100, sizeof(int)) <
           (size_t)(100 * (8 + sizeof(int))));
}

static void test_init_capacity_from_pool(void) {
    uint8_t pool[SLIST_POOL_BYTES(8, sizeof(int))];
    SList l;
    ASSERT(slist_init(&l, pool, sizeof pool, sizeof(int)));
    ASSERT_EQ_INT(8, (int)l.capacity);
    ASSERT(slist_empty(&l));
}

static void test_init_rejects_bad_args(void) {
    uint8_t pool[64];
    SList l;
    ASSERT(!slist_init(NULL, pool, sizeof pool, sizeof(int)));
    ASSERT(!slist_init(&l, NULL, sizeof pool, sizeof(int)));
    ASSERT(!slist_init(&l, pool, sizeof pool, 0));
    ASSERT(!slist_init(&l, pool, 0, sizeof(int)));
}

static void test_push_back_pop_front_is_fifo(void) {
    uint8_t pool[SLIST_POOL_BYTES(8, sizeof(int))];
    SList l;
    slist_init(&l, pool, sizeof pool, sizeof(int));
    for (int i = 0; i < 5; i++) ASSERT(slist_push_back(&l, &i));
    for (int i = 0; i < 5; i++) {
        int v = -1; ASSERT(slist_pop_front(&l, &v)); ASSERT_EQ_INT(i, v);
    }
    ASSERT(slist_empty(&l));
}

static void test_push_front_pop_front_is_lifo(void) {
    uint8_t pool[SLIST_POOL_BYTES(8, sizeof(int))];
    SList l;
    slist_init(&l, pool, sizeof pool, sizeof(int));
    for (int i = 0; i < 5; i++) ASSERT(slist_push_front(&l, &i));
    for (int i = 4; i >= 0; i--) {
        int v = -1; ASSERT(slist_pop_front(&l, &v)); ASSERT_EQ_INT(i, v);
    }
}

static void test_pop_back_correct(void) {
    /* pop_back is O(n) but must be correct: build 1..5, pop from back. */
    uint8_t pool[SLIST_POOL_BYTES(8, sizeof(int))];
    SList l;
    slist_init(&l, pool, sizeof pool, sizeof(int));
    for (int i = 1; i <= 5; i++) slist_push_back(&l, &i);
    for (int i = 5; i >= 1; i--) {
        int v = -1; ASSERT(slist_pop_back(&l, &v)); ASSERT_EQ_INT(i, v);
    }
    ASSERT(slist_empty(&l));
}

static void test_pop_back_single_element(void) {
    uint8_t pool[SLIST_POOL_BYTES(4, sizeof(int))];
    SList l;
    slist_init(&l, pool, sizeof pool, sizeof(int));
    int x = 42;
    slist_push_back(&l, &x);
    int v = 0;
    ASSERT(slist_pop_back(&l, &v));
    ASSERT_EQ_INT(42, v);
    ASSERT(slist_empty(&l));
    /* head and tail both reset — pushing again works */
    ASSERT(slist_push_front(&l, &x));
    ASSERT_EQ_INT(1, (int)slist_count(&l));
}

static void test_front_back_peek(void) {
    uint8_t pool[SLIST_POOL_BYTES(8, sizeof(int))];
    SList l;
    slist_init(&l, pool, sizeof pool, sizeof(int));
    int a = 10, b = 20, c = 30;
    slist_push_back(&l, &a); slist_push_back(&l, &b); slist_push_back(&l, &c);
    int v = 0;
    ASSERT(slist_front(&l, &v)); ASSERT_EQ_INT(10, v);
    ASSERT(slist_back(&l, &v));  ASSERT_EQ_INT(30, v);
    ASSERT_EQ_INT(3, (int)slist_count(&l));
}

static void test_push_rejects_when_full(void) {
    uint8_t pool[SLIST_POOL_BYTES(3, sizeof(int))];
    SList l;
    slist_init(&l, pool, sizeof pool, sizeof(int));
    int x = 1;
    ASSERT(slist_push_back(&l, &x));
    ASSERT(slist_push_back(&l, &x));
    ASSERT(slist_push_back(&l, &x));
    ASSERT(slist_full(&l));
    ASSERT(!slist_push_back(&l, &x));
    ASSERT(!slist_push_front(&l, &x));
}

static void test_pop_empty_returns_zero(void) {
    uint8_t pool[SLIST_POOL_BYTES(4, sizeof(int))];
    SList l;
    slist_init(&l, pool, sizeof pool, sizeof(int));
    int v = 99;
    ASSERT(!slist_pop_front(&l, &v));
    ASSERT(!slist_pop_back(&l, &v));
    ASSERT_EQ_INT(99, v);
}

static void test_node_reuse_after_pop(void) {
    uint8_t pool[SLIST_POOL_BYTES(3, sizeof(int))];
    SList l;
    slist_init(&l, pool, sizeof pool, sizeof(int));
    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 3; i++) ASSERT(slist_push_back(&l, &i));
        ASSERT(slist_full(&l));
        for (int i = 0; i < 3; i++) { int v; ASSERT(slist_pop_front(&l, &v)); }
        ASSERT(slist_empty(&l));
    }
}

static void test_forward_iteration(void) {
    uint8_t pool[SLIST_POOL_BYTES(8, sizeof(int))];
    SList l;
    slist_init(&l, pool, sizeof pool, sizeof(int));
    for (int i = 0; i < 5; i++) slist_push_back(&l, &i);
    int expected = 0;
    for (uint32_t it = slist_begin(&l); it != SLIST_NIL; it = slist_next(&l, it)) {
        int v; ASSERT(slist_get(&l, it, &v));
        ASSERT_EQ_INT(expected, v); expected++;
    }
    ASSERT_EQ_INT(5, expected);
}

typedef struct { int id; char name[12]; } Thing;

static void test_struct_payload(void) {
    uint8_t pool[SLIST_POOL_BYTES(4, sizeof(Thing))];
    SList l;
    ASSERT(slist_init(&l, pool, sizeof pool, sizeof(Thing)));
    Thing a = { 1, "alpha" }, b = { 2, "beta" };
    slist_push_back(&l, &a); slist_push_back(&l, &b);
    Thing out;
    ASSERT(slist_pop_front(&l, &out));
    ASSERT_EQ_INT(1, out.id); ASSERT_EQ_STR("alpha", out.name);
    ASSERT(slist_pop_front(&l, &out));
    ASSERT_EQ_INT(2, out.id); ASSERT_EQ_STR("beta", out.name);
}

int main(void) {
    RUN(test_pool_bytes_macro_matches_function);
    RUN(test_node_smaller_than_dlist);
    RUN(test_init_capacity_from_pool);
    RUN(test_init_rejects_bad_args);

    RUN(test_push_back_pop_front_is_fifo);
    RUN(test_push_front_pop_front_is_lifo);
    RUN(test_pop_back_correct);
    RUN(test_pop_back_single_element);
    RUN(test_front_back_peek);

    RUN(test_push_rejects_when_full);
    RUN(test_pop_empty_returns_zero);
    RUN(test_node_reuse_after_pop);

    RUN(test_forward_iteration);
    RUN(test_struct_payload);

    return TEST_SUITE_RESULT();
}
