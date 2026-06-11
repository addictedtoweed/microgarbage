/* ============================================================
 *  audio_pool.h — fragmentation-free block pool for audio data
 *
 *  Manages a region of "special" audio memory (PSRAM on the
 *  STM32H745; a plain malloc'd region on the desktop) as fixed-size
 *  blocks, handing out refcounted OBJECT handles. This is the
 *  samples-at-rest store from docs/audio-architecture.md — songs,
 *  VA streams, and SFX live here; the mixer streams from it into
 *  contiguous SRAM staging buffers.
 *
 *  ---------------------------------------------------------------
 *  Why blocks (and why out-of-band metadata)
 *  ---------------------------------------------------------------
 *  Fixed-size blocks mean allocation NEVER fails from fragmentation
 *  (only from genuine exhaustion) — important for a long-running
 *  music system where songs come and go. The cost is that an
 *  object's data may be non-contiguous (a chain of blocks); that is
 *  fine because the mixer reads it sequentially into a contiguous
 *  staging buffer (the non-contiguity never reaches the DMA).
 *
 *  All bookkeeping is OUT OF BAND — parallel arrays indexed by block
 *  number, NOT headers inside the blocks:
 *      free bitmap          — 1 bit per block
 *      owner[block]         — which object owns this block (or NONE)
 *      next[block]          — next block in this object's chain (FAT-
 *                             style), or NONE at the end
 *  Keeping metadata out of the blocks leaves the block payload as
 *  pure PCM, so a staging DMA copies clean audio with no header to
 *  step over. (Same lesson as trashfs's separated data blocks.)
 *
 *  ---------------------------------------------------------------
 *  Handles, generations, refcounts
 *  ---------------------------------------------------------------
 *  An object handle is a packed (slot, generation) pair. The slot
 *  indexes the object descriptor table; the generation is bumped
 *  each time a slot is freed, so a stale handle to a since-reused
 *  slot is detected and rejected rather than silently aliasing a
 *  different object. Handle 0 is reserved as "invalid/none".
 *
 *  Each live object carries a refcount. Cross-VM sharing makes
 *  lifetime a refcount problem, not a lock problem: an object stays
 *  alive while any reference exists (a holding VM, or a playing
 *  voice); the last drop frees its blocks. A dying VM drops its refs
 *  (orphan sweep) rather than hard-freeing blocks another VM may
 *  still use.
 *
 *  ---------------------------------------------------------------
 *  Concurrency
 *  ---------------------------------------------------------------
 *  The pool is NOT internally locked. It is designed to run on the
 *  single service context (the M4 / desktop worker thread), mutated
 *  only by serialized request processing drained from the service
 *  channel. So there is no concurrent access to guard here — the
 *  channel is the synchronization boundary, upstream of this. Do not
 *  call pool functions from two threads concurrently.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef AUDIO_POOL_H
#define AUDIO_POOL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "containers/bitset.h"   /* block free-map */

/* ---- tunables ---- */

#ifndef AUDIO_POOL_BLOCK_SIZE
#define AUDIO_POOL_BLOCK_SIZE   8192u   /* 8 KB — see audio note */
#endif

/* Max distinct live objects (descriptor table size). Independent of
 * block count; an object uses >=1 block. 256 is generous for a music
 * system (a handful of songs + many SFX). */
#ifndef AUDIO_POOL_MAX_OBJECTS
#define AUDIO_POOL_MAX_OBJECTS  256u
#endif

/* "no block" / "no object" sentinels (valid indices are < count). */
#define AUDIO_POOL_NO_BLOCK     0xFFFFFFFFu

/* Reserved invalid handle. */
#define AUDIO_POOL_HANDLE_NONE  0u

typedef uint32_t AudioObjHandle;   /* packed (generation<<16 | slot+1) */

/* ---- result codes ---- */
typedef enum {
    AUDIO_POOL_OK = 0,
    AUDIO_POOL_ERR_NO_SPACE,     /* not enough free blocks            */
    AUDIO_POOL_ERR_NO_OBJECTS,   /* descriptor table full             */
    AUDIO_POOL_ERR_BAD_HANDLE,   /* invalid / stale handle            */
    AUDIO_POOL_ERR_INVALID_ARG,
} AudioPoolResult;

/* ---- one object descriptor (in the table) ---- */
typedef struct {
    uint32_t head_block;   /* first block of the chain, or NO_BLOCK   */
    uint32_t size;         /* logical byte size of the object data    */
    uint32_t nblocks;      /* number of blocks in the chain           */
    int32_t  refcount;     /* live references; freed at 0             */
    uint16_t generation;   /* bumped on free; matched against handle  */
    uint16_t owner_vm;     /* vm_id that created it (for orphan sweep)*/
    bool     in_use;       /* slot occupied                           */
    /* Source sample rate (Hz). Set by audio_pool_alloc_with_rate when
     * the object is loaded; 0 means "assume mixer output rate" — the
     * mixer treats it as no-resample. Callers that need linear/cubic
     * interpolation at play time look this up via
     * audio_pool_object_sample_rate(). Per docs/audio-architecture.md:
     * per-channel source rate is the design intent; this is the per-
     * sample metadata that backs it. */
    uint32_t sample_rate;
} AudioObject;

