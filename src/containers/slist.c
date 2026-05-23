/* ============================================================
 *  slist.c — singly-linked list over a caller-provided node pool
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "containers/slist.h"
#include <string.h>

/* Node layout: [uint32 next][payload]. Index-addressed. */

static inline uint8_t *node_at(const SList *l, uint32_t i) {
    return l->pool + (size_t)i * l->stride;
}
static inline uint32_t *node_next(const SList *l, uint32_t i) {
    return (uint32_t *)(void *)node_at(l, i);
}
static inline void *node_payload(const SList *l, uint32_t i) {
    return node_at(l, i) + SLIST_NODE_OVERHEAD;
}

size_t slist_pool_bytes(size_t capacity, size_t element_size) {
    return capacity * (SLIST_NODE_OVERHEAD + element_size);
}

bool slist_init(SList *l, void *pool, size_t pool_bytes,
                size_t element_size) {
    if (!l || !pool || element_size == 0) return false;

    size_t stride = SLIST_NODE_OVERHEAD + element_size;
    size_t capacity = pool_bytes / stride;
    if (capacity == 0) return false;
    if (capacity >= SLIST_NIL) return false;

    l->pool         = (uint8_t *)pool;
    l->capacity     = capacity;
    l->element_size = element_size;
    l->stride       = stride;
    slist_clear(l);
    return true;
}

void slist_clear(SList *l) {
    if (!l) return;
    l->count     = 0;
    l->head      = SLIST_NIL;
    l->tail      = SLIST_NIL;
    l->free_head = (l->capacity > 0) ? 0u : SLIST_NIL;
    for (uint32_t i = 0; i < l->capacity; i++) {
        *node_next(l, i) = (i + 1 < l->capacity) ? (i + 1) : SLIST_NIL;
    }
}

static uint32_t alloc_node(SList *l) {
    uint32_t i = l->free_head;
    if (i == SLIST_NIL) return SLIST_NIL;
    l->free_head = *node_next(l, i);
    return i;
}

static void free_node(SList *l, uint32_t i) {
    *node_next(l, i) = l->free_head;
    l->free_head = i;
}

int slist_push_front(SList *l, const void *elem) {
    if (!l || !elem) return 0;
    uint32_t n = alloc_node(l);
    if (n == SLIST_NIL) return 0;

    memcpy(node_payload(l, n), elem, l->element_size);
    *node_next(l, n) = l->head;
    l->head = n;
    if (l->tail == SLIST_NIL) l->tail = n;   /* was empty */
    l->count++;
    return 1;
}

int slist_push_back(SList *l, const void *elem) {
    if (!l || !elem) return 0;
    uint32_t n = alloc_node(l);
    if (n == SLIST_NIL) return 0;

    memcpy(node_payload(l, n), elem, l->element_size);
    *node_next(l, n) = SLIST_NIL;
    if (l->tail != SLIST_NIL) *node_next(l, l->tail) = n;
    else                      l->head = n;   /* was empty */
    l->tail = n;
    l->count++;
    return 1;
}

int slist_pop_front(SList *l, void *out) {
    if (!l || l->head == SLIST_NIL) return 0;
    uint32_t n = l->head;
    if (out) memcpy(out, node_payload(l, n), l->element_size);

    l->head = *node_next(l, n);
    if (l->head == SLIST_NIL) l->tail = SLIST_NIL;   /* now empty */
    free_node(l, n);
    l->count--;
    return 1;
}

int slist_pop_back(SList *l, void *out) {
    if (!l || l->tail == SLIST_NIL) return 0;
    uint32_t n = l->tail;
    if (out) memcpy(out, node_payload(l, n), l->element_size);

    /* O(n): find the node whose next is the tail (the new tail). */
    if (l->head == l->tail) {
        /* single element */
        l->head = SLIST_NIL;
        l->tail = SLIST_NIL;
    } else {
        uint32_t prev = l->head;
        while (*node_next(l, prev) != n) prev = *node_next(l, prev);
        *node_next(l, prev) = SLIST_NIL;
        l->tail = prev;
    }
    free_node(l, n);
    l->count--;
    return 1;
}

int slist_front(const SList *l, void *out) {
    if (!l || l->head == SLIST_NIL) return 0;
    if (out) memcpy(out, node_payload(l, l->head), l->element_size);
    return 1;
}

int slist_back(const SList *l, void *out) {
    if (!l || l->tail == SLIST_NIL) return 0;
    if (out) memcpy(out, node_payload(l, l->tail), l->element_size);
    return 1;
}

size_t slist_count(const SList *l) { return l ? l->count : 0; }
bool   slist_empty(const SList *l) { return !l || l->count == 0; }
bool   slist_full(const SList *l)  { return l && l->count >= l->capacity; }

uint32_t slist_begin(const SList *l) { return l ? l->head : SLIST_NIL; }

uint32_t slist_next(const SList *l, uint32_t cursor) {
    if (!l || cursor == SLIST_NIL || cursor >= l->capacity) return SLIST_NIL;
    return *node_next(l, cursor);
}
int slist_get(const SList *l, uint32_t cursor, void *out) {
    if (!l || cursor == SLIST_NIL || cursor >= l->capacity) return 0;
    if (out) memcpy(out, node_payload(l, cursor), l->element_size);
    return 1;
}
