/* ============================================================
 *  audio_pool.c — block pool allocator implementation
 *
 *  See audio_pool.h for the model. All metadata is out of band; the
 *  blocks themselves hold only payload.
 *
 *  Handle packing: (generation << 16) | (slot + 1). Slot+1 so that a
 *  zero handle is always invalid (slot 0 would otherwise pack to 0
 *  at generation 0). Generation is 16-bit and wraps; a collision
 *  needs 65536 frees of the same slot, acceptable for this use.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_pool.h"

#include <stdlib.h>
#include <string.h>

/* ---- handle pack/unpack ---- */

static AudioObjHandle pack_handle(uint32_t slot, uint16_t gen) {
    return ((AudioObjHandle)gen << 16) | (slot + 1u);
}
static bool unpack_handle(AudioObjHandle h, uint32_t *slot, uint16_t *gen) {
    if (h == AUDIO_POOL_HANDLE_NONE) return false;
    uint32_t s = (h & 0xFFFFu);
    if (s == 0) return false;
    *slot = s - 1u;
    *gen  = (uint16_t)(h >> 16);
    return true;
}

/* Resolve a handle to a live object pointer, or NULL if invalid /
 * stale / not in use. */
static AudioObject *resolve(const AudioPool *p, AudioObjHandle h) {
    uint32_t slot; uint16_t gen;
    if (!unpack_handle(h, &slot, &gen)) return NULL;
    if (slot >= AUDIO_POOL_MAX_OBJECTS) return NULL;
    const AudioObject *o = &p->objects[slot];
    if (!o->in_use) return NULL;
    if (o->generation != gen) return NULL;     /* stale handle */
    return (AudioObject *)o;
}

/* ---- block free map (bitset; set bit = allocated) ---- */

/* Allocate one free block, or NO_BLOCK. Updates the map + free count.
 * The bitset scans words with ctz from a rolling hint — O(1) amortized
 * even at thousands of blocks (the old code was a from-0 linear scan). */
static uint32_t alloc_one_block(AudioPool *p) {
    if (p->free_blocks == 0) return AUDIO_POOL_NO_BLOCK;
    size_t b = bitset_find_first_clear(&p->used_map);
    if (b == BITSET_NPOS) return AUDIO_POOL_NO_BLOCK;   /* shouldn't happen */
    bitset_set(&p->used_map, b);
    p->free_blocks--;
    return (uint32_t)b;
}

static void free_one_block(AudioPool *p, uint32_t b) {
    if (b >= p->block_count) return;
    if (bitset_test(&p->used_map, b)) {
        bitset_clear(&p->used_map, b);
        p->free_blocks++;
    }
    p->owner[b] = AUDIO_POOL_NO_BLOCK;
    p->next[b]  = AUDIO_POOL_NO_BLOCK;
}

/* Free an object's whole block chain. */
static void free_chain(AudioPool *p, uint32_t head) {
    uint32_t b = head;
    while (b != AUDIO_POOL_NO_BLOCK) {
        uint32_t nxt = p->next[b];
        free_one_block(p, b);
        b = nxt;
    }
}

/* ---- lifecycle ---- */

AudioPoolResult audio_pool_init(AudioPool *p, void *region, size_t region_size) {
    if (!p || !region) return AUDIO_POOL_ERR_INVALID_ARG;

    memset(p, 0, sizeof(*p));
    p->region      = (uint8_t *)region;
    p->region_size = region_size;
    p->block_size  = AUDIO_POOL_BLOCK_SIZE;
    p->block_count = (uint32_t)(region_size / AUDIO_POOL_BLOCK_SIZE);
    if (p->block_count == 0) return AUDIO_POOL_ERR_INVALID_ARG;

    size_t map_words = bitset_words(p->block_count);
    p->owner      = malloc(p->block_count * sizeof(uint32_t));
    p->next       = malloc(p->block_count * sizeof(uint32_t));
    p->used_words = malloc(map_words * sizeof(uint32_t));
    if (!p->owner || !p->next || !p->used_words) {
        free(p->owner); free(p->next); free(p->used_words);
        p->owner = p->next = NULL; p->used_words = NULL;
        return AUDIO_POOL_ERR_NO_SPACE;
    }

    for (uint32_t b = 0; b < p->block_count; b++) {
        p->owner[b] = AUDIO_POOL_NO_BLOCK;
        p->next[b]  = AUDIO_POOL_NO_BLOCK;
    }
    /* set bit = allocated; start all clear (every block free). The
     * bitset masks tail bits beyond block_count, so no padding fixup. */
    bitset_init(&p->used_map, p->used_words, p->block_count);
    bitset_clear_all(&p->used_map);
    p->free_blocks = p->block_count;

    /* generations start at 1 so a fresh slot never matches a 0 gen
     * from a zeroed handle. */
    for (uint32_t s = 0; s < AUDIO_POOL_MAX_OBJECTS; s++) {
        p->generations[s] = 1;
        p->objects[s].in_use = false;
    }
    return AUDIO_POOL_OK;
}

