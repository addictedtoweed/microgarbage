/* ============================================================
 *  ring_buffer.h — fixed-capacity circular buffer
 *
 *  Caller-provided storage. The buffer struct is public so the
 *  user can allocate it on the stack, in static memory, or
 *  wherever they like. The element storage is also caller-
 *  provided: pass a pointer to a buffer of (capacity * element_size)
 *  bytes to rb_init.
 *
 *  Payloads: any type, identified by element_size at init. Pushes
 *  and pops copy element_size bytes via memcpy.
 *
 *  Drop policy: rb_push always succeeds. When the buffer is full,
 *  the oldest element is overwritten. If you want reject-on-full
 *  semantics instead, use fifo_queue (which wraps this module).
 *
 *  Thread safety: none. The single-producer/single-consumer
 *  variant of a ring buffer is the standard pattern for lock-free
 *  use; this implementation is NOT that — it's a sequential one
 *  that's safe only from a single context.
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    void   *storage;        /* caller-owned buffer, capacity*elem_size bytes */
    size_t  capacity;       /* in elements                                   */
    size_t  element_size;   /* in bytes                                      */
    size_t  head;           /* index of next push position                   */
    size_t  tail;           /* index of next pop position                    */
    size_t  count;          /* current number of elements                    */
} RingBuffer;

/* Initialize a ring buffer over caller-provided storage. The
 * `storage` buffer must be at least (capacity * element_size)
 * bytes and must outlive the RingBuffer. */
void rb_init(RingBuffer *rb, void *storage,
             size_t capacity, size_t element_size);

/* Reset the buffer to empty without touching the storage. Useful
 * for "flush all" operations. */
void rb_reset(RingBuffer *rb);

/* Push one element. Always succeeds; if the buffer is full, the
 * oldest element is overwritten and the tail advances. */
void rb_push(RingBuffer *rb, const void *element);

/* Pop one element into *out. Returns 1 on success, 0 if the
 * buffer was empty (in which case *out is untouched). */
int rb_pop(RingBuffer *rb, void *out);

/* Peek at the front (oldest) element without removing it. Returns
 * 1 on success, 0 if empty. */
int rb_peek(const RingBuffer *rb, void *out);

/* Peek at the element at `offset` from the front (0 = front, same
 * as rb_peek). Useful when you need to look ahead without consuming
 * (e.g., for interpolation). Returns 1 on success, 0 if offset is
 * out of range (offset >= count). */
int rb_peek_at(const RingBuffer *rb, size_t offset, void *out);

/* Current count. Same as reading rb->count directly. */
size_t rb_count(const RingBuffer *rb);

/* Convenience predicates. */
bool rb_empty(const RingBuffer *rb);
bool rb_full(const RingBuffer *rb);

#endif /* RING_BUFFER_H */
