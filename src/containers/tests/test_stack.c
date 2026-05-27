/* Tests for stack.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "containers/stack.h"

#include <stdint.h>
#include <stdio.h>

/* ============================================================
 *  Init and basic state
 * ============================================================ */

static void test_init_empty(void) {
    int storage[8];
    Stack s;
    stack_init(&s, storage, 8, sizeof(int));

    ASSERT(stack_empty(&s));
    ASSERT(!stack_full(&s));
    ASSERT_EQ_INT(0, (int)stack_count(&s));
}

static void test_pop_empty_returns_zero(void) {
    int storage[4];
    Stack s;
    stack_init(&s, storage, 4, sizeof(int));
    int out;
    ASSERT_EQ_INT(0, stack_pop(&s, &out));
}

static void test_peek_empty_returns_zero(void) {
    int storage[4];
    Stack s;
    stack_init(&s, storage, 4, sizeof(int));
    int out;
    ASSERT_EQ_INT(0, stack_peek(&s, &out));
}

/* ============================================================
 *  Push and pop — LIFO ordering
 * ============================================================ */

static void test_push_pop_one(void) {
    int storage[4];
    Stack s;
    stack_init(&s, storage, 4, sizeof(int));

    int in = 42, out = 0;
    ASSERT_EQ_INT(1, stack_push(&s, &in));
    ASSERT_EQ_INT(1, (int)stack_count(&s));
    ASSERT(!stack_empty(&s));

    ASSERT_EQ_INT(1, stack_pop(&s, &out));
    ASSERT_EQ_INT(42, out);
    ASSERT(stack_empty(&s));
}

static void test_lifo_ordering(void) {
    int storage[8];
    Stack s;
    stack_init(&s, storage, 8, sizeof(int));

    /* Push 0, 1, 2, 3, 4 — pop in reverse */
    for (int i = 0; i < 5; i++) stack_push(&s, &i);
    ASSERT_EQ_INT(5, (int)stack_count(&s));

    for (int i = 4; i >= 0; i--) {
        int out;
        ASSERT_EQ_INT(1, stack_pop(&s, &out));
        ASSERT_EQ_INT(i, out);
    }
    ASSERT(stack_empty(&s));
}

static void test_peek_does_not_consume(void) {
    int storage[4];
    Stack s;
    stack_init(&s, storage, 4, sizeof(int));

    int a = 1, b = 2, out = 0;
    stack_push(&s, &a);
    stack_push(&s, &b);

    ASSERT_EQ_INT(1, stack_peek(&s, &out));
    ASSERT_EQ_INT(2, out);   /* sees top */
    ASSERT_EQ_INT(2, (int)stack_count(&s));   /* didn't consume */

    out = 0;
    ASSERT_EQ_INT(1, stack_pop(&s, &out));
    ASSERT_EQ_INT(2, out);   /* same value pops */
}

/* ============================================================
 *  Reject-on-full
 * ============================================================ */

static void test_push_rejects_when_full(void) {
    int storage[3];
    Stack s;
    stack_init(&s, storage, 3, sizeof(int));

    for (int i = 0; i < 3; i++) {
        ASSERT_EQ_INT(1, stack_push(&s, &i));
    }
    ASSERT(stack_full(&s));

    int extra = 99;
    ASSERT_EQ_INT(0, stack_push(&s, &extra));
    ASSERT_EQ_INT(3, (int)stack_count(&s));   /* unchanged */

    /* The original three are still there in LIFO order. */
    int out;
    stack_pop(&s, &out); ASSERT_EQ_INT(2, out);
    stack_pop(&s, &out); ASSERT_EQ_INT(1, out);
    stack_pop(&s, &out); ASSERT_EQ_INT(0, out);
}

static void test_push_succeeds_after_pop(void) {
    int storage[2];
    Stack s;
    stack_init(&s, storage, 2, sizeof(int));

    int a = 1, b = 2;
    stack_push(&s, &a);
    stack_push(&s, &b);
    ASSERT(stack_full(&s));

    int c = 3;
    ASSERT_EQ_INT(0, stack_push(&s, &c));   /* still full, rejects */

    int out;
    stack_pop(&s, &out);                    /* makes room */
    ASSERT_EQ_INT(1, stack_push(&s, &c));   /* succeeds */
}

/* ============================================================
 *  Reset and reuse
 * ============================================================ */

static void test_reset(void) {
    int storage[8];
    Stack s;
    stack_init(&s, storage, 8, sizeof(int));

    for (int i = 0; i < 5; i++) stack_push(&s, &i);
    stack_reset(&s);

    ASSERT(stack_empty(&s));
    ASSERT_EQ_INT(0, (int)stack_count(&s));

    /* After reset, can push fresh items. */
    int x = 77;
    stack_push(&s, &x);
    int out;
    stack_pop(&s, &out);
    ASSERT_EQ_INT(77, out);
}

/* ============================================================
 *  Different element types
 * ============================================================ */

static void test_byte_elements(void) {
    uint8_t storage[16];
    Stack s;
    stack_init(&s, storage, 16, sizeof(uint8_t));

    for (uint8_t i = 0; i < 10; i++) stack_push(&s, &i);
    for (uint8_t i = 9; i != UINT8_MAX; i--) {  /* count down through 0 */
        uint8_t out;
        stack_pop(&s, &out);
        ASSERT_EQ_INT(i, out);
        if (i == 0) break;
    }
}

typedef struct { int a; int b; float c; } TestStruct;

static void test_struct_elements(void) {
    TestStruct storage[4];
    Stack s;
    stack_init(&s, storage, 4, sizeof(TestStruct));

    TestStruct in = { 1, 2, 3.5f };
    stack_push(&s, &in);

    TestStruct out = {0};
    ASSERT_EQ_INT(1, stack_pop(&s, &out));
    ASSERT_EQ_INT(1, out.a);
    ASSERT_EQ_INT(2, out.b);
    ASSERT(out.c == 3.5f);
}

int main(void) {
    TEST_SUITE("stack");

    RUN(test_init_empty);
    RUN(test_pop_empty_returns_zero);
    RUN(test_peek_empty_returns_zero);

    RUN(test_push_pop_one);
    RUN(test_lifo_ordering);
    RUN(test_peek_does_not_consume);

    RUN(test_push_rejects_when_full);
    RUN(test_push_succeeds_after_pop);

    RUN(test_reset);

    RUN(test_byte_elements);
    RUN(test_struct_elements);

    return TEST_SUITE_RESULT();
}
