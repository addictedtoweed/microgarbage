/* ============================================================
 *  slab_stack.h — two-layer slab allocator
 *
 *  A fixed-bin, fixed-capacity allocator for embedded systems
 *  where predictable allocation behavior matters more than
 *  squeezing every byte. Inspired by TLSF and SLAB but simpler.
 *
 *  ---------------------------------------------------------------
 *  Architecture
 *  ---------------------------------------------------------------
 *
 *  The allocator carves a caller-provided region into bins. Each
 *  bin holds a fixed number of fixed-size blocks. Allocations
 *  round up to the nearest bin size; the block comes from that
 *  bin's pool.
 *
 *  Bins are sized as powers of 2:
 *    bin 0:  32 B
 *    bin 1:  64 B
 *    bin 2:  128 B
 *    ...
 *    bin 8:  8 KB
 *    bin 9:  16 KB
 *    ...
 *    bin 15: 1 MB
 *
 *  Free blocks per bin are tracked by a stack of pointers (push
 *  on free, pop on alloc — O(1) both ways). A 32-bit bitmap
 *  indicates which bins have free blocks for fast empty-check.
 *
 *  No fallback to larger bins: if your requested size's bin is
 *  empty, the allocation fails. This is predictable: the user's
 *  config promises N blocks of size X, and that's what they get.
 *  No splitting, no coalescing, no fragmentation surprises.
 *
 *  ---------------------------------------------------------------
 *  Memory layout in the caller-provided region
 *  ---------------------------------------------------------------
 *
 *    [ Allocator header / state ]
 *    [ Per-bin pointer-stack arrays (one per non-empty bin) ]
 *    [ Block storage: bins layed out in increasing bin order ]
 *
 *  All state lives in the region. The SlabAllocator handle is a
 *  pointer to the start of that region (cast appropriately).
 *
 *  ---------------------------------------------------------------
 *  Per-block header
 *  ---------------------------------------------------------------
 *
 *  Each block carries an 8-byte header:
 *    - 4 bytes magic (0x4D4D4D4D = allocated, 0xDEADBEEF = freed)
 *    - 4 bytes bin_index (so free() can return the block correctly)
 *
 *  The magic catches:
 *    - Freeing a pointer not from this allocator
 *    - Double-free (second free sees the freed-magic)
 *    - Header corruption from buffer underflow
 *
 *  Define SLAB_NO_MAGIC at compile time to strip the magic for
 *  release builds. Saves 4 bytes per block and one comparison per
 *  free.
 *
 *  ---------------------------------------------------------------
 *  Locking
 *  ---------------------------------------------------------------
 *
 *  The allocator calls user-supplied lock/unlock callbacks around
 *  its internal critical sections. The user implements these as
 *  appropriate for their context:
 *
 *    - Single-threaded with no ISR concerns: no-op locker
 *    - Bare-metal with ISRs: __disable_irq / __enable_irq pair
 *    - RTOS thread context: mutex or critical section
 *    - Bare-metal + RTOS (mixed): interrupt-disable handles both
 *
 *  Lock returns a saved-state value (typically PRIMASK); unlock
 *  takes it back. This supports nested locking correctly.
 *
 *  ---------------------------------------------------------------
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef SLAB_STACK_H
#define SLAB_STACK_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Number of bins. Bin N has size SLAB_MIN_BLOCK << N.
 *   SLAB_BIN_COUNT = 16 → sizes 32B to 1MB.
 * The bitmap is uint32_t, so up to 32 bins are supported by the
 * type; we use 16 to keep max size at 1MB which is appropriate
 * for embedded use. Top 16 bits of bitmap are reserved. */
#define SLAB_BIN_COUNT  16
#define SLAB_MIN_BLOCK  32

/* Header size per block in bytes. Includes magic (4) + bin index
 * (4) when SLAB_NO_MAGIC is not defined; just bin index (4) when
 * stripped. The block layout aligns the user pointer to 8 bytes
 * which fits the bump allocator's default alignment. */
#ifdef SLAB_NO_MAGIC
#define SLAB_HEADER_SIZE  8   /* 4B bin_index + 4B padding for alignment */
#else
#define SLAB_HEADER_SIZE  8   /* 4B magic + 4B bin_index */
#endif

/* ============================================================
 *  Result codes
 * ============================================================ */

