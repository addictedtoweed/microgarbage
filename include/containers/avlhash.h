/* ============================================================
 *  avlhash.h — hash table with AVL-tree buckets over one node pool.
 *
 *  A SECOND hashing mechanism alongside hashtable.h. Where hashtable.h
 *  is malloc-backed, string-keyed, and resolves collisions with
 *  open chains, this one is:
 *    - caller-provides-the-pool (no malloc); fits a fixed RAM budget,
 *    - generic: any fixed-size payload, with a caller-supplied hash
 *      AND comparator,
 *    - AVL-bucketed: each bucket is a height-balanced tree, so a hot
 *      or adversarial bucket degrades to O(log n), never O(n). On a
 *      fast MCU the log-n insert/traverse is well within budget.
 *
 *  ONE combined node pool backs ALL buckets: every entry is the same
 *  16-byte-overhead AVL node, handed out from a single shared free
 *  list (see avl_core). You size the pool once for the TOTAL number of
 *  live entries across the whole table, independent of bucket count.
 *
 *  The bucket count is fixed at init (no rehash/grow — this is the
 *  no-malloc tradeoff). Pick it for spread; collisions cost a tree
 *  level, not a probe storm.
 *
 *  Ordering within a bucket follows the comparator; iteration yields
 *  bucket-by-bucket, in-order within each bucket (NOT globally sorted
 *  — it's a hash). Duplicate keys (compare == 0) are rejected by
 *  insert (set semantics).
 *
 *  Sizing:
 *      uint32_t buckets[16];
 *      uint8_t  pool[AVLHASH_POOL_BYTES(64, sizeof(Entry))];
 *      AvlHash  h;
 *      avlhash_init(&h, buckets, 16, my_hash, my_cmp,
 *                   pool, sizeof pool, sizeof(Entry));
 *
 *  Thread safety: none. Single-context use only.
 *
 *  Depends on: containers_config.h (default sizes).
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef AVLHASH_H
#define AVLHASH_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "containers/containers_config.h"  /* GARBAGE_AVLHASH_DEFAULT_* */

#define AVLHASH_NIL  0xFFFFFFFFu

/* Per-node overhead: left + right + parent indices + int8 balance +
 * 3 pad — the shared avl_core node shape. (Asserted equal to
 * AVL_CORE_NODE_OVERHEAD in avlhash.c.) */
#define AVLHASH_NODE_OVERHEAD  (3u * sizeof(uint32_t) + 4u)

/* Bytes of node pool for `cap` TOTAL entries of `elem` bytes
 * (compile-time). Sizes the combined pool across all buckets. */
#define AVLHASH_POOL_BYTES(cap, elem) \
    ((size_t)(cap) * (AVLHASH_NODE_OVERHEAD + (size_t)(elem)))

/* Pool bytes for the configured default entry count (override via
 * GARBAGE_AVLHASH_DEFAULT_NODES). */
#define AVLHASH_DEFAULT_POOL_BYTES(elem) \
    AVLHASH_POOL_BYTES(GARBAGE_AVLHASH_DEFAULT_NODES, (elem))

/* Hash of a payload/key -> bucket selector (taken mod bucket count). */
typedef uint32_t (*avlhash_hash_fn)(const void *key);

/* Comparator: <0 if a<b, 0 if equal, >0 if a>b (orders within a
 * bucket's AVL tree and detects duplicate keys). */
typedef int (*avlhash_cmp_fn)(const void *a, const void *b);

typedef struct {
    uint32_t       *buckets;      /* bucket roots, n_buckets entries  */
    size_t          n_buckets;
    uint8_t        *pool;         /* shared entry-node pool           */
    size_t          capacity;     /* max entry nodes                  */
    size_t          element_size; /* payload bytes per entry          */
    size_t          stride;       /* overhead + element_size          */
    size_t          count;        /* live entries across all buckets  */
    uint32_t        free_head;    /* shared free-list head            */
    avlhash_hash_fn hash;
    avlhash_cmp_fn  cmp;
} AvlHash;

/* Runtime pool sizing — matches AVLHASH_POOL_BYTES. */
size_t avlhash_pool_bytes(size_t capacity, size_t element_size);

/* Initialize. `buckets` is a caller-owned array of `n_buckets`
 * roots (set to empty here); `pool` is the shared node buffer, at
 * least avlhash_pool_bytes(capacity, element_size). Both must outlive
 * the table. hash + cmp are fixed for its life. Returns true on
 * success, false on bad args / pool too small / n_buckets == 0. */
bool avlhash_init(AvlHash *h, uint32_t *buckets, size_t n_buckets,
                  avlhash_hash_fn hash, avlhash_cmp_fn cmp,
                  void *pool, size_t pool_bytes, size_t element_size);

/* Reset to empty without touching the pool memory. */
void avlhash_clear(AvlHash *h);

/* Insert a copy of *elem. Returns 1 on success, 0 if the pool is
 * exhausted OR a matching key already exists (set semantics). */
int avlhash_insert(AvlHash *h, const void *elem);

/* Find the entry matching *key (hash + comparator). Copies it into
 * *out if non-NULL. Returns 1 if found, 0 if not. */
int avlhash_find(const AvlHash *h, const void *key, void *out);

/* True if a matching key is present. */
bool avlhash_contains(const AvlHash *h, const void *key);

/* Remove the entry matching *key; copies the removed payload into
 * *out if non-NULL. Returns 1 if found+removed, 0 if absent. */
int avlhash_remove(AvlHash *h, const void *key, void *out);

/* Count + predicates. */
size_t avlhash_count(const AvlHash *h);
bool   avlhash_empty(const AvlHash *h);
bool   avlhash_full(const AvlHash *h);

/* Visit every entry: bucket by bucket, in-order within each bucket
 * (not globally sorted). The callback must not mutate the table. */
typedef void (*avlhash_visit_fn)(const void *elem, void *user);
void avlhash_foreach(const AvlHash *h, avlhash_visit_fn visit, void *user);

#endif /* AVLHASH_H */
