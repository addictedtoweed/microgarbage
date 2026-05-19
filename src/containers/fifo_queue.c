/* ============================================================
 *  fifo_queue.c — thin wrapper over ring_buffer.
 *  See fifo_queue.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "containers/fifo_queue.h"

void fifo_init(FifoQueue *fifo, void *storage,
               size_t capacity, size_t element_size) {
    rb_init(&fifo->rb, storage, capacity, element_size);
}

void fifo_reset(FifoQueue *fifo) {
    rb_reset(&fifo->rb);
}

int fifo_push(FifoQueue *fifo, const void *element) {
    /* The only behavioral difference from ring_buffer: refuse on full. */
    if (rb_full(&fifo->rb)) return 0;
    rb_push(&fifo->rb, element);
    return 1;
}

int fifo_pop(FifoQueue *fifo, void *out) {
    return rb_pop(&fifo->rb, out);
}

int fifo_peek(const FifoQueue *fifo, void *out) {
    return rb_peek(&fifo->rb, out);
}

size_t fifo_count(const FifoQueue *fifo) { return rb_count(&fifo->rb); }
bool   fifo_empty(const FifoQueue *fifo) { return rb_empty(&fifo->rb); }
bool   fifo_full(const FifoQueue *fifo)  { return rb_full(&fifo->rb); }
