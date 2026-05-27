/* ============================================================
 *  test_spsc_ring_stress.c — concurrent producer/consumer stress
 *
 *  The mechanics test (test_spsc_ring.c) runs single-threaded and
 *  cannot validate the memory ordering — acquire/release only
 *  matters under genuine concurrency. This test runs a real
 *  producer thread and a real consumer thread against one ring and
 *  checks two things:
 *
 *    1. CORRECTNESS: every value the producer sends is received by
 *       the consumer exactly once, in order. The producer sends a
 *       monotonic sequence; the consumer asserts each popped value
 *       is exactly the previous + 1. A torn read (payload visible
 *       before / without its index publication) would corrupt a
 *       value and fail this.
 *
 *    2. RACE-FREEDOM: run under ThreadSanitizer:
 *         cc -std=c11 -fsanitize=thread -Iinclude -o t \
 *            src/containers/tests/test_spsc_ring_stress.c \
 *            src/containers/spsc_ring.c -lpthread
 *       TSan flags any data race on the ring's memory. A correct
 *       SPSC ring (single writer per index, acquire/release) is
 *       race-free; TSan passing is the real proof the ordering is
 *       right.
 *
 *  The ring is intentionally small relative to the message count so
 *  it fills and drains many times — exercising the full/empty
 *  boundaries and wrap-around under contention, with both sides
 *  spinning (retry on full / empty) the way the real channel will.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "containers/spsc_ring.h"

#include <stdio.h>
#include <stdint.h>
#include <sched.h>
#include <pthread.h>

#define RING_SLOTS    64          /* 63 usable — small on purpose */
#define N_MESSAGES    2000000u    /* enough to wrap thousands of times */

/* Spin backoff. A raw tight spin on the atomics (no yield) causes
 * severe cache-line contention between the two cores — the head/tail
 * line ping-pongs and BOTH threads crawl. That's a property of naive
 * busy-waiting, not of the ring. Real callers never raw-spin: the
 * ServiceChannel's wait() blocks on a condvar (desktop) or WFE/HSEM
 * (MCU). Here we approximate that with sched_yield() after a few
 * failed tries, which keeps the test fast while still exercising the
 * full/empty boundaries under genuine concurrency. */
#define SPIN_BACKOFF_TRIES  256
static inline void backoff(int *tries) {
    if (++(*tries) >= SPIN_BACKOFF_TRIES) { sched_yield(); *tries = 0; }
}

typedef struct {
    SpscRing *ring;
} Args;

/* Producer: send 0,1,2,...,N-1 as fast as possible, spinning on full. */
static void *producer(void *arg) {
    Args *a = (Args *)arg;
    int tries = 0;
    for (uint32_t i = 0; i < N_MESSAGES; i++) {
        while (!spsc_ring_push(a->ring, &i)) {
            backoff(&tries);   /* full — yield-back-off until room */
        }
        tries = 0;
    }
    return NULL;
}

/* Consumer: receive N values, assert strict monotonic +1 order. */
static int g_order_ok = 1;

static void *consumer(void *arg) {
    Args *a = (Args *)arg;
    uint32_t expect = 0;
    int tries = 0;
    for (uint32_t i = 0; i < N_MESSAGES; i++) {
        uint32_t got;
        while (!spsc_ring_pop(a->ring, &got)) {
            backoff(&tries);   /* empty — yield-back-off until data */
        }
        tries = 0;
        if (got != expect) {
            g_order_ok = 0;
            printf("  FAIL  ordering: expected %u got %u at index %u\n",
                   expect, got, i);
            return NULL;
        }
        expect++;
    }
    return NULL;
}

int main(void) {
    SpscRing ring;
    uint32_t storage[RING_SLOTS];
    if (!spsc_ring_init(&ring, storage, RING_SLOTS, sizeof(uint32_t))) {
        printf("  FAIL  init\n");
        return 1;
    }

    Args args = { &ring };
    pthread_t tp, tc;

    /* Consumer first so it's ready, then producer. */
    pthread_create(&tc, NULL, consumer, &args);
    pthread_create(&tp, NULL, producer, &args);

    pthread_join(tp, NULL);
    pthread_join(tc, NULL);

    if (g_order_ok) {
        printf("1 passed, 0 failed (%u messages, ring %d slots)\n",
               N_MESSAGES, RING_SLOTS);
        return 0;
    } else {
        printf("0 passed, 1 failed\n");
        return 1;
    }
}
