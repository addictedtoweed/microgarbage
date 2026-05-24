/* ============================================================
 *  test_spsc_ring.c — single-threaded mechanics tests
 *
 *  Verifies correctness of the ring's logic with no concurrency:
 *  init validation, push/pop FIFO order, full rejection, empty
 *  detection, wrap-around, the reserved-slot capacity convention,
 *  and reset. Concurrency/ordering is covered separately by
 *  test_spsc_ring_stress.c (run under ThreadSanitizer).
 *
 *  Build:
 *    cc -std=c11 -Iinclude -o t \
 *       src/containers/tests/test_spsc_ring.c src/containers/spsc_ring.c
 * ============================================================ */

#include "containers/spsc_ring.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

/* ---- init validation ---- */

static void test_init_rejects_bad_args(void) {
    SpscRing r;
    uint32_t buf[8];
    CHECK(!spsc_ring_init(NULL, buf, 8, 4), "init NULL ring rejected");
    CHECK(!spsc_ring_init(&r, NULL, 8, 4), "init NULL storage rejected");
    CHECK(!spsc_ring_init(&r, buf, 8, 0), "init zero elem_size rejected");
    CHECK(!spsc_ring_init(&r, buf, 0, 4), "init zero capacity rejected");
    CHECK(!spsc_ring_init(&r, buf, 1, 4), "init capacity 1 rejected");
    CHECK(!spsc_ring_init(&r, buf, 6, 4), "init non-power-of-two rejected");
    CHECK(spsc_ring_init(&r, buf, 8, 4), "init valid power-of-two accepted");
}

static void test_capacity_reserves_one_slot(void) {
    SpscRing r;
    uint32_t buf[8];
    spsc_ring_init(&r, buf, 8, sizeof(uint32_t));
    CHECK(spsc_ring_capacity(&r) == 7, "usable capacity is slots-1");
    CHECK(spsc_ring_empty(&r), "fresh ring is empty");
    CHECK(!spsc_ring_full(&r), "fresh ring is not full");
    CHECK(spsc_ring_count(&r) == 0, "fresh ring count 0");
}

/* ---- basic push/pop FIFO ---- */

static void test_push_pop_fifo_order(void) {
    SpscRing r;
    uint32_t buf[8];
    spsc_ring_init(&r, buf, 8, sizeof(uint32_t));

    for (uint32_t i = 0; i < 5; i++) {
        CHECK(spsc_ring_push(&r, &i), "push succeeds with room");
    }
    CHECK(spsc_ring_count(&r) == 5, "count after 5 pushes");

    for (uint32_t i = 0; i < 5; i++) {
        uint32_t out = 0xFFFFFFFF;
        CHECK(spsc_ring_pop(&r, &out), "pop succeeds with data");
        CHECK(out == i, "FIFO order preserved");
    }
    CHECK(spsc_ring_empty(&r), "empty after draining all");
}

/* ---- full rejection (no overwrite) ---- */

static void test_full_rejects_no_overwrite(void) {
    SpscRing r;
    uint32_t buf[4];                 /* 4 slots -> 3 usable */
    spsc_ring_init(&r, buf, 4, sizeof(uint32_t));

    uint32_t a = 10, b = 20, c = 30, d = 40;
    CHECK(spsc_ring_push(&r, &a), "push 1/3");
    CHECK(spsc_ring_push(&r, &b), "push 2/3");
    CHECK(spsc_ring_push(&r, &c), "push 3/3");
    CHECK(spsc_ring_full(&r), "full at usable capacity");
    CHECK(!spsc_ring_push(&r, &d), "push on full REJECTED");
    CHECK(spsc_ring_count(&r) == 3, "count unchanged after rejected push");

    /* The rejected element must not have clobbered the oldest. */
    uint32_t out = 0;
    CHECK(spsc_ring_pop(&r, &out) && out == 10, "oldest intact (no overwrite)");
}

/* ---- empty detection ---- */

static void test_pop_empty_returns_false(void) {
    SpscRing r;
    uint32_t buf[8];
    spsc_ring_init(&r, buf, 8, sizeof(uint32_t));
    uint32_t out = 0xABCD;
    CHECK(!spsc_ring_pop(&r, &out), "pop on empty returns false");
    CHECK(out == 0xABCD, "pop on empty leaves out untouched");
}

/* ---- wrap-around: indices cycle through storage many times ---- */

static void test_wrap_around(void) {
    SpscRing r;
    uint32_t buf[4];                 /* 3 usable */
    spsc_ring_init(&r, buf, 4, sizeof(uint32_t));

    /* Push 2, pop 2, repeated, for far more than capacity, so head
     * and tail wrap the storage repeatedly. */
    uint32_t next_push = 0, next_expect = 0;
    int ok = 1;
    for (int round = 0; round < 100; round++) {
        for (int k = 0; k < 2; k++) {
            if (!spsc_ring_push(&r, &next_push)) ok = 0;
            next_push++;
        }
        for (int k = 0; k < 2; k++) {
            uint32_t out = 0xFFFFFFFF;
            if (!spsc_ring_pop(&r, &out) || out != next_expect) ok = 0;
            next_expect++;
        }
    }
    CHECK(ok, "200 push/pop across wraps keep FIFO order + values");
    CHECK(spsc_ring_empty(&r), "empty after balanced wrap test");
}

/* ---- element size other than 4 (struct payload) ---- */

typedef struct { uint32_t a, b, c, d; } Msg16;

static void test_struct_payload(void) {
    SpscRing r;
    Msg16 buf[8];
    spsc_ring_init(&r, buf, 8, sizeof(Msg16));

    Msg16 in = { 1, 2, 3, 4 };
    CHECK(spsc_ring_push(&r, &in), "push struct payload");
    Msg16 out;
    memset(&out, 0, sizeof(out));
    CHECK(spsc_ring_pop(&r, &out), "pop struct payload");
    CHECK(out.a == 1 && out.b == 2 && out.c == 3 && out.d == 4,
          "struct payload round-trips intact");
}

/* ---- reset ---- */

static void test_reset_empties(void) {
    SpscRing r;
    uint32_t buf[8];
    spsc_ring_init(&r, buf, 8, sizeof(uint32_t));
    uint32_t v = 7;
    spsc_ring_push(&r, &v);
    spsc_ring_push(&r, &v);
    CHECK(spsc_ring_count(&r) == 2, "two before reset");
    spsc_ring_reset(&r);
    CHECK(spsc_ring_empty(&r), "empty after reset");
    CHECK(spsc_ring_count(&r) == 0, "count 0 after reset");
}

int main(void) {
    test_init_rejects_bad_args();
    test_capacity_reserves_one_slot();
    test_push_pop_fifo_order();
    test_full_rejects_no_overwrite();
    test_pop_empty_returns_false();
    test_wrap_around();
    test_struct_payload();
    test_reset_empties();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
