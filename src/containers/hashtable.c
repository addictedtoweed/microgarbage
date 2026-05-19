/* ============================================================
 *  hashtable.c — implementation of the generic hash table.
 *  See hashtable.h for the public contract.
 * ============================================================ */

#include "containers/hashtable.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HT_DEFAULT_BUCKETS  16

/* Resize trigger. When count exceeds buckets * HT_MAX_LOAD_NUM
 * / HT_MAX_LOAD_DEN, the table doubles in size. Stored as a
 * fraction so the comparison can stay in integer math. 3/4 =
 * load factor 0.75, the standard general-purpose threshold. */
#define HT_MAX_LOAD_NUM  3
#define HT_MAX_LOAD_DEN  4

typedef struct Entry {
    char         *key;       /* table-owned copy of the caller's key   */
    void         *value;     /* caller-owned                            */
    uint32_t      hash;      /* cached so resize doesn't recompute      */
    struct Entry *next;      /* chain within bucket                     */
} Entry;

struct HashTable {
    Entry       **buckets;
    size_t        bucket_count;   /* always a power of 2                */
    size_t        item_count;
    ht_alloc_fn   alloc;          /* this table's allocator             */
    ht_free_fn    free_fn;        /* paired with alloc; never NULL      */
};

/* ============================================================
 *  Internal helpers
 * ============================================================ */

/* 32-bit FNV-1a hash of a NUL-terminated string. Fast, no
 * seeding, good enough distribution for short ASCII keys. */
static uint32_t ht_hash(const char *s) {
    uint32_t h = 0x811C9DC5u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 0x01000193u;
    }
    return h;
}

/* Round `n` up to the next power of two, with a minimum of 1.
 * Used so bucket_count is always a power of 2 — that lets us
 * mod with `& (bucket_count - 1)` instead of `%`, which is a
 * meaningful win when bucket_count isn't a compile-time constant. */
static size_t next_pow2(size_t n) {
    if (n < 2) return 1;
    size_t p = 1;
    while (p < n) p <<= 1;
    return p;
}

/* Copy a string into a fresh buffer obtained from the given
 * allocator. The hashtable can't assume strdup exists (POSIX,
 * not C standard) — and we need to route the allocation through
 * the table's allocator anyway. Returns NULL on alloc failure. */
