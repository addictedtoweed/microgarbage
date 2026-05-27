/* Tests for fifo_queue.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "containers/fifo_queue.h"

#include <stdint.h>
#include <stdio.h>

/* ============================================================
 *  Basic ops
 * ============================================================ */

static void test_init_empty(void) {
    int storage[8];
    FifoQueue f;
    fifo_init(&f, storage, 8, sizeof(int));
    ASSERT(fifo_empty(&f));
    ASSERT(!fifo_full(&f));
    ASSERT_EQ_INT(0, (int)fifo_count(&f));
}

static void test_push_pop(void) {
    int storage[4];
    FifoQueue f;
    fifo_init(&f, storage, 4, sizeof(int));

    int in = 7, out = 0;
    ASSERT_EQ_INT(1, fifo_push(&f, &in));
    ASSERT_EQ_INT(1, fifo_pop(&f, &out));
    ASSERT_EQ_INT(7, out);
}

static void test_fifo_ordering(void) {
    int storage[8];
    FifoQueue f;
    fifo_init(&f, storage, 8, sizeof(int));

    for (int i = 0; i < 5; i++) fifo_push(&f, &i);
    for (int i = 0; i < 5; i++) {
        int out;
        fifo_pop(&f, &out);
        ASSERT_EQ_INT(i, out);
    }
}

/* ============================================================
 *  Reject-on-full — the FIFO's defining behavior
 * ============================================================ */

static void test_push_rejects_when_full(void) {
    int storage[4];
    FifoQueue f;
    fifo_init(&f, storage, 4, sizeof(int));

    /* Fill it */
    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_INT(1, fifo_push(&f, &i));
    }
    ASSERT(fifo_full(&f));

    /* Fifth push must fail */
    int extra = 999;
    ASSERT_EQ_INT(0, fifo_push(&f, &extra));
    ASSERT_EQ_INT(4, (int)fifo_count(&f));

    /* The original four are still there in order */
    for (int i = 0; i < 4; i++) {
        int out;
        fifo_pop(&f, &out);
        ASSERT_EQ_INT(i, out);
    }
}

static void test_push_succeeds_after_pop(void) {
    int storage[2];
    FifoQueue f;
    fifo_init(&f, storage, 2, sizeof(int));

    int a = 1, b = 2;
    fifo_push(&f, &a);
    fifo_push(&f, &b);
    ASSERT(fifo_full(&f));

    int c = 3;
    ASSERT_EQ_INT(0, fifo_push(&f, &c));   /* still full, rejects */

    int out;
    fifo_pop(&f, &out);                    /* now has room */
    ASSERT_EQ_INT(1, fifo_push(&f, &c));   /* succeeds */
    ASSERT(fifo_full(&f));
}

/* ============================================================
 *  Pop and peek on empty
 * ============================================================ */

static void test_pop_empty(void) {
    int storage[4];
    FifoQueue f;
    fifo_init(&f, storage, 4, sizeof(int));
    int out;
    ASSERT_EQ_INT(0, fifo_pop(&f, &out));
}

static void test_peek(void) {
    int storage[4];
    FifoQueue f;
    fifo_init(&f, storage, 4, sizeof(int));

    int in = 55, out = 0;
    fifo_push(&f, &in);
    ASSERT_EQ_INT(1, fifo_peek(&f, &out));
    ASSERT_EQ_INT(55, out);
    ASSERT_EQ_INT(1, (int)fifo_count(&f));   /* peek didn't consume */
}

static void test_reset(void) {
    int storage[4];
    FifoQueue f;
    fifo_init(&f, storage, 4, sizeof(int));

    for (int i = 0; i < 4; i++) fifo_push(&f, &i);
    fifo_reset(&f);
    ASSERT(fifo_empty(&f));
}

int main(void) {
    TEST_SUITE("fifo_queue");

    RUN(test_init_empty);
    RUN(test_push_pop);
    RUN(test_fifo_ordering);

    RUN(test_push_rejects_when_full);
    RUN(test_push_succeeds_after_pop);

    RUN(test_pop_empty);
    RUN(test_peek);
    RUN(test_reset);

    return TEST_SUITE_RESULT();
}
