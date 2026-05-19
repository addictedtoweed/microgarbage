/* ============================================================
 *  stack.c — implementation
 *  See stack.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "containers/stack.h"
#include <string.h>

static void *slot_at(Stack *s, size_t slot) {
    return (char*)s->storage + slot * s->element_size;
}

static const void *slot_at_const(const Stack *s, size_t slot) {
    return (const char*)s->storage + slot * s->element_size;
}

void stack_init(Stack *s, void *storage,
                size_t capacity, size_t element_size) {
    s->storage      = storage;
    s->capacity     = capacity;
    s->element_size = element_size;
    s->top          = 0;
}

void stack_reset(Stack *s) {
    s->top = 0;
}

int stack_push(Stack *s, const void *element) {
    if (s->top == s->capacity) return 0;
    memcpy(slot_at(s, s->top), element, s->element_size);
    s->top++;
    return 1;
}

int stack_pop(Stack *s, void *out) {
    if (s->top == 0) return 0;
    s->top--;
    memcpy(out, slot_at(s, s->top), s->element_size);
    return 1;
}

int stack_peek(const Stack *s, void *out) {
    if (s->top == 0) return 0;
    memcpy(out, slot_at_const(s, s->top - 1), s->element_size);
    return 1;
}

size_t stack_count(const Stack *s) {
    return s->top;
}

bool stack_empty(const Stack *s) {
    return s->top == 0;
}

bool stack_full(const Stack *s) {
    return s->top == s->capacity;
}
