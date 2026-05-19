/* ============================================================
 *  bump.c — implementation
 *  See memory/bump.h for the public contract.
 *
 *  Alignment math: given current offset `off` and required alignment
 *  `align` (a power of 2), the aligned position is:
 *    aligned = (off + align - 1) & ~(align - 1)
 *  This rounds `off` up to the next multiple of `align`.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "memory/bump.h"
#include <string.h>

/* slab_stack integration — only used if the user calls
 * bump_init_from_slab. Forward declarations to avoid a hard
 * dependency. The user must link slab_stack.c if they use the
 * slab-backed bump path; if they only use bump_init with a
 * caller-provided region, slab_stack is never referenced. */
extern void *slab_alloc(SlabAllocator *s, size_t n);
extern void  slab_free(SlabAllocator *s, void *p);

static bool is_power_of_two(size_t x) {
    return x != 0 && (x & (x - 1)) == 0;
}

BumpResult bump_init(BumpAllocator *b, void *region, size_t bytes) {
    if (!b || !region || bytes == 0) return BUMP_ERR_INVALID_ARG;
    b->region       = (uint8_t *)region;
    b->region_bytes = bytes;
    b->offset       = 0;
    b->peak_offset  = 0;
    b->parent_slab  = NULL;
    return BUMP_OK;
}

BumpResult bump_init_from_slab(BumpAllocator *b,
                                SlabAllocator *slab,
                                size_t bytes) {
    if (!b || !slab || bytes == 0) return BUMP_ERR_INVALID_ARG;

    void *region = slab_alloc(slab, bytes);
    if (!region) return BUMP_ERR_NO_SPACE;

    b->region       = (uint8_t *)region;
    b->region_bytes = bytes;
    b->offset       = 0;
    b->peak_offset  = 0;
    b->parent_slab  = slab;
    return BUMP_OK;
}

void bump_destroy(BumpAllocator *b) {
    if (!b) return;
    if (b->parent_slab && b->region) {
        slab_free(b->parent_slab, b->region);
    }
    /* Zero the fields so reuse-after-destroy fails loudly. */
    b->region       = NULL;
    b->region_bytes = 0;
    b->offset       = 0;
    b->peak_offset  = 0;
    b->parent_slab  = NULL;
}

void bump_reset(BumpAllocator *b) {
    if (!b) return;
    b->offset      = 0;
    b->peak_offset = 0;
}

void *bump_alloc(BumpAllocator *b, size_t n) {
    return bump_alloc_aligned(b, n, BUMP_DEFAULT_ALIGNMENT);
}

void *bump_alloc_aligned(BumpAllocator *b, size_t n, size_t alignment) {
    if (!b || !b->region || n == 0) return NULL;
    if (!is_power_of_two(alignment)) return NULL;

    /* Round current offset up to alignment. */
    size_t mask        = alignment - 1;
    size_t aligned_off = (b->offset + mask) & ~mask;

    /* Check for overflow (aligned_off wrapped) or out-of-space. */
    if (aligned_off < b->offset) return NULL;        /* overflow */
    if (aligned_off > b->region_bytes) return NULL;  /* no room even before n */
    if (n > b->region_bytes - aligned_off) return NULL; /* no room for n */

    void *p = b->region + aligned_off;
    b->offset = aligned_off + n;
    if (b->offset > b->peak_offset) b->peak_offset = b->offset;
    return p;
}
