/* ============================================================
 *  tree.c — ordered binary tree (BST/AVL) over a node pool.
 *  Public domain (CC0). No warranty.
 *
 *  Node layout in the pool:
 *    [uint32 left][uint32 right][uint32 parent][int8 balance][pad*3]
 *    [payload]
 *  Index-addressed (TREE_NIL = none). The balance field is the AVL
 *  height-balance factor (height(right) - height(left)); BST ignores
 *  it. parent is maintained by all disciplines (the shared walk uses
 *  it for in-order successor/predecessor).
 * ============================================================ */

#include "containers/tree.h"
#include <string.h>

/* ---- node field access ---------------------------------------- */

static inline uint8_t *node_at(const Tree *t, uint32_t i) {
    return t->pool + (size_t)i * t->stride;
}
static inline uint32_t *n_left(const Tree *t, uint32_t i) {
    return (uint32_t *)(void *)(node_at(t, i));
}
static inline uint32_t *n_right(const Tree *t, uint32_t i) {
    return (uint32_t *)(void *)(node_at(t, i) + sizeof(uint32_t));
}
static inline uint32_t *n_parent(const Tree *t, uint32_t i) {
    return (uint32_t *)(void *)(node_at(t, i) + 2u * sizeof(uint32_t));
}
static inline int8_t *n_bal(const Tree *t, uint32_t i) {
    return (int8_t *)(node_at(t, i) + 3u * sizeof(uint32_t));
}
static inline void *n_payload(const Tree *t, uint32_t i) {
    return node_at(t, i) + TREE_NODE_OVERHEAD;
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

/* ---- init / clear / free list --------------------------------- */

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
    t->count     = 0;
    t->root      = TREE_NIL;
    t->free_head = (t->capacity > 0) ? 0u : TREE_NIL;
    for (uint32_t i = 0; i < t->capacity; i++) {
        *n_left(t, i) = (i + 1 < t->capacity) ? (i + 1) : TREE_NIL;
    }
}

static uint32_t alloc_node(Tree *t) {
    uint32_t i = t->free_head;
    if (i == TREE_NIL) return TREE_NIL;
    t->free_head = *n_left(t, i);
    return i;
}
static void free_node(Tree *t, uint32_t i) {
    *n_left(t, i) = t->free_head;
    t->free_head = i;
}

/* ---- shared search -------------------------------------------- */

/* Returns the node matching key, or TREE_NIL. */
static uint32_t find_node(const Tree *t, const void *key) {
    uint32_t cur = t->root;
    while (cur != TREE_NIL) {
        int c = t->cmp(key, n_payload(t, cur));
        if (c == 0) return cur;
        cur = (c < 0) ? *n_left(t, cur) : *n_right(t, cur);
    }
    return TREE_NIL;
}

int tree_find(const Tree *t, const void *key, void *out) {
    if (!t || !key) return 0;
    uint32_t n = find_node(t, key);
    if (n == TREE_NIL) return 0;
    if (out) memcpy(out, n_payload(t, n), t->element_size);
    return 1;
}

bool tree_contains(const Tree *t, const void *key) {
    return t && key && find_node(t, key) != TREE_NIL;
}

/* ---- shared in-order walk ------------------------------------- */

static uint32_t subtree_min(const Tree *t, uint32_t n) {
    if (n == TREE_NIL) return TREE_NIL;
    while (*n_left(t, n) != TREE_NIL) n = *n_left(t, n);
    return n;
}
static uint32_t subtree_max(const Tree *t, uint32_t n) {
    if (n == TREE_NIL) return TREE_NIL;
    while (*n_right(t, n) != TREE_NIL) n = *n_right(t, n);
    return n;
}

uint32_t tree_begin(const Tree *t) {
    return t ? subtree_min(t, t->root) : TREE_NIL;
}
uint32_t tree_rbegin(const Tree *t) {
    return t ? subtree_max(t, t->root) : TREE_NIL;
}

uint32_t tree_next(const Tree *t, uint32_t c) {
    if (!t || c == TREE_NIL || c >= t->capacity) return TREE_NIL;
    /* successor: min of right subtree, else climb until we come up
     * from a left child. */
    if (*n_right(t, c) != TREE_NIL) return subtree_min(t, *n_right(t, c));
    uint32_t p = *n_parent(t, c);
    while (p != TREE_NIL && c == *n_right(t, p)) { c = p; p = *n_parent(t, p); }
    return p;
}
uint32_t tree_prev(const Tree *t, uint32_t c) {
    if (!t || c == TREE_NIL || c >= t->capacity) return TREE_NIL;
    if (*n_left(t, c) != TREE_NIL) return subtree_max(t, *n_left(t, c));
    uint32_t p = *n_parent(t, c);
    while (p != TREE_NIL && c == *n_left(t, p)) { c = p; p = *n_parent(t, p); }
    return p;
}
int tree_get(const Tree *t, uint32_t c, void *out) {
    if (!t || c == TREE_NIL || c >= t->capacity) return 0;
    if (out) memcpy(out, n_payload(t, c), t->element_size);
    return 1;
}

