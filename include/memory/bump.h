/* ============================================================
 *  bump.h — bump allocator
 *
 *  A bump allocator is the simplest possible allocator: a region
 *  with a pointer that advances on each allocation. Allocations
 *  are essentially free (pointer arithmetic + alignment math).
 *  You cannot free individual allocations — only reset the whole
 *  arena at once, which invalidates ALL pointers issued from it.
 *
 *  This is the right tool for "allocate a bunch of related things,
 *  use them, throw them all away" workloads:
 *    - Per-frame allocations in a game loop
 *    - Per-request state in a server
 *    - Per-script-load in hot-load workflows
 *    - Temporary buffers during initialization
 *
 *  The arena's memory can come from:
 *    1. A caller-provided buffer (stack, static, or any block of
 *       memory the caller owns) via bump_init.
 *    2. A slab_stack allocator instance via bump_init_from_slab.
 *       The bump arena requests one block from the slab allocator
 *       at create time and returns it on destroy.
 *
 *  No per-allocation header. No magic number. Allocations are
 *  raw pointers with no metadata. The tradeoff is: no detection
 *  of double-free, use-after-reset, or buffer corruption. If you
 *  need that safety, use slab_stack directly instead.
 *
 *  ---------------------------------------------------------------
 *  Thread safety
 *  ---------------------------------------------------------------
 *
 *  Bump allocators are NOT thread-safe. bump_alloc performs a
 *  read-modify-write on the offset field; concurrent calls will
 *  race and may return overlapping pointers.
 *
 *  This is intentional. Bump allocators trade safety for speed;
 *  the typical pattern is "one arena per thread" or "arena lives
 *  within a bounded scope where only one thread touches it":
 *
 *    - Project init phase: build long-lived state in an arena
 *      before any threads start. The arena is single-threaded by
 *      virtue of being used during init only.
 *
 *    - Per-thread scratch: each thread gets its own arena (often
 *      with thread-local storage). No sharing means no race.
 *
 *    - Per-task / per-request scratch: arena lifetime matches a
 *      single-threaded scope (a request handler, a frame, a
 *      processing batch). Reset or destroy at scope exit.
 *
 *  If you genuinely need a fast allocator usable from multiple
 *  threads simultaneously, use slab_stack instead — it has a
 *  locker callback designed for this. Wrapping bump_alloc in an
 *  external mutex works but mostly defeats the point of using a
 *  bump allocator.
 *
 *  Calls to bump_init, bump_destroy, and bump_reset are also not
 *  thread-safe. Lifecycle operations should happen on a single
 *  thread or be externally synchronized.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef BUMP_H
#define BUMP_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Forward declaration — bump can be backed by slab_stack but
 * doesn't require it. Including slab_stack.h is optional. */
struct SlabAllocator;
typedef struct SlabAllocator SlabAllocator;

/* ============================================================
 *  Result codes
 * ============================================================ */

typedef enum {
    BUMP_OK = 0,
    BUMP_ERR_INVALID_ARG,
    BUMP_ERR_NO_SPACE,
} BumpResult;

/* ============================================================
 *  BumpAllocator
 *
 *  Caller declares directly (no allocator-of-allocator pattern;
 *  the struct is just a few pointers and counts).
 * ============================================================ */

typedef struct {
    uint8_t        *region;       /* base of the arena                */
    size_t          region_bytes; /* total size                       */
    size_t          offset;       /* current bump position            */
    size_t          peak_offset;  /* high-water mark since last reset */
    /* Only used when the arena was sourced from a slab allocator;
     * NULL for caller-provided regions. */
    SlabAllocator  *parent_slab;
} BumpAllocator;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Initialize with a caller-provided region.
 *
 *   b:      bump handle (caller-owned, will be filled in)
 *   region: pointer to caller-owned memory (must outlive b)
 *   bytes:  size of the region; must be > 0
 *
 * The region's contents are NOT touched. On destroy, the region
 * is NOT freed — caller still owns it. */
BumpResult bump_init(BumpAllocator *b, void *region, size_t bytes);

/* Initialize by requesting a region from a slab_stack allocator.
 *
 *   b:      bump handle (caller-owned, will be filled in)
 *   slab:   parent slab allocator
 *   bytes:  desired size; slab will round up to nearest bin
 *
 * On destroy, the region is returned to the slab allocator.
 * Returns BUMP_ERR_NO_SPACE if the slab can't satisfy the request. */
BumpResult bump_init_from_slab(BumpAllocator *b,
                                SlabAllocator *slab,
                                size_t bytes);

/* Destroy the arena. For slab-backed arenas, the region is freed
 * back to the parent allocator. For caller-provided regions, the
 * region is left alone. Safe to call with NULL. */
void bump_destroy(BumpAllocator *b);

/* Reset the bump position to 0. ALL previously-issued pointers
 * are invalidated by this — they still point into the region but
 * may be overwritten by subsequent allocations.
 *
 * The peak_offset field is reset too. If you want to track
 * lifetime peak, read it before reset. */
void bump_reset(BumpAllocator *b);

/* ============================================================
 *  Allocation
 *
 *  bump_alloc returns memory aligned to BUMP_DEFAULT_ALIGNMENT
 *  (8 bytes — suitable for most C struct alignment).
 *
 *  bump_alloc_aligned lets you specify alignment. The alignment
 *  MUST be a power of 2 (1, 2, 4, 8, 16, 32, 64, ...). Other
 *  values are rejected with NULL. This matches C's own alignment
 *  contract — every native type and struct alignment is a power
 *  of 2, and no realistic hardware requires non-power-of-2
 *  alignment.
 *
 *  Returns NULL if:
 *    - the request can't fit in remaining space
 *    - alignment isn't a power of 2
 *    - the allocator handle is invalid
 *    - n is 0
 * ============================================================ */

#define BUMP_DEFAULT_ALIGNMENT 8

void *bump_alloc(BumpAllocator *b, size_t n);

void *bump_alloc_aligned(BumpAllocator *b, size_t n, size_t alignment);

/* ============================================================
 *  Introspection (direct field access, no functions)
 *
 *  Users can read b->offset, b->peak_offset, b->region_bytes
 *  directly. These accessors exist for explicitness. */

static inline size_t bump_used(const BumpAllocator *b) {
    return b ? b->offset : 0;
}

static inline size_t bump_remaining(const BumpAllocator *b) {
    return b ? b->region_bytes - b->offset : 0;
}

static inline size_t bump_capacity(const BumpAllocator *b) {
    return b ? b->region_bytes : 0;
}

static inline size_t bump_peak(const BumpAllocator *b) {
    return b ? b->peak_offset : 0;
}

#endif /* BUMP_H */
