/* Tests for ring_buffer.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "containers/ring_buffer.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ============================================================
 *  Init and basic state
 * ============================================================ */

static void test_init_empty(void) {
    int storage[8];
    RingBuffer rb;
    rb_init(&rb, storage, 8, sizeof(int));

    ASSERT_EQ_INT(0, (int)rb_count(&rb));
    ASSERT(rb_empty(&rb));
    ASSERT(!rb_full(&rb));
}

static void test_pop_empty_returns_zero(void) {
    int storage[4];
    RingBuffer rb;
    rb_init(&rb, storage, 4, sizeof(int));
    int out;
    ASSERT_EQ_INT(0, rb_pop(&rb, &out));
}

static void test_peek_empty_returns_zero(void) {
    int storage[4];
    RingBuffer rb;
    rb_init(&rb, storage, 4, sizeof(int));
    int out;
    ASSERT_EQ_INT(0, rb_peek(&rb, &out));
}

/* ============================================================
 *  Push and pop
 * ============================================================ */

static void test_push_pop_one(void) {
    int storage[4];
    RingBuffer rb;
    rb_init(&rb, storage, 4, sizeof(int));

    int in = 42, out = 0;
    rb_push(&rb, &in);
    ASSERT_EQ_INT(1, (int)rb_count(&rb));
    ASSERT(!rb_empty(&rb));

    ASSERT_EQ_INT(1, rb_pop(&rb, &out));
    ASSERT_EQ_INT(42, out);
    ASSERT_EQ_INT(0, (int)rb_count(&rb));
    ASSERT(rb_empty(&rb));
}

static void test_fifo_ordering(void) {
    int storage[8];
    RingBuffer rb;
    rb_init(&rb, storage, 8, sizeof(int));

    for (int i = 0; i < 5; i++) rb_push(&rb, &i);
    ASSERT_EQ_INT(5, (int)rb_count(&rb));

    for (int i = 0; i < 5; i++) {
        int out;
        ASSERT_EQ_INT(1, rb_pop(&rb, &out));
        ASSERT_EQ_INT(i, out);
    }
    ASSERT(rb_empty(&rb));
}

static void test_peek_does_not_consume(void) {
    int storage[4];
    RingBuffer rb;
    rb_init(&rb, storage, 4, sizeof(int));

    int in = 99, out = 0;
    rb_push(&rb, &in);

    ASSERT_EQ_INT(1, rb_peek(&rb, &out));
    ASSERT_EQ_INT(99, out);
    ASSERT_EQ_INT(1, (int)rb_count(&rb));   /* still there */

    out = 0;
    ASSERT_EQ_INT(1, rb_pop(&rb, &out));
    ASSERT_EQ_INT(99, out);
}

/* ============================================================
 *  Overwrite behavior
 * ============================================================ */

static void test_overwrites_on_full(void) {
    int storage[4];
    RingBuffer rb;
    rb_init(&rb, storage, 4, sizeof(int));

    /* Push 4 — fills the buffer */
    for (int i = 0; i < 4; i++) rb_push(&rb, &i);
    ASSERT(rb_full(&rb));
    ASSERT_EQ_INT(4, (int)rb_count(&rb));

    /* Push a 5th — overwrites oldest, count stays at 4 */
    int extra = 100;
    rb_push(&rb, &extra);
    ASSERT_EQ_INT(4, (int)rb_count(&rb));
    ASSERT(rb_full(&rb));

    /* Pop order: 1, 2, 3, 100 (the 0 got overwritten). */
    int out;
    rb_pop(&rb, &out); ASSERT_EQ_INT(1, out);
    rb_pop(&rb, &out); ASSERT_EQ_INT(2, out);
    rb_pop(&rb, &out); ASSERT_EQ_INT(3, out);
    rb_pop(&rb, &out); ASSERT_EQ_INT(100, out);
}

static void test_many_overwrites(void) {
    /* Push 100 elements into a 4-slot buffer. The last 4 should
     * survive. */
    int storage[4];
    RingBuffer rb;
    rb_init(&rb, storage, 4, sizeof(int));

    for (int i = 0; i < 100; i++) rb_push(&rb, &i);

    ASSERT_EQ_INT(4, (int)rb_count(&rb));
    for (int i = 96; i < 100; i++) {
        int out;
        rb_pop(&rb, &out);
        ASSERT_EQ_INT(i, out);
    }
}

/* ============================================================
 *  Reset
 * ============================================================ */

