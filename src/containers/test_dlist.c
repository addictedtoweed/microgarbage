/* Tests for dlist (doubly-linked list over a node pool). */

#include "test_runner.h"
#include "containers/dlist.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ============================================================
 *  Sizing
 * ============================================================ */

static void test_pool_bytes_macro_matches_function(void) {
    ASSERT_EQ_INT((int)DLIST_POOL_BYTES(64, sizeof(int)),
                  (int)dlist_pool_bytes(64, sizeof(int)));
    ASSERT_EQ_INT((int)DLIST_POOL_BYTES(10, 40),
                  (int)dlist_pool_bytes(10, 40));
}

static void test_init_capacity_from_pool(void) {
    uint8_t pool[DLIST_POOL_BYTES(8, sizeof(int))];
    DList l;
    ASSERT(dlist_init(&l, pool, sizeof pool, sizeof(int)));
    ASSERT_EQ_INT(8, (int)l.capacity);
    ASSERT(dlist_empty(&l));
    ASSERT(!dlist_full(&l));
}

static void test_init_rejects_bad_args(void) {
    uint8_t pool[64];
    DList l;
    ASSERT(!dlist_init(NULL, pool, sizeof pool, sizeof(int)));
    ASSERT(!dlist_init(&l, NULL, sizeof pool, sizeof(int)));
    ASSERT(!dlist_init(&l, pool, sizeof pool, 0));
    ASSERT(!dlist_init(&l, pool, 0, sizeof(int)));   /* zero capacity */
}

/* ============================================================
 *  Basic push / pop
 * ============================================================ */

static void test_push_back_pop_front_is_fifo(void) {
    uint8_t pool[DLIST_POOL_BYTES(8, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));

    for (int i = 0; i < 5; i++) ASSERT(dlist_push_back(&l, &i));
    ASSERT_EQ_INT(5, (int)dlist_count(&l));

    for (int i = 0; i < 5; i++) {
        int v = -1;
        ASSERT(dlist_pop_front(&l, &v));
        ASSERT_EQ_INT(i, v);
    }
    ASSERT(dlist_empty(&l));
}

static void test_push_front_pop_front_is_lifo(void) {
    uint8_t pool[DLIST_POOL_BYTES(8, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));

    for (int i = 0; i < 5; i++) ASSERT(dlist_push_front(&l, &i));
    for (int i = 4; i >= 0; i--) {
        int v = -1;
        ASSERT(dlist_pop_front(&l, &v));
        ASSERT_EQ_INT(i, v);
    }
}

static void test_push_back_pop_back_is_lifo(void) {
    uint8_t pool[DLIST_POOL_BYTES(8, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));

    for (int i = 0; i < 5; i++) ASSERT(dlist_push_back(&l, &i));
    for (int i = 4; i >= 0; i--) {
        int v = -1;
        ASSERT(dlist_pop_back(&l, &v));
        ASSERT_EQ_INT(i, v);
    }
}

static void test_front_back_peek(void) {
    uint8_t pool[DLIST_POOL_BYTES(8, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));

    int a = 10, b = 20, c = 30;
    dlist_push_back(&l, &a);
    dlist_push_back(&l, &b);
    dlist_push_back(&l, &c);

    int v = 0;
    ASSERT(dlist_front(&l, &v)); ASSERT_EQ_INT(10, v);
    ASSERT(dlist_back(&l, &v));  ASSERT_EQ_INT(30, v);
    ASSERT_EQ_INT(3, (int)dlist_count(&l));   /* peek didn't consume */
}

/* ============================================================
 *  Capacity / empty edge cases
 * ============================================================ */

static void test_push_rejects_when_full(void) {
    uint8_t pool[DLIST_POOL_BYTES(3, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));

    int x = 1;
    ASSERT(dlist_push_back(&l, &x));
    ASSERT(dlist_push_back(&l, &x));
    ASSERT(dlist_push_back(&l, &x));
    ASSERT(dlist_full(&l));
    ASSERT(!dlist_push_back(&l, &x));   /* pool exhausted */
    ASSERT(!dlist_push_front(&l, &x));
    ASSERT_EQ_INT(3, (int)dlist_count(&l));
}

