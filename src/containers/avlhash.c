/* ============================================================
 *  avlhash.c — hash table with AVL-tree buckets over one node pool.
 *  Public domain (CC0). No warranty.
 *
 *  Buckets are an array of AVL roots; all of them draw entry nodes
 *  from ONE shared pool + free list via the avl_core engine. Bucket
 *  selection is hash(key) % n_buckets; within a bucket, the comparator
 *  orders the AVL tree, so lookups/inserts/removes in a bucket are
 *  O(log k) for k entries in that bucket.
 * ============================================================ */

#include "containers/avlhash.h"
#include "containers/avl_core.h"
#include <string.h>

/* Node shape must match the shared engine. */
_Static_assert(AVLHASH_NODE_OVERHEAD == AVL_CORE_NODE_OVERHEAD,
               "avlhash node overhead must match avl_core");
_Static_assert(AVLHASH_NIL == AVL_CORE_NIL, "NIL sentinels must match");

/* ---- AvlCore views over an AvlHash ---------------------------- */

static AvlCore ah_core_ro(const AvlHash *h) {
    AvlCore c;
    c.pool         = h->pool;
    c.stride       = h->stride;
    c.element_size = h->element_size;
    c.capacity     = (uint32_t)h->capacity;
    c.cmp          = h->cmp;
    c.free_head    = NULL;
    c.count        = NULL;
    return c;
}
static AvlCore ah_core_rw(AvlHash *h) {
    AvlCore c = ah_core_ro(h);
    c.free_head = &h->free_head;
    c.count     = &h->count;
    return c;
}

static size_t bucket_of(const AvlHash *h, const void *key) {
    return (size_t)(h->hash(key) % h->n_buckets);
}

/* ---- sizing ---------------------------------------------------- */

size_t avlhash_pool_bytes(size_t capacity, size_t element_size) {
    return capacity * (AVLHASH_NODE_OVERHEAD + element_size);
}

/* ---- init / clear --------------------------------------------- */

bool avlhash_init(AvlHash *h, uint32_t *buckets, size_t n_buckets,
                  avlhash_hash_fn hash, avlhash_cmp_fn cmp,
                  void *pool, size_t pool_bytes, size_t element_size) {
    if (!h || !buckets || n_buckets == 0 || !hash || !cmp ||
        !pool || element_size == 0) {
        return false;
    }

    size_t stride = AVLHASH_NODE_OVERHEAD + element_size;
    size_t capacity = pool_bytes / stride;
    if (capacity == 0 || capacity >= AVLHASH_NIL) return false;

    h->buckets      = buckets;
    h->n_buckets    = n_buckets;
    h->pool         = (uint8_t *)pool;
    h->capacity     = capacity;
    h->element_size = element_size;
    h->stride       = stride;
    h->hash         = hash;
    h->cmp          = cmp;
    avlhash_clear(h);
    return true;
}

void avlhash_clear(AvlHash *h) {
    if (!h) return;
    h->count = 0;
    for (size_t i = 0; i < h->n_buckets; i++) h->buckets[i] = AVLHASH_NIL;
    avl_core_build_freelist(h->pool, h->stride, (uint32_t)h->capacity,
                            &h->free_head);
}

/* ---- mutate ---------------------------------------------------- */

int avlhash_insert(AvlHash *h, const void *elem) {
    if (!h || !elem) return 0;
    size_t b = bucket_of(h, elem);
    AvlCore c = ah_core_rw(h);
    return avl_core_insert(&c, &h->buckets[b], elem, /*balance=*/1);
}

int avlhash_remove(AvlHash *h, const void *key, void *out) {
    if (!h || !key) return 0;
    size_t b = bucket_of(h, key);
    AvlCore c = ah_core_rw(h);
    return avl_core_remove(&c, &h->buckets[b], key, out, /*balance=*/1);
}

/* ---- query ----------------------------------------------------- */

int avlhash_find(const AvlHash *h, const void *key, void *out) {
    if (!h || !key) return 0;
    size_t b = bucket_of(h, key);
    AvlCore c = ah_core_ro(h);
    uint32_t n = avl_core_find(&c, h->buckets[b], key);
    if (n == AVLHASH_NIL) return 0;
    if (out) memcpy(out, avl_core_payload(&c, n), h->element_size);
    return 1;
}

bool avlhash_contains(const AvlHash *h, const void *key) {
    if (!h || !key) return false;
    size_t b = bucket_of(h, key);
    AvlCore c = ah_core_ro(h);
    return avl_core_find(&c, h->buckets[b], key) != AVLHASH_NIL;
}

size_t avlhash_count(const AvlHash *h) { return h ? h->count : 0; }
bool   avlhash_empty(const AvlHash *h) { return !h || h->count == 0; }
bool   avlhash_full(const AvlHash *h)  { return h && h->count >= h->capacity; }

/* ---- iteration ------------------------------------------------- */

void avlhash_foreach(const AvlHash *h, avlhash_visit_fn visit, void *user) {
    if (!h || !visit) return;
    AvlCore c = ah_core_ro(h);
    for (size_t b = 0; b < h->n_buckets; b++) {
        for (uint32_t it = avl_core_min(&c, h->buckets[b]);
             it != AVLHASH_NIL; it = avl_core_next(&c, it)) {
            visit(avl_core_payload(&c, it), user);
        }
    }
}