typedef enum {
    SLAB_OK = 0,
    SLAB_ERR_INVALID_ARG,
    SLAB_ERR_NO_SPACE,            /* region too small for the config */
    SLAB_ERR_BIN_EXHAUSTED,       /* alloc: target bin has no free blocks */
    SLAB_ERR_SIZE_TOO_LARGE,      /* alloc: size exceeds biggest bin */
    SLAB_ERR_CORRUPTED,           /* free: magic check failed */
    SLAB_ERR_DOUBLE_FREE,         /* free: block already marked freed */
    SLAB_ERR_FOREIGN_POINTER,     /* free: pointer not from this allocator */
} SlabResult;

/* ============================================================
 *  Configuration
 *
 *  bucket_counts[N] is the number of blocks of size (32 << N) to
 *  allocate. Set to 0 for bins you don't want.
 *
 *  Required region size (computed for you by slab_required_bytes):
 *    header + sum_of_bin_stacks + sum_of_bin_blocks
 *  where:
 *    bin_stack_bytes[i] = bucket_counts[i] * sizeof(void*)
 *    bin_blocks_bytes[i] = bucket_counts[i] * ((32 << i) + SLAB_HEADER_SIZE)
 * ============================================================ */

typedef struct {
    uint16_t bucket_counts[SLAB_BIN_COUNT];
} SlabConfig;

/* ============================================================
 *  Locker (caller-supplied)
 *
 *  The lock function returns a uintptr_t holding whatever state
 *  the caller wants to save (PRIMASK, mutex handle, etc). The
 *  unlock function receives that state back to restore.
 *
 *  Pass &slab_null_locker for no locking (single-threaded use).
 * ============================================================ */

typedef struct {
    uintptr_t (*lock)(void *ctx);
    void      (*unlock)(void *ctx, uintptr_t saved);
    void     *ctx;
} SlabLocker;

/* No-op locker for single-threaded use. */
extern const SlabLocker slab_null_locker;

/* ============================================================
 *  Per-bin live stats (read-only public fields)
 * ============================================================ */

typedef struct {
    size_t   block_size;       /* set at init: 32 << bin_index */
    uint16_t bucket_count;     /* set at init: from config */
    uint16_t blocks_in_use;    /* live: currently allocated count */
    uint16_t peak_in_use;      /* live: max ever simultaneously allocated */
    uint16_t _internal_top;    /* internal: stack top index */
    void   **_internal_stack;  /* internal: pointer to stack array */
    uint8_t *_internal_blocks; /* internal: start of this bin's block storage */
} SlabBinInfo;

/* ============================================================
 *  Allocator handle
 *
 *  Public fields are live stats (read-only).
 *  Internal fields are managed by the implementation.
 * ============================================================ */

typedef struct SlabAllocator {
    /* === Public read-only stats === */
    size_t   total_bytes_managed;  /* sum of bin_size × bucket_count */
    size_t   total_bytes_in_use;   /* current consumption */
    size_t   peak_bytes_in_use;    /* high-water mark */
    uint32_t alloc_count;          /* cumulative successful allocs */
    uint32_t free_count;           /* cumulative frees */
    uint32_t failed_alloc_count;   /* cumulative NULLs returned */
    SlabBinInfo bins[SLAB_BIN_COUNT];

    /* === Internal — don't read or write === */
    uint32_t bitmap;          /* bit N set: bin N has at least one free block */
    uint8_t *region;          /* base of the caller-provided region */
    size_t   region_bytes;
    SlabLocker locker;
} SlabAllocator;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Compute the region size needed for the given config. Useful for
 * sizing a static buffer at compile time (via a function that
 * computes the value, then declaring a buffer based on it). */
size_t slab_required_bytes(const SlabConfig *cfg);

/* Compute the bin index that an allocation of `bytes` would land
 * in. Bins are powers of 2 starting at 32 B: bin i has block
 * size (32 << i). Sizes larger than the biggest bin return the
 * last bin index — caller should check size against the bin's
 * block_size before relying on the result.
 *
 * Accounts for the per-block header overhead (SLAB_HEADER_SIZE
 * bytes) so a caller asking "where would a 64 KB block land?"
 * gets the right answer (the 128 KB bin, since 64 KB + 8 header
 * exceeds the 64 KB bin's capacity).
 *
 * Useful when laying out a SlabConfig: "I want N blocks of about
 * 64 KB each → bucket_counts[slab_bin_for_size(64*1024)] = N". */
