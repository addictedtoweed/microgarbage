/* ============================================================
 *  tree.c — ordered binary tree (BST/AVL) over a node pool.
 *  Public domain (CC0). No warranty.
 *
 *  The node-pool free list, ordered descent, AVL rotations/retracing,
 *  and the in-order walk now live in the shared avl_core engine
 *  (avl_core.c), which the AVL-bucketed hash (avlhash.c) also uses.
 *  This file is the Tree facade over that engine: it owns the pool +
 *  the single root, picks BST vs AVL per its discipline, and keeps the
 *  public Tree API. Node layout (shared with avl_core):
 *    [uint32 left][uint32 right][uint32 parent][int8 balance][pad*3]
 *    [payload]
 * ============================================================ */

#include "containers/tree.h"
#include "containers/avl_core.h"
#include <string.h>

/* The Tree node shape must match the shared engine's. */
_Static_assert(TREE_NODE_OVERHEAD == AVL_CORE_NODE_OVERHEAD,
               "tree node overhead must match avl_core");
_Static_assert(TREE_NIL == AVL_CORE_NIL, "NIL sentinels must match");

/* ---- AvlCore views over a Tree -------------------------------- */

/* Read-only view (find/walk): free_head/count are untouched by those
 * paths, so leave them NULL. */
static AvlCore tree_core_ro(const Tree *t) {
    AvlCore c;
    c.pool         = t->pool;
    c.stride       = t->stride;
    c.element_size = t->element_size;
    c.capacity     = (uint32_t)t->capacity;
    c.cmp          = t->cmp;
    c.free_head    = NULL;
    c.count        = NULL;
    return c;
}

/* Mutating view (insert/remove): borrows the tree's free_head + count
 * so the engine updates them in place. */
static AvlCore tree_core_rw(Tree *t) {
    AvlCore c = tree_core_ro(t);
    c.free_head = &t->free_head;
    c.count     = &t->count;
    return c;
}

/* ---- sizing ---------------------------------------------------- */

size_t tree_pool_bytes(size_t capacity, size_t element_size) {
    return capacity * (TREE_NODE_OVERHEAD + element_size);
}

size_t tree_pool_bytes_for_depth(unsigned depth, size_t element_size) {
    if (depth == 0) return 0;
    if (depth >= 32) return 0;                 /* 2^32 nodes overflows */
    size_t nodes = ((size_t)1u << depth) - 1u; /* full tree node count */
    return tree_pool_bytes(nodes, element_size);
}

/* ---- init / clear --------------------------------------------- */

bool tree_init(Tree *t, TreeDiscipline discipline, tree_cmp_fn cmp,
               void *pool, size_t pool_bytes, size_t element_size) {
    if (!t || !pool || !cmp || element_size == 0) return false;
    if (discipline != TREE_BST && discipline != TREE_AVL) return false;

    size_t stride = TREE_NODE_OVERHEAD + element_size;
    size_t capacity = pool_bytes / stride;
    if (capacity == 0 || capacity >= TREE_NIL) return false;

    t->pool         = (uint8_t *)pool;
    t->capacity     = capacity;
    t->element_size = element_size;
    t->stride       = stride;
    t->discipline   = discipline;
    t->cmp          = cmp;
    tree_clear(t);
    return true;
}

void tree_clear(Tree *t) {
    if (!t) return;
    t->count = 0;
    t->root  = TREE_NIL;
    avl_core_build_freelist(t->pool, t->stride, (uint32_t)t->capacity,
                            &t->free_head);
}

/* ---- mutate ---------------------------------------------------- */

int tree_insert(Tree *t, const void *elem) {
    if (!t || !elem) return 0;
    AvlCore c = tree_core_rw(t);
    return avl_core_insert(&c, &t->root, elem, t->discipline == TREE_AVL);
}

int tree_remove(Tree *t, const void *key, void *out) {
    if (!t || !key) return 0;
    AvlCore c = tree_core_rw(t);
    return avl_core_remove(&c, &t->root, key, out, t->discipline == TREE_AVL);
}

/* ---- query ----------------------------------------------------- */

int tree_find(const Tree *t, const void *key, void *out) {
    if (!t || !key) return 0;
    AvlCore c = tree_core_ro(t);
    uint32_t n = avl_core_find(&c, t->root, key);
    if (n == TREE_NIL) return 0;
    if (out) memcpy(out, avl_core_payload(&c, n), t->element_size);
    return 1;
}

bool tree_contains(const Tree *t, const void *key) {
    if (!t || !key) return false;
    AvlCore c = tree_core_ro(t);
    return avl_core_find(&c, t->root, key) != TREE_NIL;
}

size_t tree_count(const Tree *t) { return t ? t->count : 0; }
bool   tree_empty(const Tree *t) { return !t || t->count == 0; }
bool   tree_full(const Tree *t)  { return t && t->count >= t->capacity; }

/* ---- in-order walk (shared across disciplines) ---------------- */

uint32_t tree_begin(const Tree *t) {
    if (!t) return TREE_NIL;
    AvlCore c = tree_core_ro(t);
    return avl_core_min(&c, t->root);
}
uint32_t tree_rbegin(const Tree *t) {
    if (!t) return TREE_NIL;
    AvlCore c = tree_core_ro(t);
    return avl_core_max(&c, t->root);
}
uint32_t tree_next(const Tree *t, uint32_t cursor) {
    if (!t || cursor >= t->capacity) return TREE_NIL;
    AvlCore c = tree_core_ro(t);
    return avl_core_next(&c, cursor);
}
uint32_t tree_prev(const Tree *t, uint32_t cursor) {
    if (!t || cursor >= t->capacity) return TREE_NIL;
    AvlCore c = tree_core_ro(t);
    return avl_core_prev(&c, cursor);
}
int tree_get(const Tree *t, uint32_t cursor, void *out) {
    if (!t || cursor == TREE_NIL || cursor >= t->capacity) return 0;
    AvlCore c = tree_core_ro(t);
    if (out) memcpy(out, avl_core_payload(&c, cursor), t->element_size);
    return 1;
}