/* ---- counts --------------------------------------------------- */

size_t tree_count(const Tree *t) { return t ? t->count : 0; }
bool   tree_empty(const Tree *t) { return !t || t->count == 0; }
bool   tree_full(const Tree *t)  { return t && t->count >= t->capacity; }

/* ================================================================
 *  AVL rotations + rebalancing
 *
 *  balance(n) = height(right) - height(left), kept in [-2,+2] during
 *  fixups and restored to [-1,+1]. We use the iterative,
 *  parent-pointer formulation with retracing.
 * ================================================================ */

/* Replace child link in `parent` (or root) that points to `old` with
 * `neu`, and set neu's parent. */
static void replace_child(Tree *t, uint32_t parent, uint32_t old, uint32_t neu) {
    if (parent == TREE_NIL) {
        t->root = neu;
    } else if (*n_left(t, parent) == old) {
        *n_left(t, parent) = neu;
    } else {
        *n_right(t, parent) = neu;
    }
    if (neu != TREE_NIL) *n_parent(t, neu) = parent;
}

/* Left rotation around x (x's right child y becomes subtree root).
 * Returns the new subtree root. Updates balances per AVL rules. */
static uint32_t rotate_left(Tree *t, uint32_t x) {
    uint32_t y = *n_right(t, x);
    uint32_t p = *n_parent(t, x);
    uint32_t b = *n_left(t, y);

    *n_right(t, x) = b;
    if (b != TREE_NIL) *n_parent(t, b) = x;
    *n_left(t, y) = x;
    *n_parent(t, x) = y;
    replace_child(t, p, x, y);

    /* balance update (standard AVL after left rotation) */
    if (*n_bal(t, y) == 0) {
        *n_bal(t, x) = (int8_t)(+1);
        *n_bal(t, y) = (int8_t)(-1);
    } else {
        *n_bal(t, x) = 0;
        *n_bal(t, y) = 0;
    }
    return y;
}

static uint32_t rotate_right(Tree *t, uint32_t x) {
    uint32_t y = *n_left(t, x);
    uint32_t p = *n_parent(t, x);
    uint32_t b = *n_right(t, y);

    *n_left(t, x) = b;
    if (b != TREE_NIL) *n_parent(t, b) = x;
    *n_right(t, y) = x;
    *n_parent(t, x) = y;
    replace_child(t, p, x, y);

    if (*n_bal(t, y) == 0) {
        *n_bal(t, x) = (int8_t)(-1);
        *n_bal(t, y) = (int8_t)(+1);
    } else {
        *n_bal(t, x) = 0;
        *n_bal(t, y) = 0;
    }
    return y;
}

/* Left-Right and Right-Left double rotations, with the precise
 * balance-factor fixups that depend on the inner node's balance. */
static uint32_t rotate_left_right(Tree *t, uint32_t x) {
    uint32_t z = *n_left(t, x);
    uint32_t y = *n_right(t, z);
    int8_t yb = *n_bal(t, y);

    (void)rotate_left(t, z);   /* sets some balances; we override below */
    uint32_t newroot = rotate_right(t, x);

    /* After an LR rotation, balances depend on y's original balance. */
    if (yb > 0) {
        *n_bal(t, z) = (int8_t)(-1);
        *n_bal(t, x) = 0;
    } else if (yb == 0) {
        *n_bal(t, z) = 0;
        *n_bal(t, x) = 0;
    } else {
        *n_bal(t, z) = 0;
        *n_bal(t, x) = (int8_t)(+1);
    }
    *n_bal(t, y) = 0;
    return newroot;
}

static uint32_t rotate_right_left(Tree *t, uint32_t x) {
    uint32_t z = *n_right(t, x);
    uint32_t y = *n_left(t, z);
    int8_t yb = *n_bal(t, y);

    (void)rotate_right(t, z);
    uint32_t newroot = rotate_left(t, x);

    if (yb < 0) {
        *n_bal(t, z) = (int8_t)(+1);
        *n_bal(t, x) = 0;
    } else if (yb == 0) {
        *n_bal(t, z) = 0;
        *n_bal(t, x) = 0;
    } else {
        *n_bal(t, z) = 0;
        *n_bal(t, x) = (int8_t)(-1);
    }
    *n_bal(t, y) = 0;
    return newroot;
}

/* Retrace after inserting node `n` (child just attached). Walks up
 * adjusting balances, rotating where needed. */