static void test_reset(void) {
    int storage[8];
    RingBuffer rb;
    rb_init(&rb, storage, 8, sizeof(int));

    for (int i = 0; i < 5; i++) rb_push(&rb, &i);
    rb_reset(&rb);

    ASSERT(rb_empty(&rb));
    ASSERT_EQ_INT(0, (int)rb_count(&rb));
}

/* ============================================================
 *  Different element types
 * ============================================================ */

static void test_byte_elements(void) {
    uint8_t storage[16];
    RingBuffer rb;
    rb_init(&rb, storage, 16, sizeof(uint8_t));

    for (uint8_t i = 0; i < 10; i++) rb_push(&rb, &i);
    ASSERT_EQ_INT(10, (int)rb_count(&rb));

    for (uint8_t i = 0; i < 10; i++) {
        uint8_t out;
        rb_pop(&rb, &out);
        ASSERT_EQ_INT(i, out);
    }
}

typedef struct { int a; int b; float c; } TestStruct;

static void test_struct_elements(void) {
    TestStruct storage[4];
    RingBuffer rb;
    rb_init(&rb, storage, 4, sizeof(TestStruct));

    TestStruct in = { 1, 2, 3.5f };
    rb_push(&rb, &in);

    TestStruct out = {0};
    ASSERT_EQ_INT(1, rb_pop(&rb, &out));
    ASSERT_EQ_INT(1, out.a);
    ASSERT_EQ_INT(2, out.b);
    ASSERT(out.c == 3.5f);
}

static void test_peek_at_offsets(void) {
    int storage[8];
    RingBuffer rb;
    rb_init(&rb, storage, 8, sizeof(int));
    for (int i = 0; i < 5; i++) rb_push(&rb, &i);

    int out;
    ASSERT_EQ_INT(1, rb_peek_at(&rb, 0, &out));
    ASSERT_EQ_INT(0, out);
    ASSERT_EQ_INT(1, rb_peek_at(&rb, 2, &out));
    ASSERT_EQ_INT(2, out);
    ASSERT_EQ_INT(1, rb_peek_at(&rb, 4, &out));
    ASSERT_EQ_INT(4, out);

    /* Out of range. */
    ASSERT_EQ_INT(0, rb_peek_at(&rb, 5, &out));
    ASSERT_EQ_INT(0, rb_peek_at(&rb, 100, &out));

    /* Count unchanged after peeks. */
    ASSERT_EQ_INT(5, (int)rb_count(&rb));
}

static void test_peek_at_wraps_across_storage(void) {
    /* Force the tail to wrap around storage so peek_at has to handle
     * the modulo correctly. */
    int storage[4];
    RingBuffer rb;
    rb_init(&rb, storage, 4, sizeof(int));

    /* Fill, pop 2, then push 2 more so tail is at offset 2. */
    for (int i = 0; i < 4; i++) rb_push(&rb, &i);
    int dummy;
    rb_pop(&rb, &dummy);
    rb_pop(&rb, &dummy);
    int v1 = 100, v2 = 101;
    rb_push(&rb, &v1);
    rb_push(&rb, &v2);
    /* Now buffer holds [2, 3, 100, 101] starting at tail position 2,
     * wrapping around. */

    int out;
    ASSERT_EQ_INT(1, rb_peek_at(&rb, 0, &out)); ASSERT_EQ_INT(2,   out);
    ASSERT_EQ_INT(1, rb_peek_at(&rb, 1, &out)); ASSERT_EQ_INT(3,   out);
    ASSERT_EQ_INT(1, rb_peek_at(&rb, 2, &out)); ASSERT_EQ_INT(100, out);
    ASSERT_EQ_INT(1, rb_peek_at(&rb, 3, &out)); ASSERT_EQ_INT(101, out);
}

int main(void) {
    TEST_SUITE("ring_buffer");

    RUN(test_init_empty);
    RUN(test_pop_empty_returns_zero);
    RUN(test_peek_empty_returns_zero);

    RUN(test_push_pop_one);
    RUN(test_fifo_ordering);
    RUN(test_peek_does_not_consume);

    RUN(test_overwrites_on_full);
    RUN(test_many_overwrites);

    RUN(test_reset);

    RUN(test_byte_elements);
    RUN(test_struct_elements);

    RUN(test_peek_at_offsets);
    RUN(test_peek_at_wraps_across_storage);

    return TEST_SUITE_RESULT();
}
