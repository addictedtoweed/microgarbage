/* ============================================================
 *  avl_core.h — shared "AVL/BST over a node pool" engine.
 *
 *  INTERNAL building block, not a standalone container. It holds the
 *  one copy of the node-pool free-list management, the binary-search
 *  descent, the AVL rotations / insert+remove retracing, and the
 *  in-order walk — so both `tree.c` (the ordered Tree, BST or AVL
 *  discipline) and `avlhash.c` (each hash bucket is an AVL tree) reuse
 *  the same code instead of duplicating it.
 *
 *  The engine is rooted by a caller-held `uint32_t *root`, NOT by a
 *  fixed field — that's the whole point: a Tree passes the address of
 *  its single root; an AvlHash passes the address of bucket[h]. Many
 *  roots can therefore share ONE node pool + ONE free list (the hash
 *  case), since every node is the same 16-byte-overhead shape.
 *
 *  Node layout in the pool (index-addressed, AVL_CORE_NIL = none):
 *    [uint32 left][uint32 right][uint32 parent][int8 balance][pad*3]
 *    [payload]
 *  `balance` is the AVL height-balance factor; the BST path ignores it.
 *  The free list threads through the `left` field of free nodes.
 *
 *  An `AvlCore` is a lightweight VIEW over the owner's storage — it
 *  borrows the owner's free_head and count by pointer so updates land
 *  back on the owner. Construct one on the stack per call; it owns
 *  nothing.
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef AVL_CORE_H
#define AVL_CORE_H

#include <stddef.h>
#include <stdint.h>

#define AVL_CORE_NIL  0xFFFFFFFFu

/* left + right + parent indices, int8 balance, 3 bytes pad. Kept a
 * macro so pool sizing stays a constant expression. */
#define AVL_CORE_NODE_OVERHEAD  (3u * sizeof(uint32_t) + 4u)

/* Comparator: <0 if a<b, 0 if equal, >0 if a>b. */
typedef int (*avl_cmp_fn)(const void *a, const void *b);

/* A borrowed view over the owner's node pool. Build per call. */
typedef struct {
    uint8_t   *pool;          /* node buffer (owner-owned)            */
    size_t     stride;        /* AVL_CORE_NODE_OVERHEAD + element     */
    size_t     element_size;  /* payload bytes                        */
    uint32_t   capacity;      /* max nodes                            */
    avl_cmp_fn cmp;           /* payload ordering                     */
    uint32_t  *free_head;     /* -> owner's free-list head            */
    size_t    *count;         /* -> owner's live-node count           */
} AvlCore;

/* Thread the free list through nodes [0,capacity) and set *free_head
 * to 0 (or AVL_CORE_NIL if capacity == 0). Call once at init/clear. */
void avl_core_build_freelist(uint8_t *pool, size_t stride,
                             uint32_t capacity, uint32_t *free_head);

/* Payload pointer for node i (no bounds check). */
void *avl_core_payload(const AvlCore *c, uint32_t i);

/* Insert a copy of *elem under *root, ordered by cmp. `balance`
 * selects AVL (1) vs plain BST (0). Returns 1 on success, 0 on a
 * duplicate key (set semantics) or an exhausted pool. Updates *root,
 * *free_head, *count. */
int avl_core_insert(const AvlCore *c, uint32_t *root,
                    const void *elem, int balance);

/* Find the node matching *key under root. Returns its index or
 * AVL_CORE_NIL. (Discipline-independent.) */
uint32_t avl_core_find(const AvlCore *c, uint32_t root, const void *key);

/* Remove the node matching *key from *root; copies its payload into
 * *out if non-NULL. `balance` selects AVL vs BST retracing. Returns 1
 * if found+removed, 0 if absent. Updates *root, *free_head, *count. */
int avl_core_remove(const AvlCore *c, uint32_t *root,
                    const void *key, void *out, int balance);

/* In-order walk. min/max are relative to the given root; next/prev
 * climb via parent pointers and so need only a cursor. */
uint32_t avl_core_min(const AvlCore *c, uint32_t root);
uint32_t avl_core_max(const AvlCore *c, uint32_t root);
uint32_t avl_core_next(const AvlCore *c, uint32_t cursor);
uint32_t avl_core_prev(const AvlCore *c, uint32_t cursor);

#endif /* AVL_CORE_H */
