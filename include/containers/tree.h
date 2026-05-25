/* ============================================================
 *  tree.h — ordered binary tree over a caller-provided node pool,
 *           with a selectable insertion discipline and one shared
 *           in-order walk across all disciplines.
 *
 *  ONE tree type. You choose the balancing discipline at init:
 *    TREE_BST  — plain unbalanced binary search tree. Smallest work
 *                per insert; O(log n) average but O(n) worst case on
 *                sorted/sequential input. Fine when input is random
 *                or you accept the tradeoff.
 *    TREE_AVL  — height-balanced. O(log n) guaranteed regardless of
 *                insertion order (rotations keep it balanced). No
 *                sorted-input footgun. Costs a little rebalancing work
 *                per insert/remove and one balance byte per node.
 *
 *  A single tree instance uses ONE discipline for its whole life —
 *  you do NOT mix (BST-inserting then AVL-inserting the same tree is
 *  undefined; the AVL step assumes an already-balanced tree). The
 *  discipline is fixed at init.
 *
 *  THE SAME WALK works for every discipline: in-order traversal
 *  (tree_begin/tree_next) yields elements in sorted order whether the
 *  tree is a BST or AVL, because the search-ordering invariant is the
 *  same — only the shape differs. tree_find is likewise shared.
 *
 *  Caller-provided storage, no malloc. You give a node pool; nodes are
 *  handed out / reclaimed from an internal free list; inserts fail
 *  (return 0) when the pool is exhausted.
 *
 *  Ordering: you provide a comparator at init. It compares two
 *  payloads and returns <0, 0, >0 (like strcmp/qsort). Duplicate keys
 *  (compare == 0) are rejected by insert (returns 0) — this is a set,
 *  not a multiset.
 *
 *  Sizing:
 *      uint8_t pool[TREE_POOL_BYTES(64, sizeof(MyKey))];
 *      Tree t;
 *      tree_init(&t, TREE_AVL, my_cmp, pool, sizeof pool, sizeof(MyKey));
 *  TREE_POOL_BYTES() is a compile-time constant; tree_pool_bytes() is
 *  the matching runtime function. There is also tree_pool_bytes_for_
 *  depth() which returns the WORST-CASE bytes to hold any tree up to a
 *  given depth (= a full tree of that depth) — a bound, not exact.
 *
 *  Thread safety: none. Single-context use only.
 *
 *  Depends on: nothing
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef TREE_H
#define TREE_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "containers/containers_config.h"  /* GARBAGE_TREE_DEFAULT_NODES */

#define TREE_NIL  0xFFFFFFFFu

/* Per-node overhead: left + right + parent indices, plus an int8
 * balance factor (AVL) and 3 bytes padding to keep payload aligned.
 * Kept as a macro so sizing is a constant expression. */
#define TREE_NODE_OVERHEAD  (3u * sizeof(uint32_t) + 4u)

/* Bytes of node pool for `cap` nodes of `elem` bytes (compile-time). */
#define TREE_POOL_BYTES(cap, elem) \
    ((size_t)(cap) * (TREE_NODE_OVERHEAD + (size_t)(elem)))

/* Bytes for a pool of the configured default node count (override via
 * GARBAGE_TREE_DEFAULT_NODES; see containers_config.h). */
#define TREE_DEFAULT_POOL_BYTES(elem) \
    TREE_POOL_BYTES(GARBAGE_TREE_DEFAULT_NODES, (elem))

/* Insertion / balancing discipline, chosen at init. */
typedef enum {
    TREE_BST = 0,   /* plain unbalanced */
    TREE_AVL = 1    /* height-balanced  */
} TreeDiscipline;

/* Comparator: <0 if a<b, 0 if equal, >0 if a>b. */
typedef int (*tree_cmp_fn)(const void *a, const void *b);

typedef struct {
    uint8_t       *pool;
    size_t         capacity;
    size_t         element_size;
    size_t         stride;
    size_t         count;
    uint32_t       root;
    uint32_t       free_head;
    TreeDiscipline discipline;
    tree_cmp_fn    cmp;
} Tree;

/* Runtime sizing — matches TREE_POOL_BYTES exactly. */
size_t tree_pool_bytes(size_t capacity, size_t element_size);

/* Worst-case bytes to hold ANY tree up to `depth` levels (i.e. a full
 * tree of that depth, 2^depth - 1 nodes). A BOUND, not exact — a tree
 * of depth D may hold as few as D nodes. Use when you size by "how
 * deep could it get" rather than "how many nodes". Returns 0 if depth
 * is 0 or the node count would overflow. */
size_t tree_pool_bytes_for_depth(unsigned depth, size_t element_size);

/* Initialize. discipline + comparator are fixed for the tree's life.
 * `pool` must be >= tree_pool_bytes(capacity, element_size) bytes and
 * outlive the tree. Returns true on success. */
bool tree_init(Tree *t, TreeDiscipline discipline, tree_cmp_fn cmp,
               void *pool, size_t pool_bytes, size_t element_size);

/* Reset to empty without touching the pool. */
void tree_clear(Tree *t);

/* Insert a copy of *elem, ordered by the comparator, using the tree's
 * discipline. Returns 1 on success, 0 if the pool is exhausted OR a
 * matching key already exists (set semantics — no duplicates). */
int tree_insert(Tree *t, const void *elem);

/* Remove the element matching *key (by the comparator). Copies the
 * removed payload into *out if non-NULL. Returns 1 if found+removed,
 * 0 if not present. Rebalances if the discipline requires it. */
int tree_remove(Tree *t, const void *key, void *out);

/* Find the element matching *key. Copies it into *out if non-NULL.
 * Returns 1 if found, 0 if not. (Shared across disciplines.) */
int tree_find(const Tree *t, const void *key, void *out);

/* True if a matching key is present. */
bool tree_contains(const Tree *t, const void *key);

/* Count + predicates. */
size_t tree_count(const Tree *t);
bool   tree_empty(const Tree *t);
bool   tree_full(const Tree *t);

/* ---- In-order walk (SHARED across all disciplines) ------------
 * Yields elements in sorted order:
 *      for (uint32_t it = tree_begin(&t); it != TREE_NIL;
 *           it = tree_next(&t, it)) {
 *          MyKey k; tree_get(&t, it, &k);
 *      }
 * tree_rbegin/tree_prev walk in reverse (descending) order. */
uint32_t tree_begin(const Tree *t);   /* smallest */
uint32_t tree_rbegin(const Tree *t);  /* largest  */
uint32_t tree_next(const Tree *t, uint32_t cursor);
uint32_t tree_prev(const Tree *t, uint32_t cursor);
int      tree_get(const Tree *t, uint32_t cursor, void *out);

#endif /* TREE_H */
