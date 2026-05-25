/* ============================================================
 *  avl_core.c — shared AVL/BST-over-a-pool engine. See avl_core.h.
 *
 *  This is the single home of the node-pool free list, the ordered
 *  descent, the AVL rotations + insert/remove retracing, and the
 *  in-order walk. Tree (tree.c) and AvlHash (avlhash.c) both drive it.
 *
 *  Ported from the original in tree.c, generalized so the subtree
 *  root is a caller-held `uint32_t *root` (a Tree's single root or an
 *  AvlHash bucket root) and the free_head/count live on the owner.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "containers/avl_core.h"
#include <string.h>

/* ---- node field access ---------------------------------------- */

static inline uint8_t *node_at(const AvlCore *c, uint32_t i) {
    return c->pool + (size_t)i * c->stride;
}
static inline uint32_t *n_left(const AvlCore *c, uint32_t i) {
    return (uint32_t *)(void *)(node_at(c, i));
}
static inline uint32_t *n_right(const AvlCore *c, uint32_t i) {
    return (uint32_t *)(void *)(node_at(c, i) + sizeof(uint32_t));
}
static inline uint32_t *n_parent(const AvlCore *c, uint32_t i) {
    return (uint32_t *)(void *)(node_at(c, i) + 2u * sizeof(uint32_t));
}
static inline int8_t *n_bal(const AvlCore *c, uint32_t i) {
    return (int8_t *)(node_at(c, i) + 3u * sizeof(uint32_t));
}

void *avl_core_payload(const AvlCore *c, uint32_t i) {
    return node_at(c, i) + AVL_CORE_NODE_OVERHEAD;
}

/* ---- free list ------------------------------------------------- */

void avl_core_build_freelist(uint8_t *pool, size_t stride,
                             uint32_t capacity, uint32_t *free_head) {
    *free_head = (capacity > 0) ? 0u : AVL_CORE_NIL;
    for (uint32_t i = 0; i < capacity; i++) {
        uint32_t *left = (uint32_t *)(void *)(pool + (size_t)i * stride);
        *left = (i + 1 < capacity) ? (i + 1) : AVL_CORE_NIL;
    }
}

static uint32_t alloc_node(const AvlCore *c) {
    uint32_t i = *c->free_head;
    if (i == AVL_CORE_NIL) return AVL_CORE_NIL;
    *c->free_head = *n_left(c, i);
    return i;
}
static void free_node(const AvlCore *c, uint32_t i) {
    *n_left(c, i) = *c->free_head;
    *c->free_head = i;
}

/* ---- shared search + walk ------------------------------------- */

uint32_t avl_core_find(const AvlCore *c, uint32_t root, const void *key) {
    uint32_t cur = root;
    while (cur != AVL_CORE_NIL) {
        int r = c->cmp(key, avl_core_payload(c, cur));
        if (r == 0) return cur;
        cur = (r < 0) ? *n_left(c, cur) : *n_right(c, cur);
    }
    return AVL_CORE_NIL;
}

static uint32_t subtree_min(const AvlCore *c, uint32_t n) {
    if (n == AVL_CORE_NIL) return AVL_CORE_NIL;
    while (*n_left(c, n) != AVL_CORE_NIL) n = *n_left(c, n);
    return n;
}
static uint32_t subtree_max(const AvlCore *c, uint32_t n) {
    if (n == AVL_CORE_NIL) return AVL_CORE_NIL;
    while (*n_right(c, n) != AVL_CORE_NIL) n = *n_right(c, n);
    return n;
}

uint32_t avl_core_min(const AvlCore *c, uint32_t root) {
    return subtree_min(c, root);
}
uint32_t avl_core_max(const AvlCore *c, uint32_t root) {
    return subtree_max(c, root);
}

uint32_t avl_core_next(const AvlCore *c, uint32_t cur) {
    if (cur == AVL_CORE_NIL || cur >= c->capacity) return AVL_CORE_NIL;
    if (*n_right(c, cur) != AVL_CORE_NIL) return subtree_min(c, *n_right(c, cur));
    uint32_t p = *n_parent(c, cur);
    while (p != AVL_CORE_NIL && cur == *n_right(c, p)) { cur = p; p = *n_parent(c, p); }
    return p;
}
uint32_t avl_core_prev(const AvlCore *c, uint32_t cur) {
    if (cur == AVL_CORE_NIL || cur >= c->capacity) return AVL_CORE_NIL;
    if (*n_left(c, cur) != AVL_CORE_NIL) return subtree_max(c, *n_left(c, cur));
    uint32_t p = *n_parent(c, cur);
    while (p != AVL_CORE_NIL && cur == *n_left(c, p)) { cur = p; p = *n_parent(c, p); }
    return p;
}

