/* ============================================================
 *  hashtable.h — generic string-keyed hash table
 *
 *  A reusable open-chaining hash table with FNV-1a hashing,
 *  NUL-terminated string keys, and opaque void-pointer values.
 *  The table is self-contained: pass around HashTable* and don't
 *  worry about how it manages its buckets.
 *
 *  Key ownership: the table copies every key, so callers can
 *  pass stack-allocated or temporary key strings. Keys are freed
 *  when entries are deleted or when the table is destroyed.
 *
 *  Value ownership: values are opaque void* and are not freed by
 *  the table by default. Pass a value_free callback to ht_destroy
 *  or ht_delete if you want the table to free values for you.
 *
 *  Resize: the table grows automatically when the load factor
 *  exceeds 0.75. Bucket count doubles on each grow.
 *
 *  Allocation: by default, the table uses malloc and free. You
 *  can pass a custom allocator at create time via the
 *  ht_create_with_allocator() entry point. The allocator pair is
 *  stored on the table and used for that table's entire lifetime,
 *  so two tables in the same program can use different allocators
 *  without conflict. Embedded users with a fixed pool typically
 *  pass an alloc function that bumps through the pool and a free
 *  function that's a no-op.
 *
 *  IMPORTANT: a table allocated with allocator A must not be
 *  freed by allocator B. ht_destroy uses the table's own stored
 *  free function, so this is automatic — just don't try to share
 *  pointers across tables with different allocators.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef HASHTABLE_H
#define HASHTABLE_H

#include <stddef.h>

/* Opaque handle. Full struct is private to hashtable.c. */
typedef struct HashTable HashTable;

/* Optional callback for freeing values. Pass NULL to ht_destroy
 * or ht_delete if the caller manages value lifetimes independently. */
typedef void (*ht_value_free_fn)(void *value);

/* Custom-allocator function pointer types. Signatures match the
 * C standard malloc/free so most allocators can be passed in
 * without adapter wrappers. */
typedef void *(*ht_alloc_fn)(size_t n);
typedef void  (*ht_free_fn)(void *p);

/* Create an empty hash table using malloc/free. `initial_buckets`
 * is a hint; pass 0 to use the default (16). The table grows
 * automatically as items are added. Returns NULL on out-of-memory. */
HashTable *ht_create(size_t initial_buckets);

/* Create an empty hash table using a custom allocator. `alloc`
 * and `free_fn` are stored on the table and used for every
 * allocation the table does over its lifetime, including resizes
 * and the eventual ht_destroy. Pass NULL for either to use the
 * corresponding default (malloc / free). Returns NULL on alloc
 * failure. */
HashTable *ht_create_with_allocator(size_t initial_buckets,
                                    ht_alloc_fn alloc,
                                    ht_free_fn  free_fn);

/* Tear down a hash table. Frees every key copy and the bucket
 * array using the table's own allocator. If value_free is
 * non-NULL, calls it on each stored value before freeing the
 * bucket. Safe to call with NULL ht. */
void ht_destroy(HashTable *ht, ht_value_free_fn value_free);

/* Look up a value by key. Returns NULL if the key is not present.
 * Note: NULL is also a valid stored value, so use ht_get for
 * presence checks only when you know NULL won't be stored. */
void *ht_get(const HashTable *ht, const char *key);

/* Insert or update a value under `key`. If the key already exists,
 * its value is replaced (the old value is NOT freed — the caller
 * is responsible for any cleanup of the previous value, which can
 * be retrieved via ht_get before calling ht_set). Returns 0 on
 * success, -1 on allocation failure. */
int ht_set(HashTable *ht, const char *key, void *value);

/* Remove an entry. If value_free is non-NULL, calls it on the
 * stored value. Returns 1 if the key was present and removed,
 * 0 if it was absent. */
int ht_delete(HashTable *ht, const char *key, ht_value_free_fn value_free);

/* Number of entries currently in the table. */
size_t ht_count(const HashTable *ht);

#endif /* HASHTABLE_H */