void audio_pool_destroy(AudioPool *p) {
    if (!p) return;
    free(p->owner); free(p->next); free(p->used_words);
    p->owner = p->next = NULL; p->used_words = NULL;
}

/* ---- object allocation ---- */

static uint32_t find_free_slot(const AudioPool *p) {
    for (uint32_t s = 0; s < AUDIO_POOL_MAX_OBJECTS; s++) {
        if (!p->objects[s].in_use) return s;
    }
    return AUDIO_POOL_NO_BLOCK;   /* table full */
}

AudioPoolResult audio_pool_alloc(AudioPool *p, uint32_t size,
                                 uint16_t owner_vm,
                                 AudioObjHandle *out_handle) {
    if (!p || !out_handle) return AUDIO_POOL_ERR_INVALID_ARG;
    *out_handle = AUDIO_POOL_HANDLE_NONE;

    uint32_t need = (size + p->block_size - 1u) / p->block_size;
    if (need == 0) need = 1;                /* zero-size object still gets 1 block */
    if (need > p->free_blocks) return AUDIO_POOL_ERR_NO_SPACE;

    uint32_t slot = find_free_slot(p);
    if (slot == AUDIO_POOL_NO_BLOCK) return AUDIO_POOL_ERR_NO_OBJECTS;

    /* Build the chain. Since we pre-checked free_blocks >= need, the
     * per-block allocs cannot fail; but guard anyway and roll back. */
    uint32_t head = AUDIO_POOL_NO_BLOCK, prev = AUDIO_POOL_NO_BLOCK;
    for (uint32_t i = 0; i < need; i++) {
        uint32_t b = alloc_one_block(p);
        if (b == AUDIO_POOL_NO_BLOCK) { free_chain(p, head); return AUDIO_POOL_ERR_NO_SPACE; }
        p->owner[b] = slot;
        p->next[b]  = AUDIO_POOL_NO_BLOCK;
        if (prev == AUDIO_POOL_NO_BLOCK) head = b;
        else                             p->next[prev] = b;
        prev = b;
    }

    AudioObject *o = &p->objects[slot];
    o->head_block = head;
    o->size       = size;
    o->nblocks    = need;
    o->refcount   = 1;
    o->generation = p->generations[slot];
    o->owner_vm   = owner_vm;
    o->sample_rate = 0;   /* default: assume mixer rate (no resample) */
    o->in_use     = true;

    *out_handle = pack_handle(slot, o->generation);
    return AUDIO_POOL_OK;
}

AudioPoolResult audio_pool_alloc_with_rate(AudioPool *p, uint32_t size,
                                            uint16_t owner_vm,
                                            uint32_t sample_rate,
                                            AudioObjHandle *out_handle) {
    AudioPoolResult r = audio_pool_alloc(p, size, owner_vm, out_handle);
    if (r != AUDIO_POOL_OK) return r;
    /* Stash the rate on the freshly-allocated slot. Handle is
     * (gen << 16) | (slot + 1), per pack_handle above. */
    uint32_t slot;
    uint16_t gen;
    if (unpack_handle(*out_handle, &slot, &gen)
        && slot < AUDIO_POOL_MAX_OBJECTS
        && p->objects[slot].in_use) {
        p->objects[slot].sample_rate = sample_rate;
    }
    return AUDIO_POOL_OK;
}

uint32_t audio_pool_object_sample_rate(const AudioPool *p, AudioObjHandle h) {
    if (!p) return 0;
    uint32_t slot;
    uint16_t gen;
    if (!unpack_handle(h, &slot, &gen)) return 0;
    if (slot >= AUDIO_POOL_MAX_OBJECTS) return 0;
    const AudioObject *o = &p->objects[slot];
    if (!o->in_use) return 0;
    if (gen != o->generation) return 0;
    return o->sample_rate;
}

static void free_object(AudioPool *p, uint32_t slot) {
    AudioObject *o = &p->objects[slot];
    free_chain(p, o->head_block);
    /* bump generation so outstanding handles to this slot go stale */
    p->generations[slot] = (uint16_t)(p->generations[slot] + 1u);
    if (p->generations[slot] == 0) p->generations[slot] = 1; /* skip 0 */
    memset(o, 0, sizeof(*o));
    o->in_use = false;
    o->head_block = AUDIO_POOL_NO_BLOCK;
}

AudioPoolResult audio_pool_ref(AudioPool *p, AudioObjHandle h) {
    if (!p) return AUDIO_POOL_ERR_INVALID_ARG;
    AudioObject *o = resolve(p, h);
    if (!o) return AUDIO_POOL_ERR_BAD_HANDLE;
    o->refcount++;
    return AUDIO_POOL_OK;
}