/* ================================================================
 *  AVL rotations + rebalancing
 *
 *  balance(n) = height(right) - height(left). `root` is threaded so a
 *  rotation that lifts a new node to the top updates the owner's root.
 * ================================================================ */

static void replace_child(const AvlCore *c, uint32_t *root,
                          uint32_t parent, uint32_t old, uint32_t neu) {
    if (parent == AVL_CORE_NIL) {
        *root = neu;
    } else if (*n_left(c, parent) == old) {
        *n_left(c, parent) = neu;
    } else {
        *n_right(c, parent) = neu;
    }
    if (neu != AVL_CORE_NIL) *n_parent(c, neu) = parent;
}

static uint32_t rotate_left(const AvlCore *c, uint32_t *root, uint32_t x) {
    uint32_t y = *n_right(c, x);
    uint32_t p = *n_parent(c, x);
    uint32_t b = *n_left(c, y);

    *n_right(c, x) = b;
    if (b != AVL_CORE_NIL) *n_parent(c, b) = x;
    *n_left(c, y) = x;
    *n_parent(c, x) = y;
    replace_child(c, root, p, x, y);

    if (*n_bal(c, y) == 0) {
        *n_bal(c, x) = (int8_t)(+1);
        *n_bal(c, y) = (int8_t)(-1);
    } else {
        *n_bal(c, x) = 0;
        *n_bal(c, y) = 0;
    }
    return y;
}

static uint32_t rotate_right(const AvlCore *c, uint32_t *root, uint32_t x) {
    uint32_t y = *n_left(c, x);
    uint32_t p = *n_parent(c, x);
    uint32_t b = *n_right(c, y);

    *n_left(c, x) = b;
    if (b != AVL_CORE_NIL) *n_parent(c, b) = x;
    *n_right(c, y) = x;
    *n_parent(c, x) = y;
    replace_child(c, root, p, x, y);

    if (*n_bal(c, y) == 0) {
        *n_bal(c, x) = (int8_t)(-1);
        *n_bal(c, y) = (int8_t)(+1);
    } else {
        *n_bal(c, x) = 0;
        *n_bal(c, y) = 0;
    }
    return y;
}

static uint32_t rotate_left_right(const AvlCore *c, uint32_t *root, uint32_t x) {
    uint32_t z = *n_left(c, x);
    uint32_t y = *n_right(c, z);
    int8_t yb = *n_bal(c, y);

    (void)rotate_left(c, root, z);
    uint32_t newroot = rotate_right(c, root, x);

    if (yb > 0) {
        *n_bal(c, z) = (int8_t)(-1);
        *n_bal(c, x) = 0;
    } else if (yb == 0) {
        *n_bal(c, z) = 0;
        *n_bal(c, x) = 0;
    } else {
        *n_bal(c, z) = 0;
        *n_bal(c, x) = (int8_t)(+1);
    }
    *n_bal(c, y) = 0;
    return newroot;
}

static uint32_t rotate_right_left(const AvlCore *c, uint32_t *root, uint32_t x) {
    uint32_t z = *n_right(c, x);
    uint32_t y = *n_left(c, z);
    int8_t yb = *n_bal(c, y);

    (void)rotate_right(c, root, z);
    uint32_t newroot = rotate_left(c, root, x);

    if (yb < 0) {
        *n_bal(c, z) = (int8_t)(+1);
        *n_bal(c, x) = 0;
    } else if (yb == 0) {
        *n_bal(c, z) = 0;
        *n_bal(c, x) = 0;
    } else {
        *n_bal(c, z) = 0;
        *n_bal(c, x) = (int8_t)(-1);
    }
    *n_bal(c, y) = 0;
    return newroot;
}

static void avl_retrace_insert(const AvlCore *c, uint32_t *root, uint32_t n) {
    uint32_t child = n;
    uint32_t p = *n_parent(c, child);
    while (p != AVL_CORE_NIL) {
        if (child == *n_right(c, p)) {
            if (*n_bal(c, p) > 0) {
                if (*n_bal(c, child) < 0) rotate_right_left(c, root, p);
                else                      rotate_left(c, root, p);
                break;
            } else if (*n_bal(c, p) < 0) {
                *n_bal(c, p) = 0;
                break;
            } else {
                *n_bal(c, p) = (int8_t)(+1);
            }
        } else {
            if (*n_bal(c, p) < 0) {
                if (*n_bal(c, child) > 0) rotate_left_right(c, root, p);
                else                      rotate_right(c, root, p);
                break;
            } else if (*n_bal(c, p) > 0) {
                *n_bal(c, p) = 0;
                break;
            } else {
                *n_bal(c, p) = (int8_t)(-1);
            }
        }
        child = p;
        p = *n_parent(c, child);
    }
}