/* ---- the pool ---- */
typedef struct {
    uint8_t  *region;        /* caller-owned audio memory (PSRAM base) */
    size_t    region_size;   /* bytes                                  */
    uint32_t  block_size;    /* AUDIO_POOL_BLOCK_SIZE                   */
    uint32_t  block_count;   /* region_size / block_size               */
    uint32_t  free_blocks;   /* current count of free blocks           */

    /* out-of-band metadata (caller-or-internally provided) */
    uint32_t *owner;         /* [block_count] object slot, or NO_BLOCK */
    uint32_t *next;          /* [block_count] next in chain, or NO_BLK */
    Bitset    used_map;      /* set bit = block allocated (see bitset.h)*/
    uint32_t *used_words;    /* backing store for used_map (malloc'd)   */

    AudioObject objects[AUDIO_POOL_MAX_OBJECTS];
    uint16_t    generations[AUDIO_POOL_MAX_OBJECTS]; /* live gen per slot */
} AudioPool;

/* ---- lifecycle ---- */

/* Initialize a pool over a caller-provided audio-memory region. The
 * region is carved into block_size blocks (the tail remainder, if
 * any, is unused). The out-of-band metadata arrays (owner, next,
 * used_map words) are allocated with malloc here and freed by
 * audio_pool_destroy — on the MCU these would instead be placed in a
 * fixed reserved region, but the logic is identical. Returns OK or
 * an error. */
AudioPoolResult audio_pool_init(AudioPool *p, void *region, size_t region_size);

/* Free the internally-allocated metadata arrays. Does not touch the
 * caller's region. */
void audio_pool_destroy(AudioPool *p);

/* ---- object allocation ---- */

/* Allocate an object of `size` bytes, owned by `owner_vm`, with an
 * initial refcount of 1. Carves ceil(size/block_size) blocks into a
 * chain. Returns the handle via *out_handle, or an error (and sets
 * *out_handle = AUDIO_POOL_HANDLE_NONE) if there isn't enough space
 * or the descriptor table is full. */
AudioPoolResult audio_pool_alloc(AudioPool *p, uint32_t size,
                                 uint16_t owner_vm,
                                 AudioObjHandle *out_handle);

/* Same as audio_pool_alloc but also records the sample rate of the
 * data the caller is about to write into the object. The mixer reads
 * this back at play time (via audio_pool_object_sample_rate) and
 * configures per-channel linear/cubic interpolation. Pass 0 to skip
 * the metadata (mixer assumes source == output rate, no resample). */
AudioPoolResult audio_pool_alloc_with_rate(AudioPool *p, uint32_t size,
                                           uint16_t owner_vm,
                                           uint32_t sample_rate,
                                           AudioObjHandle *out_handle);

/* Read back the source sample rate stored on a pool object (set by
 * audio_pool_alloc_with_rate). Returns 0 if the handle is invalid or
 * no rate was recorded — callers should interpret 0 as "assume
 * mixer output rate." */
uint32_t audio_pool_object_sample_rate(const AudioPool *p, AudioObjHandle h);

/* Increment an object's refcount (e.g. a new voice references it, or
 * another VM takes a reference). */
AudioPoolResult audio_pool_ref(AudioPool *p, AudioObjHandle h);

/* Decrement an object's refcount; frees the object (reclaims its
 * blocks, bumps the slot generation) when the count reaches zero.
 * Returns OK whether or not it was the last reference; check
 * *freed if you care. */
AudioPoolResult audio_pool_unref(AudioPool *p, AudioObjHandle h, bool *freed);

/* Drop all references held by a dying VM: for every object whose
 * owner_vm matches, decrement once (the creation reference). Objects
 * still referenced by other VMs / playing voices survive. Returns
 * the number of objects whose final reference was dropped (i.e.
 * freed) via *out_freed (may be NULL). */
AudioPoolResult audio_pool_sweep_vm(AudioPool *p, uint16_t owner_vm,
                                    uint32_t *out_freed);

/* ---- data access (the staging-DMA read path) ---- */

/* Copy `n` bytes starting at byte `offset` within object `h` into
 * `dst`, walking the block chain (handles non-contiguity). Sets
 * *out_copied to the number of bytes actually copied (clamped to the
 * object size). This is what the pool-as-stream_fn adapter (step 4)
 * will call to feed the mixer. Returns OK or BAD_HANDLE. */
AudioPoolResult audio_pool_read(const AudioPool *p, AudioObjHandle h,
                                uint32_t offset, void *dst, uint32_t n,
                                uint32_t *out_copied);

/* Copy `n` bytes from `src` into object `h` at byte `offset` (used
 * when loading sample data into a freshly allocated object). Won't
 * grow the object; writes past the object size are clamped. Sets
 * *out_written. Returns OK or BAD_HANDLE. */
AudioPoolResult audio_pool_write(AudioPool *p, AudioObjHandle h,
                                 uint32_t offset, const void *src, uint32_t n,
                                 uint32_t *out_written);

/* ---- introspection (diagnostics / tests) ---- */

uint32_t audio_pool_free_blocks(const AudioPool *p);
uint32_t audio_pool_block_count(const AudioPool *p);
int32_t  audio_pool_refcount(const AudioPool *p, AudioObjHandle h); /* -1 if bad */
bool     audio_pool_handle_valid(const AudioPool *p, AudioObjHandle h);
uint32_t audio_pool_object_size(const AudioPool *p, AudioObjHandle h); /* 0 if bad */

#endif /* AUDIO_POOL_H */
