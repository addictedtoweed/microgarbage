/* ============================================================
 *  stack.h — fixed-capacity LIFO stack
 *
 *  Caller-provided storage. The Stack struct is public so the user
 *  can place it on the stack, in static memory, or wherever they
 *  like. The element storage is also caller-provided: pass a pointer
 *  to a buffer of (capacity * element_size) bytes to stack_init.
 *
 *  Payloads: any type, identified by element_size at init. Pushes
 *  and pops copy element_size bytes via memcpy.
 *
 *  Drop policy: stack_push rejects when full (returns 0). The only
 *  alternative would be growing the buffer, which isn't compatible
 *  with caller-provided storage. If you need a growing stack, build
 *  one over hashtable's allocator pattern or just size your storage
 *  generously up front.
 *
 *  Thread safety: none. Single-context use only.
 *
 *  Depends on: nothing
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef STACK_H
#define STACK_H

#include <stddef.h>
#include <stdbool.h>

typedef struct {
    void   *storage;        /* caller-owned buffer, capacity*elem_size bytes */
    size_t  capacity;       /* in elements                                   */
    size_t  element_size;   /* in bytes                                      */
    size_t  top;            /* index of next push slot (== count)            */
} Stack;

/* Initialize a stack over caller-provided storage. The `storage`
 * buffer must be at least (capacity * element_size) bytes and must
 * outlive the Stack. */
void stack_init(Stack *s, void *storage,
                size_t capacity, size_t element_size);

/* Reset the stack to empty without touching the storage. */
void stack_reset(Stack *s);

/* Push one element. Returns 1 on success, 0 if the stack is full
 * (the element is NOT inserted — caller can resize their own
 * storage and re-init, or just drop). */
int stack_push(Stack *s, const void *element);

/* Pop one element into *out. Returns 1 on success, 0 if empty
 * (in which case *out is untouched). */
int stack_pop(Stack *s, void *out);

/* Peek at the top element without removing it. Returns 1 on
 * success, 0 if empty. */
int stack_peek(const Stack *s, void *out);

/* Current count. Same as reading s->top directly. */
size_t stack_count(const Stack *s);

/* Convenience predicates. */
bool stack_empty(const Stack *s);
bool stack_full(const Stack *s);

#endif /* STACK_H */
