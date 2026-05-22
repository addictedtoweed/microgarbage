/* ============================================================
 *  spsc_ring.c — lock-free SPSC ring implementation
 *
 *  Ordering rationale (the whole correctness argument):
 *
 *  PUSH (producer):
 *    1. load head  — relaxed. Only the producer writes head, so its
 *       own most recent value is trivially visible to itself.
 *    2. load tail  — ACQUIRE. We need to see the consumer's latest
 *       tail to know if there's room, and acquire pairs with the
 *       consumer's release store of tail so any slot the consumer
 *       freed is seen as free.
 *    3. if full, bail.
 *    4. write the element into storage[head].
 *    5. store head = head+1 — RELEASE. This publishes the element:
 *       the release ensures step 4's write is visible to a consumer
 *       that later does an acquire-load of head before reading the
 *       slot.
 *
 *  POP (consumer):
 *    1. load tail  — relaxed. Only the consumer writes tail.
 *    2. load head  — ACQUIRE. Pairs with the producer's release
 *       store of head; if we observe the new head, we also observe
 *       the element write that preceded it.
 *    3. if empty, bail.
 *    4. read the element from storage[tail].
 *    5. store tail = tail+1 — RELEASE. Publishes that the slot is
 *       free; pairs with the producer's acquire-load of tail.
 *
 *  This is the textbook Lamport/SPSC pattern. The reserved-slot
 *  full test (head+1 == tail, modulo capacity) avoids needing a
 *  separate count.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "containers/spsc_ring.h"

#include <string.h>

/* ---- helpers ---- */

static bool is_pow2(size_t x) {
    return x >= 2 && (x & (x - 1)) == 0;
}

/* ---- lifecycle (not concurrency-safe) ---- */

bool spsc_ring_init(SpscRing *r, void *storage,
                    size_t capacity, size_t element_size) {
    if (!r || !storage || element_size == 0) return false;
    if (!is_pow2(capacity)) return false;

    r->storage      = storage;
    r->capacity     = capacity;
    r->mask         = capacity - 1;
    r->element_size = element_size;
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
    return true;
}

void spsc_ring_reset(SpscRing *r) {
    if (!r) return;
    atomic_store_explicit(&r->head, 0, memory_order_relaxed);
    atomic_store_explicit(&r->tail, 0, memory_order_relaxed);
}

/* ---- producer ---- */

bool spsc_ring_push(SpscRing *r, const void *element) {
    if (!r || !element) return false;

    const size_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    const size_t next = (head + 1) & r->mask;
    const size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);

    if (next == tail) {
        return false;   /* full — one slot reserved to distinguish from empty */
    }

    void *slot = (char *)r->storage + head * r->element_size;
    memcpy(slot, element, r->element_size);

    atomic_store_explicit(&r->head, next, memory_order_release);
    return true;
}

/* ---- consumer ---- */

bool spsc_ring_pop(SpscRing *r, void *out) {
    if (!r || !out) return false;

    const size_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    const size_t head = atomic_load_explicit(&r->head, memory_order_acquire);

    if (tail == head) {
        return false;   /* empty */
    }

    const void *slot = (const char *)r->storage + tail * r->element_size;
    memcpy(out, slot, r->element_size);

    atomic_store_explicit(&r->tail, (tail + 1) & r->mask, memory_order_release);
    return true;
}

/* ---- observers (best-effort) ---- */

size_t spsc_ring_count(const SpscRing *r) {
    if (!r) return 0;
    const size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    const size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return (head - tail) & r->mask;
}

bool spsc_ring_empty(const SpscRing *r) {
    if (!r) return true;
    const size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    const size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return head == tail;
}

bool spsc_ring_full(const SpscRing *r) {
    if (!r) return false;
    const size_t head = atomic_load_explicit(&r->head, memory_order_acquire);
    const size_t tail = atomic_load_explicit(&r->tail, memory_order_acquire);
    return ((head + 1) & r->mask) == tail;
}

size_t spsc_ring_capacity(const SpscRing *r) {
    if (!r) return 0;
    return r->capacity - 1;   /* one slot reserved */
}
