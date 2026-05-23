/* ============================================================
 *  dlist.h — doubly-linked list over a caller-provided node pool
 *
 *  Caller-provided storage, no malloc. You give dlist a pool of
 *  nodes (a plain byte buffer); the list hands them out on insert
 *  and reclaims them on remove, maintaining its own free list inside
 *  that buffer. When the pool is exhausted, inserts fail (return 0) —
 *  the same fixed-capacity discipline as the rest of this library.
 *
 *  Payloads: any type, identified by element_size at init. Inserts
 *  copy element_size bytes into the node via memcpy; reads copy them
 *  back out.
 *
 *  Sizing: the node pool must be exactly dlist_pool_bytes(capacity,
 *  element_size) bytes. Use that to size a static array or a bump
 *  allocation up front:
 *
 *      uint8_t pool[DLIST_POOL_BYTES(64, sizeof(MyThing))];
 *      DList list;
 *      dlist_init(&list, pool, sizeof pool, sizeof(MyThing));
 *
 *  The DLIST_POOL_BYTES() macro is a compile-time constant (for
 *  static arrays / _Static_assert); dlist_pool_bytes() is the runtime
 *  function (for bump arenas sized at startup). They agree.
 *
 *  Thread safety: none. Single-context use only.
 *
 *  Depends on: nothing
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef DLIST_H
#define DLIST_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

/* A node is two link indices + the inline payload. We use 32-bit
 * indices (not pointers) into the pool so the structure is the same
 * size on 32- and 64-bit hosts — which keeps DLIST_POOL_BYTES() a
 * portable compile-time constant. DLIST_NIL marks "no node". */
#define DLIST_NIL  0xFFFFFFFFu

/* Per-node overhead: next + prev indices (the payload is separate,
 * sized by element_size). Kept as a macro so the sizing math below
 * is a constant expression. */
#define DLIST_NODE_OVERHEAD  (2u * sizeof(uint32_t))

/* Bytes of node pool needed for `cap` elements of `elem` bytes each.
 * Compile-time constant form (for static arrays and _Static_assert). */
#define DLIST_POOL_BYTES(cap, elem) \
    ((size_t)(cap) * (DLIST_NODE_OVERHEAD + (size_t)(elem)))

typedef struct {
    uint8_t *pool;          /* caller-owned node buffer            */
    size_t   capacity;      /* max nodes (elements)                */
    size_t   element_size;  /* payload bytes per node              */
    size_t   stride;        /* bytes per node (overhead + payload) */
    size_t   count;         /* live elements                       */
    uint32_t head;          /* first node index, or DLIST_NIL      */
    uint32_t tail;          /* last node index, or DLIST_NIL       */
    uint32_t free_head;     /* free-list head index, or DLIST_NIL  */
} DList;

/* Runtime sizing function — matches DLIST_POOL_BYTES exactly. Use
 * this when sizing a bump allocation at startup. */
size_t dlist_pool_bytes(size_t capacity, size_t element_size);

/* Initialize a list over a caller-provided node pool. `pool` must be
 * at least dlist_pool_bytes(capacity, element_size) bytes and must
 * outlive the list. Returns true on success, false on bad args or a
 * pool too small for the requested capacity. */
bool dlist_init(DList *l, void *pool, size_t pool_bytes,
                size_t element_size);

/* Reset to empty without touching the pool memory. */
void dlist_clear(DList *l);

/* Insert at the front / back. Copies element_size bytes from `elem`.
 * Returns 1 on success, 0 if the pool is exhausted. */
int dlist_push_front(DList *l, const void *elem);
int dlist_push_back(DList *l, const void *elem);

/* Remove from the front / back into *out (if non-NULL). Returns 1 on
 * success, 0 if empty. */
int dlist_pop_front(DList *l, void *out);
int dlist_pop_back(DList *l, void *out);

/* Peek at the front / back without removing. Returns 1 on success,
 * 0 if empty. */
int dlist_front(const DList *l, void *out);
int dlist_back(const DList *l, void *out);

/* Count + predicates. */
size_t dlist_count(const DList *l);
bool   dlist_empty(const DList *l);
bool   dlist_full(const DList *l);

/* ---- Iteration ------------------------------------------------
 * Stable, index-based cursor so callers don't touch internals. Walk
 * forward:
 *      uint32_t it = dlist_begin(&l);
 *      while (it != DLIST_NIL) {
 *          MyThing t; dlist_get(&l, it, &t);
 *          it = dlist_next(&l, it);
 *      }
 * dlist_rbegin / dlist_prev walk backward. dlist_get copies the
 * payload at a cursor into *out (returns 1, or 0 if the cursor is
 * NIL). */
uint32_t dlist_begin(const DList *l);
uint32_t dlist_rbegin(const DList *l);
uint32_t dlist_next(const DList *l, uint32_t cursor);
uint32_t dlist_prev(const DList *l, uint32_t cursor);
int      dlist_get(const DList *l, uint32_t cursor, void *out);

#endif /* DLIST_H */