static inline int slab_bin_for_size(size_t bytes) {
    size_t total = bytes + SLAB_HEADER_SIZE;
    int bin = 0;
    size_t sz = 32;
    while (sz < total && bin < SLAB_BIN_COUNT - 1) {
        sz <<= 1;
        bin++;
    }
    return bin;
}

/* Initialize a slab allocator in the caller-provided region.
 *
 *   a:        allocator handle (caller-owned)
 *   region:   pointer to backing memory (must remain valid for life)
 *   bytes:    size of region in bytes; must be >= slab_required_bytes(cfg)
 *   cfg:      bucket configuration (copied internally)
 *   locker:   lock callbacks; pass &slab_null_locker for no locking
 *
 * Returns SLAB_OK on success. The region is partitioned at init;
 * its prior contents are overwritten with allocator state and
 * empty headers. */
SlabResult slab_init(SlabAllocator *a,
                     void *region, size_t bytes,
                     const SlabConfig *cfg,
                     SlabLocker locker);

/* Tear down. The caller's region is NOT freed — they still own it.
 * Safe to call on a zero-initialized struct. */
void slab_destroy(SlabAllocator *a);

/* ============================================================
 *  Allocation
 * ============================================================ */

/* Allocate at least n bytes. Returns NULL if:
 *   - n exceeds the biggest bin size (1 MB)
 *   - the bin for size n is exhausted
 *   - a is invalid
 * On failure, a->failed_alloc_count is incremented. */
void *slab_alloc(SlabAllocator *a, size_t n);

/* Free a pointer previously returned by slab_alloc. NULL is a
 * no-op. Detects double-free and pointer-from-other-allocator
 * via the header magic (unless SLAB_NO_MAGIC was defined). */
SlabResult slab_free(SlabAllocator *a, void *p);

/* Return the block size (the usable payload size, which is the
 * bin's block_size — slab allocations are bucket-rounded) for
 * a pointer previously returned by slab_alloc. Returns 0 if
 * `p` is NULL, isn't from this allocator, or has been freed.
 *
 * Used by the SYS_ALLOC_SIZE syscall so guest realloc() can
 * copy the right number of bytes from old to new. */
size_t slab_block_size(const SlabAllocator *a, const void *p);

/* Resize an allocation (mirrors C realloc, scoped to this allocator).
 *   - p == NULL        -> slab_alloc(new_size)
 *   - new_size == 0    -> slab_free(p), returns NULL
 *   - fits current bin -> returns p unchanged, no copy (shrink or
 *                         grow-within-bin both stay in place)
 *   - needs bigger bin -> alloc new, copy old payload, free old
 *
 * On grow-allocation failure the ORIGINAL block is left intact and
 * NULL is returned (the C realloc contract). The internal memcpy runs
 * with no lock held (the two O(1) alloc/free each lock; the O(size)
 * copy does not) — so on the micro interrupts are disabled only for
 * the bounded ops, not the copy.
 *
 * NON-REAL-TIME convenience (string building, content assembly): it
 * moves pointers and is O(size) on growth. Do not use on real-time
 * paths or for memory whose pointer is held/shared elsewhere. */
void *slab_realloc(SlabAllocator *a, void *p, size_t new_size);

/* ============================================================
 *  Introspection
 *
 *  All live stats are public fields on the allocator struct.
 *  Read directly:
 *
 *    a->total_bytes_in_use
 *    a->peak_bytes_in_use
 *    a->bins[3].blocks_in_use
 *    a->bins[3].peak_in_use
 *
 *  These accessors exist for explicitness and for read-side
 *  consistency in concurrent code (if needed, take the locker
 *  during a snapshot). */

static inline size_t slab_bytes_used(const SlabAllocator *a) {
    return a ? a->total_bytes_in_use : 0;
}

static inline size_t slab_bytes_free(const SlabAllocator *a) {
    return a ? (a->total_bytes_managed - a->total_bytes_in_use) : 0;
}

static inline size_t slab_bytes_peak(const SlabAllocator *a) {
    return a ? a->peak_bytes_in_use : 0;
}

#endif /* SLAB_STACK_H */