AudioPoolResult audio_pool_unref(AudioPool *p, AudioObjHandle h, bool *freed) {
    if (freed) *freed = false;
    if (!p) return AUDIO_POOL_ERR_INVALID_ARG;
    uint32_t slot; uint16_t gen;
    if (!unpack_handle(h, &slot, &gen)) return AUDIO_POOL_ERR_BAD_HANDLE;
    AudioObject *o = resolve(p, h);
    if (!o) return AUDIO_POOL_ERR_BAD_HANDLE;
    if (--o->refcount <= 0) {
        free_object(p, slot);
        if (freed) *freed = true;
    }
    return AUDIO_POOL_OK;
}

AudioPoolResult audio_pool_sweep_vm(AudioPool *p, uint16_t owner_vm,
                                    uint32_t *out_freed) {
    if (!p) return AUDIO_POOL_ERR_INVALID_ARG;
    uint32_t freed = 0;
    for (uint32_t s = 0; s < AUDIO_POOL_MAX_OBJECTS; s++) {
        AudioObject *o = &p->objects[s];
        if (o->in_use && o->owner_vm == owner_vm) {
            if (--o->refcount <= 0) {
                free_object(p, s);
                freed++;
            }
        }
    }
    if (out_freed) *out_freed = freed;
    return AUDIO_POOL_OK;
}

/* ---- chained data access ---- */

/* Walk to the block containing byte `offset`, returning its block
 * index and the in-block byte offset. Returns NO_BLOCK if offset is
 * beyond the chain. */
static uint32_t seek_block(const AudioPool *p, uint32_t head,
                           uint32_t offset, uint32_t *in_block_off) {
    uint32_t b = head;
    uint32_t skip = offset / p->block_size;
    *in_block_off = offset % p->block_size;
    while (skip-- > 0 && b != AUDIO_POOL_NO_BLOCK) {
        b = p->next[b];
    }
    return b;
}

AudioPoolResult audio_pool_read(const AudioPool *p, AudioObjHandle h,
                                uint32_t offset, void *dst, uint32_t n,
                                uint32_t *out_copied) {
    if (out_copied) *out_copied = 0;
    if (!p || !dst) return AUDIO_POOL_ERR_INVALID_ARG;
    AudioObject *o = resolve(p, h);
    if (!o) return AUDIO_POOL_ERR_BAD_HANDLE;

    if (offset >= o->size) return AUDIO_POOL_OK;          /* nothing to read */
    uint32_t avail = o->size - offset;
    if (n > avail) n = avail;                             /* clamp to object */

    uint32_t in_off;
    uint32_t b = seek_block(p, o->head_block, offset, &in_off);
    uint8_t *out = (uint8_t *)dst;
    uint32_t copied = 0;
    while (copied < n && b != AUDIO_POOL_NO_BLOCK) {
        uint32_t chunk = p->block_size - in_off;
        if (chunk > n - copied) chunk = n - copied;
        memcpy(out + copied, p->region + (size_t)b * p->block_size + in_off, chunk);
        copied += chunk;
        in_off = 0;
        b = p->next[b];
    }
    if (out_copied) *out_copied = copied;
    return AUDIO_POOL_OK;
}

AudioPoolResult audio_pool_write(AudioPool *p, AudioObjHandle h,
                                 uint32_t offset, const void *src, uint32_t n,
                                 uint32_t *out_written) {
    if (out_written) *out_written = 0;
    if (!p || !src) return AUDIO_POOL_ERR_INVALID_ARG;
    AudioObject *o = resolve(p, h);
    if (!o) return AUDIO_POOL_ERR_BAD_HANDLE;

    if (offset >= o->size) return AUDIO_POOL_OK;
    uint32_t avail = o->size - offset;
    if (n > avail) n = avail;

    uint32_t in_off;
    uint32_t b = seek_block(p, o->head_block, offset, &in_off);
    const uint8_t *in = (const uint8_t *)src;
    uint32_t written = 0;
    while (written < n && b != AUDIO_POOL_NO_BLOCK) {
        uint32_t chunk = p->block_size - in_off;
        if (chunk > n - written) chunk = n - written;
        memcpy(p->region + (size_t)b * p->block_size + in_off, in + written, chunk);
        written += chunk;
        in_off = 0;
        b = p->next[b];
    }
    if (out_written) *out_written = written;
    return AUDIO_POOL_OK;
}

/* ---- introspection ---- */

uint32_t audio_pool_free_blocks(const AudioPool *p) {
    return p ? p->free_blocks : 0;
}
uint32_t audio_pool_block_count(const AudioPool *p) {
    return p ? p->block_count : 0;
}
int32_t audio_pool_refcount(const AudioPool *p, AudioObjHandle h) {
    if (!p) return -1;
    AudioObject *o = resolve(p, h);
    return o ? o->refcount : -1;
}
bool audio_pool_handle_valid(const AudioPool *p, AudioObjHandle h) {
    return p && resolve(p, h) != NULL;
}
uint32_t audio_pool_object_size(const AudioPool *p, AudioObjHandle h) {
    if (!p) return 0;
    AudioObject *o = resolve(p, h);
    return o ? o->size : 0;
}