static void avl_retrace_insert(Tree *t, uint32_t n) {
    uint32_t child = n;
    uint32_t p = *n_parent(t, child);
    while (p != TREE_NIL) {
        if (child == *n_right(t, p)) {
            /* right-heavier */
            if (*n_bal(t, p) > 0) {
                if (*n_bal(t, child) < 0) rotate_right_left(t, p);
                else                      rotate_left(t, p);
                break;  /* height restored */
            } else if (*n_bal(t, p) < 0) {
                *n_bal(t, p) = 0;
                break;
            } else {
                *n_bal(t, p) = (int8_t)(+1);
            }
        } else {
            /* left-heavier */
            if (*n_bal(t, p) < 0) {
                if (*n_bal(t, child) > 0) rotate_left_right(t, p);
                else                      rotate_right(t, p);
                break;
            } else if (*n_bal(t, p) > 0) {
                *n_bal(t, p) = 0;
                break;
            } else {
                *n_bal(t, p) = (int8_t)(-1);
            }
        }
        child = p;
        p = *n_parent(t, child);
    }
}

/* ---- insert (shared descent; discipline only changes fixup) ---- */

int tree_insert(Tree *t, const void *elem) {
    if (!t || !elem) return 0;

    /* find insertion point */
    uint32_t parent = TREE_NIL;
    uint32_t cur = t->root;
    int c = 0;
    while (cur != TREE_NIL) {
        c = t->cmp(elem, n_payload(t, cur));
        if (c == 0) return 0;            /* duplicate: set semantics */
        parent = cur;
        cur = (c < 0) ? *n_left(t, cur) : *n_right(t, cur);
    }

    uint32_t n = alloc_node(t);
    if (n == TREE_NIL) return 0;         /* pool exhausted */

    memcpy(n_payload(t, n), elem, t->element_size);
    *n_left(t, n)   = TREE_NIL;
    *n_right(t, n)  = TREE_NIL;
    *n_parent(t, n) = parent;
    *n_bal(t, n)    = 0;

    if (parent == TREE_NIL) {
        t->root = n;
    } else if (c < 0) {
        *n_left(t, parent) = n;
    } else {
        *n_right(t, parent) = n;
    }
    t->count++;

    if (t->discipline == TREE_AVL) {
        avl_retrace_insert(t, n);
    }
    return 1;
}

/* ---- remove --------------------------------------------------- */

/* For AVL remove we retrace from a starting parent on the side a node
 * was lost. Returns nothing; adjusts up to the root. */
static void avl_retrace_remove(Tree *t, uint32_t p, int lost_left) {
    while (p != TREE_NIL) {
        uint32_t next_parent = *n_parent(t, p);
        int came_from_left = lost_left;
        if (next_parent != TREE_NIL)
            lost_left = (*n_left(t, next_parent) == p);

        if (came_from_left) {
            /* left subtree shrank -> tree becomes right-heavier */
            if (*n_bal(t, p) > 0) {
                uint32_t sib = *n_right(t, p);
                int8_t sb = *n_bal(t, sib);
                uint32_t newroot;
                if (sb < 0) newroot = rotate_right_left(t, p);
                else        newroot = rotate_left(t, p);
                p = newroot;
                if (sb == 0) break;     /* height unchanged above */
            } else if (*n_bal(t, p) == 0) {
                *n_bal(t, p) = (int8_t)(+1);
                break;                  /* height unchanged */
            } else {
                *n_bal(t, p) = 0;       /* was left-heavy, now balanced */
            }
        } else {
            if (*n_bal(t, p) < 0) {
                uint32_t sib = *n_left(t, p);
                int8_t sb = *n_bal(t, sib);
                uint32_t newroot;
                if (sb > 0) newroot = rotate_left_right(t, p);
                else        newroot = rotate_right(t, p);
                p = newroot;
                if (sb == 0) break;
            } else if (*n_bal(t, p) == 0) {
                *n_bal(t, p) = (int8_t)(-1);
                break;
            } else {
                *n_bal(t, p) = 0;
            }
        }
        p = next_parent;
    }
}

int tree_remove(Tree *t, const void *key, void *out) {
    if (!t || !key) return 0;
    uint32_t z = find_node(t, key);
    if (z == TREE_NIL) return 0;
    if (out) memcpy(out, n_payload(t, z), t->element_size);

    /* If z has two children, swap with in-order successor (min of
     * right subtree), which has at most one child; then delete that. */
    uint32_t target = z;
    if (*n_left(t, z) != TREE_NIL && *n_right(t, z) != TREE_NIL) {
        uint32_t s = subtree_min(t, *n_right(t, z));
        /* move successor payload into z, then physically remove s */
        memcpy(n_payload(t, z), n_payload(t, s), t->element_size);
        target = s;
    }

    /* target has at most one child. */
    uint32_t child = (*n_left(t, target) != TREE_NIL)
                     ? *n_left(t, target) : *n_right(t, target);
    uint32_t parent = *n_parent(t, target);
    int lost_left = (parent != TREE_NIL) && (*n_left(t, parent) == target);

    replace_child(t, parent, target, child);

    if (t->discipline == TREE_AVL && parent != TREE_NIL) {
        avl_retrace_remove(t, parent, lost_left);
    }

    free_node(t, target);
    t->count--;
    return 1;
}
