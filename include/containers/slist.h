/* ============================================================
 *  slist.h — singly-linked list over a caller-provided node pool
 *
 *  The lean list: one link per node (vs dlist's two), so each node
 *  costs sizeof(uint32_t) + element_size. Meant for memory-constrained
 *  use (e.g. the VM/guest side, where every byte of the data region
 *  counts). If you need backward iteration or O(1) tail removal, use
 *  dlist instead — this trades those away for the smaller node.
 *
 *  Caller-provided storage, no malloc. You give slist a pool of nodes
 *  (a plain byte buffer); the list hands them out on insert and
 *  reclaims them on remove via an internal free list. When the pool is
 *  exhausted, inserts fail (return 0).
 *
 *  Operations are O(1) at the FRONT (push_front / pop_front) and at the
 *  BACK for push (a tail index is kept), but pop_back is O(n) — a
 *  singly-linked list can't reach the previous node cheaply. If you pop
 *  from the back a lot, use dlist.
 *
 *  Payloads: any type, identified by element_size at init; copied via
 *  memcpy.
 *
 *  Sizing:
 *      uint8_t pool[SLIST_POOL_BYTES(64, sizeof(MyThing))];
 *      SList list;
 *      slist_init(&list, pool, sizeof pool, sizeof(MyThing));
 *  SLIST_POOL_BYTES() is a compile-time constant; slist_pool_bytes()
 *  is the matching runtime function.
 *
 *  Thread safety: none. Single-context use only.
 *
 *  Depends on: nothing
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef SLIST_H
#define SLIST_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "containers/containers_config.h"  /* GARBAGE_SLIST_DEFAULT_NODES */

#define SLIST_NIL  0xFFFFFFFFu

/* Per-node overhead: a single next index. */
#define SLIST_NODE_OVERHEAD  (sizeof(uint32_t))

/* Bytes of node pool for `cap` elements of `elem` bytes (compile-time
 * constant form). */
#define SLIST_POOL_BYTES(cap, elem) \
    ((size_t)(cap) * (SLIST_NODE_OVERHEAD + (size_t)(elem)))

/* Bytes for a pool of the configured default node count (override via
 * GARBAGE_SLIST_DEFAULT_NODES; see containers_config.h). */
#define SLIST_DEFAULT_POOL_BYTES(elem) \
    SLIST_POOL_BYTES(GARBAGE_SLIST_DEFAULT_NODES, (elem))

typedef struct {
    uint8_t *pool;          /* caller-owned node buffer            */
    size_t   capacity;      /* max nodes (elements)                */
    size_t   element_size;  /* payload bytes per node              */
    size_t   stride;        /* bytes per node (overhead + payload) */
    size_t   count;         /* live elements                       */
    uint32_t head;          /* first node index, or SLIST_NIL      */
    uint32_t tail;          /* last node index, or SLIST_NIL       */
    uint32_t free_head;     /* free-list head index, or SLIST_NIL  */
} SList;

/* Runtime sizing function — matches SLIST_POOL_BYTES exactly. */
size_t slist_pool_bytes(size_t capacity, size_t element_size);

/* Initialize over a caller-provided node pool. `pool` must be at least
 * slist_pool_bytes(capacity, element_size) bytes and must outlive the
 * list. Returns true on success, false on bad args / pool too small. */
bool slist_init(SList *l, void *pool, size_t pool_bytes,
                size_t element_size);

/* Reset to empty without touching the pool memory. */
void slist_clear(SList *l);

/* Insert. push_front and push_back are O(1). Copies element_size bytes.
 * Returns 1 on success, 0 if the pool is exhausted. */
int slist_push_front(SList *l, const void *elem);
int slist_push_back(SList *l, const void *elem);

/* pop_front is O(1). pop_back is O(n) (must find the new tail). Both
 * copy into *out if non-NULL. Return 1 on success, 0 if empty. */
int slist_pop_front(SList *l, void *out);
int slist_pop_back(SList *l, void *out);

/* Peek without removing. Returns 1 on success, 0 if empty. */
int slist_front(const SList *l, void *out);
int slist_back(const SList *l, void *out);

/* Count + predicates. */
size_t slist_count(const SList *l);
bool   slist_empty(const SList *l);
bool   slist_full(const SList *l);

/* ---- Iteration (forward only) ---------------------------------
 *      uint32_t it = slist_begin(&l);
 *      while (it != SLIST_NIL) {
 *          MyThing t; slist_get(&l, it, &t);
 *          it = slist_next(&l, it);
 *      }
 * A singly-linked list has no backward walk — use dlist if you need
 * one. */
uint32_t slist_begin(const SList *l);
uint32_t slist_next(const SList *l, uint32_t cursor);
int      slist_get(const SList *l, uint32_t cursor, void *out);

#endif /* SLIST_H */