static char *ht_strdup_with(ht_alloc_fn alloc, const char *s) {
    size_t n = strlen(s) + 1;
    char *p = alloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Allocate a zeroed bucket array of `count` slots using the
 * given allocator. The contents are explicitly memset to NULL
 * since we can't assume the user's allocator zeroes memory. */
static Entry **alloc_buckets(ht_alloc_fn alloc, size_t count) {
    size_t bytes = count * sizeof(Entry*);
    Entry **buckets = alloc(bytes);
    if (buckets) memset(buckets, 0, bytes);
    return buckets;
}

/* Resize the table to `new_bucket_count` slots, rehashing all
 * existing entries into the new array. Returns 0 on success, -1
 * on allocation failure (in which case the table is left
 * unchanged). */
static int ht_resize(HashTable *ht, size_t new_bucket_count) {
    Entry **new_buckets = alloc_buckets(ht->alloc, new_bucket_count);
    if (!new_buckets) return -1;

    size_t mask = new_bucket_count - 1;

    /* Walk every entry in the old buckets and re-link it into the
     * new array at the position dictated by its cached hash. */
    for (size_t i = 0; i < ht->bucket_count; i++) {
        Entry *e = ht->buckets[i];
        while (e) {
            Entry *next = e->next;
            size_t b = e->hash & mask;
            e->next = new_buckets[b];
            new_buckets[b] = e;
            e = next;
        }
    }

    ht->free_fn(ht->buckets);
    ht->buckets = new_buckets;
    ht->bucket_count = new_bucket_count;
    return 0;
}

/* True if adding one more entry would push us past the load
 * threshold. Computed in integer math to avoid floats. */
static int ht_needs_grow(const HashTable *ht) {
    return (ht->item_count + 1) * HT_MAX_LOAD_DEN
         > ht->bucket_count * HT_MAX_LOAD_NUM;
}

/* Find the entry for `key` in `ht` and return a pointer to the
 * pointer that points to it (suitable for unlink). Returns NULL
 * if the key is absent. Used by ht_get (dereference once) and
 * ht_delete (splice out). */
static Entry **ht_find_slot(const HashTable *ht, const char *key,
                            uint32_t *out_hash) {
    uint32_t h = ht_hash(key);
    if (out_hash) *out_hash = h;

    size_t b = h & (ht->bucket_count - 1);
    Entry **slot = (Entry **)&ht->buckets[b];
    while (*slot) {
        if ((*slot)->hash == h && strcmp((*slot)->key, key) == 0) {
            return slot;
        }
        slot = &(*slot)->next;
    }
    return NULL;
}

/* ============================================================
 *  Public API
 * ============================================================ */

HashTable *ht_create(size_t initial_buckets) {
    return ht_create_with_allocator(initial_buckets, malloc, free);
}

HashTable *ht_create_with_allocator(size_t initial_buckets,
                                    ht_alloc_fn alloc,
                                    ht_free_fn  free_fn) {
    /* NULL means "use the default for this slot." Embedded users
     * who really don't want malloc can pass their own; everyone
     * else gets the C standard heap. */
    if (!alloc)   alloc   = malloc;
    if (!free_fn) free_fn = free;

    HashTable *ht = alloc(sizeof *ht);
    if (!ht) return NULL;
    memset(ht, 0, sizeof *ht);

    ht->alloc   = alloc;
    ht->free_fn = free_fn;

    if (initial_buckets == 0) initial_buckets = HT_DEFAULT_BUCKETS;
    initial_buckets = next_pow2(initial_buckets);

    ht->buckets = alloc_buckets(alloc, initial_buckets);
    if (!ht->buckets) {
        free_fn(ht);
        return NULL;
    }
    ht->bucket_count = initial_buckets;
    ht->item_count = 0;
    return ht;
}

void ht_destroy(HashTable *ht, ht_value_free_fn value_free) {
    if (!ht) return;

    for (size_t i = 0; i < ht->bucket_count; i++) {
        Entry *e = ht->buckets[i];
        while (e) {
            Entry *next = e->next;
            if (value_free) value_free(e->value);
            ht->free_fn(e->key);
            ht->free_fn(e);
            e = next;
        }
    }

    ht->free_fn(ht->buckets);
    ht->free_fn(ht);
}

void *ht_get(const HashTable *ht, const char *key) {
    Entry **slot = ht_find_slot(ht, key, NULL);
    return slot ? (*slot)->value : NULL;
}

int ht_set(HashTable *ht, const char *key, void *value) {
    /* Update path: key already present. */
    uint32_t h;
    Entry **slot = ht_find_slot(ht, key, &h);
    if (slot) {
        (*slot)->value = value;
        return 0;
    }

    /* Insert path: grow first if needed. After a grow, h is still
     * correct (we cached it from ht_find_slot above), but the
     * bucket index changes — recompute below. */
    if (ht_needs_grow(ht)) {
        if (ht_resize(ht, ht->bucket_count * 2) != 0) return -1;
    }

    Entry *e = ht->alloc(sizeof *e);
    if (!e) return -1;
    e->key = ht_strdup_with(ht->alloc, key);
    if (!e->key) { ht->free_fn(e); return -1; }
    e->value = value;
    e->hash  = h;

    size_t b = h & (ht->bucket_count - 1);
    e->next = ht->buckets[b];
    ht->buckets[b] = e;
    ht->item_count++;
    return 0;
}

int ht_delete(HashTable *ht, const char *key, ht_value_free_fn value_free) {
    Entry **slot = ht_find_slot(ht, key, NULL);
    if (!slot) return 0;

    Entry *e = *slot;
    *slot = e->next;            /* splice out */

    if (value_free) value_free(e->value);
    ht->free_fn(e->key);
    ht->free_fn(e);
    ht->item_count--;
    return 1;
}

size_t ht_count(const HashTable *ht) {
    return ht->item_count;
}