static void test_pop_empty_returns_zero(void) {
    uint8_t pool[DLIST_POOL_BYTES(4, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));

    int v = 99;
    ASSERT(!dlist_pop_front(&l, &v));
    ASSERT(!dlist_pop_back(&l, &v));
    ASSERT_EQ_INT(99, v);               /* out untouched */
    ASSERT(!dlist_front(&l, &v));
    ASSERT(!dlist_back(&l, &v));
}

static void test_node_reuse_after_pop(void) {
    /* Pop everything, push again — free list must recycle nodes so we
     * can refill to capacity repeatedly. */
    uint8_t pool[DLIST_POOL_BYTES(3, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));

    for (int round = 0; round < 4; round++) {
        for (int i = 0; i < 3; i++) ASSERT(dlist_push_back(&l, &i));
        ASSERT(dlist_full(&l));
        for (int i = 0; i < 3; i++) { int v; ASSERT(dlist_pop_front(&l, &v)); }
        ASSERT(dlist_empty(&l));
    }
}

static void test_clear_resets(void) {
    uint8_t pool[DLIST_POOL_BYTES(4, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));
    int x = 5;
    dlist_push_back(&l, &x); dlist_push_back(&l, &x);
    dlist_clear(&l);
    ASSERT(dlist_empty(&l));
    ASSERT_EQ_INT(0, (int)dlist_count(&l));
    /* usable after clear */
    ASSERT(dlist_push_back(&l, &x));
}

/* ============================================================
 *  Iteration
 * ============================================================ */

static void test_forward_iteration(void) {
    uint8_t pool[DLIST_POOL_BYTES(8, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));
    for (int i = 0; i < 5; i++) dlist_push_back(&l, &i);

    int expected = 0;
    for (uint32_t it = dlist_begin(&l); it != DLIST_NIL; it = dlist_next(&l, it)) {
        int v; ASSERT(dlist_get(&l, it, &v));
        ASSERT_EQ_INT(expected, v);
        expected++;
    }
    ASSERT_EQ_INT(5, expected);
}

static void test_backward_iteration(void) {
    uint8_t pool[DLIST_POOL_BYTES(8, sizeof(int))];
    DList l;
    dlist_init(&l, pool, sizeof pool, sizeof(int));
    for (int i = 0; i < 5; i++) dlist_push_back(&l, &i);

    int expected = 4;
    for (uint32_t it = dlist_rbegin(&l); it != DLIST_NIL; it = dlist_prev(&l, it)) {
        int v; ASSERT(dlist_get(&l, it, &v));
        ASSERT_EQ_INT(expected, v);
        expected--;
    }
    ASSERT_EQ_INT(-1, expected);
}

/* ============================================================
 *  Arbitrary payload types
 * ============================================================ */

typedef struct { int id; char name[12]; } Thing;

static void test_struct_payload(void) {
    uint8_t pool[DLIST_POOL_BYTES(4, sizeof(Thing))];
    DList l;
    ASSERT(dlist_init(&l, pool, sizeof pool, sizeof(Thing)));

    Thing a = { 1, "alpha" }, b = { 2, "beta" };
    ASSERT(dlist_push_back(&l, &a));
    ASSERT(dlist_push_back(&l, &b));

    Thing out;
    ASSERT(dlist_pop_front(&l, &out));
    ASSERT_EQ_INT(1, out.id);
    ASSERT_EQ_STR("alpha", out.name);
    ASSERT(dlist_pop_front(&l, &out));
    ASSERT_EQ_INT(2, out.id);
    ASSERT_EQ_STR("beta", out.name);
}

int main(void) {
    RUN(test_pool_bytes_macro_matches_function);
    RUN(test_init_capacity_from_pool);
    RUN(test_init_rejects_bad_args);

    RUN(test_push_back_pop_front_is_fifo);
    RUN(test_push_front_pop_front_is_lifo);
    RUN(test_push_back_pop_back_is_lifo);
    RUN(test_front_back_peek);

    RUN(test_push_rejects_when_full);
    RUN(test_pop_empty_returns_zero);
    RUN(test_node_reuse_after_pop);
    RUN(test_clear_resets);

    RUN(test_forward_iteration);
    RUN(test_backward_iteration);

    RUN(test_struct_payload);

    return TEST_SUITE_RESULT();
}
