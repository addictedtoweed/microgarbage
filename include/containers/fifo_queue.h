/* ============================================================
 *  fifo_queue.h — bounded FIFO queue
 *
 *  Wraps ring_buffer with reject-on-full semantics. The ring
 *  buffer overwrites the oldest element when full; this FIFO
 *  refuses the new element instead. Use the FIFO when you can't
 *  afford to lose items (work queues, message queues). Use the
 *  ring buffer directly when newer data is more valuable than
 *  older (audio sample streams, sensor history).
 *
 *  Caller-provided storage, same as ring_buffer.
 *
 *  Depends on: ring_buffer
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef FIFO_QUEUE_H
#define FIFO_QUEUE_H

#include "containers/ring_buffer.h"

typedef struct {
    RingBuffer rb;
} FifoQueue;

/* Initialize over caller-provided storage. Same as rb_init. */
void fifo_init(FifoQueue *fifo, void *storage,
               size_t capacity, size_t element_size);

/* Reset to empty without touching storage. */
void fifo_reset(FifoQueue *fifo);

/* Push one element. Returns 1 on success, 0 if the queue is
 * full (the element is NOT inserted — caller can retry later
 * or drop). */
int fifo_push(FifoQueue *fifo, const void *element);

/* Pop one element into *out. Returns 1 on success, 0 if empty. */
int fifo_pop(FifoQueue *fifo, void *out);

/* Peek at the front element without removing. Returns 1 on
 * success, 0 if empty. */
int fifo_peek(const FifoQueue *fifo, void *out);

size_t fifo_count(const FifoQueue *fifo);
bool   fifo_empty(const FifoQueue *fifo);
bool   fifo_full(const FifoQueue *fifo);

#endif /* FIFO_QUEUE_H */
