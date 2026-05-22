/* ============================================================
 *  spsc_ring.h — lock-free single-producer/single-consumer ring
 *
 *  A bounded FIFO for exactly ONE producer context and ONE
 *  consumer context running concurrently. Unlike containers/
 *  ring_buffer (which is single-context only — it tracks fullness
 *  in a shared `count` field that both push and pop mutate, racing
 *  under concurrency), this ring:
 *
 *    - Derives full/empty from head and tail ALONE. The producer
 *      owns `head`; the consumer owns `tail`. Neither writes the
 *      other's index. There is no shared mutable counter, so the
 *      only cross-context communication is the single-writer
 *      publication of each index.
 *
 *    - Uses C11 acquire/release ordering on the index loads/stores
 *      so the payload write is visible before the index that
 *      publishes it, and the payload read happens after the index
 *      that exposes it. This is what makes it correct without a
 *      lock.
 *
 *    - REJECTS on full (push returns false). A message channel must
 *      never silently drop a request, so there is deliberately no
 *      overwrite-oldest behaviour here.
 *
 *  ---------------------------------------------------------------
 *  Capacity convention
 *  ---------------------------------------------------------------
 *
 *  One slot is reserved to disambiguate full from empty without a
 *  count, so a ring created with capacity N holds up to N-1 live
 *  elements. spsc_ring_init rounds nothing — you ask for the slot
 *  count; usable capacity is that minus one. (Callers that want a
 *  usable depth D should size storage for D+1 slots.)
 *
 *  Capacity MUST be a power of two. Indices advance monotonically
 *  conceptually but are masked to the storage range; a power-of-two
 *  capacity makes the mask a single AND. spsc_ring_init returns
 *  false if capacity is not a power of two or is < 2.
 *
 *  ---------------------------------------------------------------
 *  Concurrency contract (READ THIS)
 *  ---------------------------------------------------------------
 *
 *  - Exactly one thread/core may call spsc_ring_push (the producer).
 *  - Exactly one thread/core may call spsc_ring_pop (the consumer).
 *  - Producer and consumer may run concurrently. That is the whole
 *    point.
 *  - spsc_ring_count / _empty / _full are best-effort observers,
 *    safe to call from either side, but the value may be stale the
 *    instant it is read (the other side may advance). Use them for
 *    diagnostics and backpressure heuristics, not for correctness
 *    decisions that assume the value is still true afterward.
 *  - spsc_ring_init and spsc_ring_reset are NOT concurrency-safe.
 *    Call them when no producer or consumer is running (setup /
 *    teardown).
 *
 *  ---------------------------------------------------------------
 *  Cross-core note (STM32H745 M7<->M4)
 *  ---------------------------------------------------------------
 *
 *  Acquire/release stops the compiler and a coherent CPU from
 *  reordering. On the H745 the two cores have SEPARATE caches over
 *  shared SRAM, which acquire/release alone does NOT handle. For
 *  cross-core use the ring storage + this struct must live in a
 *  non-cacheable MPU region (recommended), or the producer/consumer
 *  must clean/invalidate around each access. That is a transport-
 *  layer concern (see docs/intercore-channel.md), not this ring's
 *  job — this ring provides the ordering; the placement provides
 *  the coherence. On the desktop (coherent caches) acquire/release
 *  is sufficient on its own.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef SPSC_RING_H
#define SPSC_RING_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>

/* ============================================================
 *  Ring structure
 *
 *  Caller provides storage of at least capacity * element_size
 *  bytes, which must outlive the ring.
 *
 *  head: next push index, written ONLY by the producer.
 *  tail: next pop index,  written ONLY by the consumer.
 *  Both are read by both sides (acquire) and published by their
 *  owner (release).
 * ============================================================ */

typedef struct {
    void   *storage;            /* caller-owned, capacity*elem_size bytes */
    size_t  capacity;           /* slot count, power of two               */
    size_t  mask;               /* capacity - 1                           */
    size_t  element_size;       /* bytes per element                      */
    _Atomic size_t head;        /* producer-owned publish index           */
    _Atomic size_t tail;        /* consumer-owned publish index           */
} SpscRing;

/* Initialize over caller storage. capacity must be a power of two
 * >= 2; usable depth is capacity - 1. Returns false on bad args
 * (NULL, non-power-of-two capacity, capacity < 2, zero elem size).
 * NOT concurrency-safe — call before any producer/consumer runs. */
bool spsc_ring_init(SpscRing *r, void *storage,
                    size_t capacity, size_t element_size);

/* Empty the ring (resets head and tail to 0). NOT concurrency-safe;
 * call only when neither side is running. */
void spsc_ring_reset(SpscRing *r);

/* Producer-only. Copy one element (element_size bytes) into the
 * ring. Returns true on success, false if the ring is full (the
 * element is NOT written and NOT dropped from the caller — the
 * caller still owns it and may retry / back off). */
bool spsc_ring_push(SpscRing *r, const void *element);

/* Consumer-only. Copy the oldest element into *out (element_size
 * bytes) and advance. Returns true on success, false if the ring
 * is empty (*out untouched). */
bool spsc_ring_pop(SpscRing *r, void *out);

/* Best-effort observers (either side; value may be stale at once).
 * count is the number of live elements; empty/full are convenience
 * predicates. */
size_t spsc_ring_count(const SpscRing *r);
bool   spsc_ring_empty(const SpscRing *r);
bool   spsc_ring_full(const SpscRing *r);

/* Usable capacity (slot count minus the one reserved slot). */
size_t spsc_ring_capacity(const SpscRing *r);

#endif /* SPSC_RING_H */