static void avl_retrace_remove(const AvlCore *c, uint32_t *root,
                               uint32_t p, int lost_left) {
    while (p != AVL_CORE_NIL) {
        uint32_t next_parent = *n_parent(c, p);
        int came_from_left = lost_left;
        if (next_parent != AVL_CORE_NIL)
            lost_left = (*n_left(c, next_parent) == p);

        if (came_from_left) {
            if (*n_bal(c, p) > 0) {
                uint32_t sib = *n_right(c, p);
                int8_t sb = *n_bal(c, sib);
                uint32_t newroot;
                if (sb < 0) newroot = rotate_right_left(c, root, p);
                else        newroot = rotate_left(c, root, p);
                p = newroot;
                if (sb == 0) break;
            } else if (*n_bal(c, p) == 0) {
                *n_bal(c, p) = (int8_t)(+1);
                break;
            } else {
                *n_bal(c, p) = 0;
            }
        } else {
            if (*n_bal(c, p) < 0) {
                uint32_t sib = *n_left(c, p);
                int8_t sb = *n_bal(c, sib);
                uint32_t newroot;
                if (sb > 0) newroot = rotate_left_right(c, root, p);
                else        newroot = rotate_right(c, root, p);
                p = newroot;
                if (sb == 0) break;
            } else if (*n_bal(c, p) == 0) {
                *n_bal(c, p) = (int8_t)(-1);
                break;
            } else {
                *n_bal(c, p) = 0;
            }
        }
        p = next_parent;
    }
}

/* ---- insert / remove ------------------------------------------ */

int avl_core_insert(const AvlCore *c, uint32_t *root,
                    const void *elem, int balance) {
    uint32_t parent = AVL_CORE_NIL;
    uint32_t cur = *root;
    int cmp = 0;
    while (cur != AVL_CORE_NIL) {
        cmp = c->cmp(elem, avl_core_payload(c, cur));
        if (cmp == 0) return 0;            /* duplicate: set semantics */
        parent = cur;
        cur = (cmp < 0) ? *n_left(c, cur) : *n_right(c, cur);
    }

    uint32_t n = alloc_node(c);
    if (n == AVL_CORE_NIL) return 0;       /* pool exhausted */

    memcpy(avl_core_payload(c, n), elem, c->element_size);
    *n_left(c, n)   = AVL_CORE_NIL;
    *n_right(c, n)  = AVL_CORE_NIL;
    *n_parent(c, n) = parent;
    *n_bal(c, n)    = 0;

    if (parent == AVL_CORE_NIL) {
        *root = n;
    } else if (cmp < 0) {
        *n_left(c, parent) = n;
    } else {
        *n_right(c, parent) = n;
    }
    (*c->count)++;

    if (balance) avl_retrace_insert(c, root, n);
    return 1;
}

int avl_core_remove(const AvlCore *c, uint32_t *root,
                    const void *key, void *out, int balance) {
    uint32_t z = avl_core_find(c, *root, key);
    if (z == AVL_CORE_NIL) return 0;
    if (out) memcpy(out, avl_core_payload(c, z), c->element_size);

    /* Two children: swap payload with in-order successor (which has at
     * most one child), then physically delete the successor. */
    uint32_t target = z;
    if (*n_left(c, z) != AVL_CORE_NIL && *n_right(c, z) != AVL_CORE_NIL) {
        uint32_t s = subtree_min(c, *n_right(c, z));
        memcpy(avl_core_payload(c, z), avl_core_payload(c, s), c->element_size);
        target = s;
    }

    uint32_t child = (*n_left(c, target) != AVL_CORE_NIL)
                     ? *n_left(c, target) : *n_right(c, target);
    uint32_t parent = *n_parent(c, target);
    int lost_left = (parent != AVL_CORE_NIL) && (*n_left(c, parent) == target);

    replace_child(c, root, parent, target, child);

    if (balance && parent != AVL_CORE_NIL) {
        avl_retrace_remove(c, root, parent, lost_left);
    }

    free_node(c, target);
    (*c->count)--;
    return 1;
}
