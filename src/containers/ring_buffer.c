/* ============================================================
 *  ring_buffer.c — implementation
 *  See ring_buffer.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "containers/ring_buffer.h"
#include <string.h>

/* Pointer arithmetic to slot N of the storage buffer, byte-correct
 * for arbitrary element sizes. Cast through char* so we can do
 * (slot * element_size) byte offsets safely. */
static void *slot_at(RingBuffer *rb, size_t slot) {
    return (char*)rb->storage + slot * rb->element_size;
}

static const void *slot_at_const(const RingBuffer *rb, size_t slot) {
    return (const char*)rb->storage + slot * rb->element_size;
}

void rb_init(RingBuffer *rb, void *storage,
             size_t capacity, size_t element_size) {
    rb->storage      = storage;
    rb->capacity     = capacity;
    rb->element_size = element_size;
    rb->head         = 0;
    rb->tail         = 0;
    rb->count        = 0;
}

void rb_reset(RingBuffer *rb) {
    rb->head  = 0;
    rb->tail  = 0;
    rb->count = 0;
}

void rb_push(RingBuffer *rb, const void *element) {
    memcpy(slot_at(rb, rb->head), element, rb->element_size);
    rb->head = (rb->head + 1) % rb->capacity;

    if (rb->count == rb->capacity) {
        /* Overwrite: advance tail to drop the now-oldest element. */
        rb->tail = (rb->tail + 1) % rb->capacity;
    } else {
        rb->count++;
    }
}

int rb_pop(RingBuffer *rb, void *out) {
    if (rb->count == 0) return 0;
    memcpy(out, slot_at(rb, rb->tail), rb->element_size);
    rb->tail = (rb->tail + 1) % rb->capacity;
    rb->count--;
    return 1;
}

int rb_peek(const RingBuffer *rb, void *out) {
    if (rb->count == 0) return 0;
    memcpy(out, slot_at_const(rb, rb->tail), rb->element_size);
    return 1;
}

int rb_peek_at(const RingBuffer *rb, size_t offset, void *out) {
    if (offset >= rb->count) return 0;
    size_t slot = (rb->tail + offset) % rb->capacity;
    memcpy(out, slot_at_const(rb, slot), rb->element_size);
    return 1;
}

size_t rb_count(const RingBuffer *rb) {
    return rb->count;
}

bool rb_empty(const RingBuffer *rb) {
    return rb->count == 0;
}

bool rb_full(const RingBuffer *rb) {
    return rb->count == rb->capacity;
}
