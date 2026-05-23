/* ============================================================
 *  dlist.c — doubly-linked list over a caller-provided node pool
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "containers/dlist.h"
#include <string.h>

/* Node layout in the pool: [uint32 next][uint32 prev][payload].
 * We address nodes by index; helpers convert index <-> byte offset
 * and expose the link fields and payload. */

static inline uint8_t *node_at(const DList *l, uint32_t i) {
    return l->pool + (size_t)i * l->stride;
}
static inline uint32_t *node_next(const DList *l, uint32_t i) {
    return (uint32_t *)(void *)node_at(l, i);
}
static inline uint32_t *node_prev(const DList *l, uint32_t i) {
    return (uint32_t *)(void *)(node_at(l, i) + sizeof(uint32_t));
}
static inline void *node_payload(const DList *l, uint32_t i) {
    return node_at(l, i) + DLIST_NODE_OVERHEAD;
}

size_t dlist_pool_bytes(size_t capacity, size_t element_size) {
    return capacity * (DLIST_NODE_OVERHEAD + element_size);
}

bool dlist_init(DList *l, void *pool, size_t pool_bytes,
                size_t element_size) {
    if (!l || !pool || element_size == 0) return false;

    size_t stride = DLIST_NODE_OVERHEAD + element_size;
    size_t capacity = pool_bytes / stride;
    if (capacity == 0) return false;
    /* Indices are 32-bit; DLIST_NIL is the sentinel, so capacity must
     * stay below it. */
    if (capacity >= DLIST_NIL) return false;

    l->pool         = (uint8_t *)pool;
    l->capacity     = capacity;
    l->element_size = element_size;
    l->stride       = stride;
    dlist_clear(l);
    return true;
}

void dlist_clear(DList *l) {
    if (!l) return;
    l->count     = 0;
    l->head      = DLIST_NIL;
    l->tail      = DLIST_NIL;
    /* Thread every node onto the free list: 0 -> 1 -> ... -> last. */
    l->free_head = (l->capacity > 0) ? 0u : DLIST_NIL;
    for (uint32_t i = 0; i < l->capacity; i++) {
        *node_next(l, i) = (i + 1 < l->capacity) ? (i + 1) : DLIST_NIL;
    }
}

/* Pull a node off the free list, or DLIST_NIL if exhausted. */
static uint32_t alloc_node(DList *l) {
    uint32_t i = l->free_head;
    if (i == DLIST_NIL) return DLIST_NIL;
    l->free_head = *node_next(l, i);
    return i;
}

/* Return a node to the free list. */
static void free_node(DList *l, uint32_t i) {
    *node_next(l, i) = l->free_head;
    l->free_head = i;
}

int dlist_push_front(DList *l, const void *elem) {
    if (!l || !elem) return 0;
    uint32_t n = alloc_node(l);
    if (n == DLIST_NIL) return 0;

    memcpy(node_payload(l, n), elem, l->element_size);
    *node_prev(l, n) = DLIST_NIL;
    *node_next(l, n) = l->head;

    if (l->head != DLIST_NIL) *node_prev(l, l->head) = n;
    else                      l->tail = n;            /* was empty */
    l->head = n;
    l->count++;
    return 1;
}

int dlist_push_back(DList *l, const void *elem) {
    if (!l || !elem) return 0;
    uint32_t n = alloc_node(l);
    if (n == DLIST_NIL) return 0;

    memcpy(node_payload(l, n), elem, l->element_size);
    *node_next(l, n) = DLIST_NIL;
    *node_prev(l, n) = l->tail;

    if (l->tail != DLIST_NIL) *node_next(l, l->tail) = n;
    else                      l->head = n;            /* was empty */
    l->tail = n;
    l->count++;
    return 1;
}

int dlist_pop_front(DList *l, void *out) {
    if (!l || l->head == DLIST_NIL) return 0;
    uint32_t n = l->head;
    if (out) memcpy(out, node_payload(l, n), l->element_size);

    l->head = *node_next(l, n);
    if (l->head != DLIST_NIL) *node_prev(l, l->head) = DLIST_NIL;
    else                      l->tail = DLIST_NIL;    /* now empty */
    free_node(l, n);
    l->count--;
    return 1;
}

int dlist_pop_back(DList *l, void *out) {
    if (!l || l->tail == DLIST_NIL) return 0;
    uint32_t n = l->tail;
    if (out) memcpy(out, node_payload(l, n), l->element_size);

    l->tail = *node_prev(l, n);
    if (l->tail != DLIST_NIL) *node_next(l, l->tail) = DLIST_NIL;
    else                      l->head = DLIST_NIL;    /* now empty */
    free_node(l, n);
    l->count--;
    return 1;
}

int dlist_front(const DList *l, void *out) {
    if (!l || l->head == DLIST_NIL) return 0;
    if (out) memcpy(out, node_payload(l, l->head), l->element_size);
    return 1;
}

int dlist_back(const DList *l, void *out) {
    if (!l || l->tail == DLIST_NIL) return 0;
    if (out) memcpy(out, node_payload(l, l->tail), l->element_size);
    return 1;
}

size_t dlist_count(const DList *l) { return l ? l->count : 0; }
bool   dlist_empty(const DList *l) { return !l || l->count == 0; }
bool   dlist_full(const DList *l)  { return l && l->count >= l->capacity; }

uint32_t dlist_begin(const DList *l)  { return l ? l->head : DLIST_NIL; }
uint32_t dlist_rbegin(const DList *l) { return l ? l->tail : DLIST_NIL; }

uint32_t dlist_next(const DList *l, uint32_t cursor) {
    if (!l || cursor == DLIST_NIL || cursor >= l->capacity) return DLIST_NIL;
    return *node_next(l, cursor);
}
uint32_t dlist_prev(const DList *l, uint32_t cursor) {
    if (!l || cursor == DLIST_NIL || cursor >= l->capacity) return DLIST_NIL;
    return *node_prev(l, cursor);
}
int dlist_get(const DList *l, uint32_t cursor, void *out) {
    if (!l || cursor == DLIST_NIL || cursor >= l->capacity) return 0;
    if (out) memcpy(out, node_payload(l, cursor), l->element_size);
    return 1;
}
