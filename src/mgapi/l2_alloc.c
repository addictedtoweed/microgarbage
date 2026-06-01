/* ============================================================
 *  l2_alloc.c — first-fit + coalesce. ~200 LOC.
 *
 *  Region layout:
 *
 *    [ L2Alloc header ][ block_0 ][ block_1 ] ... [ block_N ]
 *
 *  Each block has a fixed 16-byte header:
 *
 *    uint32_t size        total bytes including header (always >= 32)
 *    uint32_t prev_size   total bytes of preceding block (0 if first)
 *    uint32_t flags       bit 0 = IN_USE
 *    uint32_t _pad        keeps user payload 16-byte aligned on 64-bit
 *
 *  When free, the first 16 bytes of payload hold {prev, next} free-list
 *  pointers (so free blocks need user_payload >= 16 bytes — implied by
 *  the 32-byte minimum block).
 *
 *  The walker invariant: visiting blocks in linear order via
 *  next_block(b) = (uint8_t*)b + b->size covers the whole region; the
 *  last block's size brings us exactly to region_end.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "l2_alloc.h"

#include <stdint.h>
#include <string.h>

#define L2_HEADER_BYTES     16u
#define L2_MIN_BLOCK_BYTES  32u    /* header + at least 16 payload bytes  */
#define L2_DEFAULT_ALIGN    16u

#define L2_FLAG_IN_USE      0x1u

typedef struct L2Block {
    uint32_t size;
    uint32_t prev_size;
    uint32_t flags;
    uint32_t _pad;
} L2Block;

struct L2Alloc {
    uint8_t  *region_base;
    uint8_t  *region_end;
    L2Block  *free_head;    /* LIFO free-list head (or NULL if no free) */
    uint32_t  alloc_count;
    uint32_t  free_block_count;
};

/* ----------------------------------------------------------------
 *  Block <-> payload pointer arithmetic
 * ---------------------------------------------------------------- */

static inline void *block_payload(L2Block *b) {
    return (uint8_t *)b + L2_HEADER_BYTES;
}
static inline L2Block *payload_block(void *p) {
    return (L2Block *)((uint8_t *)p - L2_HEADER_BYTES);
}
static inline L2Block *next_block(L2Alloc *a, L2Block *b) {
    uint8_t *n = (uint8_t *)b + b->size;
    return (n >= a->region_end) ? NULL : (L2Block *)n;
}
static inline L2Block *prev_block(L2Alloc *a, L2Block *b) {
    (void)a;   /* kept for symmetry with next_block; not needed here */
    if (b->prev_size == 0) return NULL;
    return (L2Block *)((uint8_t *)b - b->prev_size);
}

/* Free-list pointers live in the user payload area when the block is
 * free. We treat the payload as a struct of two pointers. */
typedef struct { L2Block *prev; L2Block *next; } FreeLink;

static inline FreeLink *fl(L2Block *b) {
    return (FreeLink *)block_payload(b);
}

static void fl_push(L2Alloc *a, L2Block *b) {
    FreeLink *bl = fl(b);
    bl->prev = NULL;
    bl->next = a->free_head;
    if (a->free_head) fl(a->free_head)->prev = b;
    a->free_head = b;
    a->free_block_count++;
}

static void fl_remove(L2Alloc *a, L2Block *b) {
    FreeLink *bl = fl(b);
    if (bl->prev) fl(bl->prev)->next = bl->next;
    else          a->free_head     = bl->next;
    if (bl->next) fl(bl->next)->prev = bl->prev;
    a->free_block_count--;
}

/* ----------------------------------------------------------------
 *  Create / destroy
 * ---------------------------------------------------------------- */

L2Alloc *l2_create(void *region, size_t region_size) {
    if (!region || region_size < sizeof(L2Alloc) + L2_MIN_BLOCK_BYTES) {
        return NULL;
    }

    /* Carve the allocator state out of the region's leading bytes.
     * The actual usable heap starts at region + sizeof(L2Alloc),
     * rounded up to L2_DEFAULT_ALIGN. */
    L2Alloc *a = (L2Alloc *)region;
    memset(a, 0, sizeof *a);

    uintptr_t heap_base_addr = (uintptr_t)region + sizeof(L2Alloc);
    heap_base_addr = (heap_base_addr + L2_DEFAULT_ALIGN - 1u) &
                      ~((uintptr_t)L2_DEFAULT_ALIGN - 1u);
    a->region_base = (uint8_t *)heap_base_addr;
    a->region_end  = (uint8_t *)region + region_size;
    if (a->region_end - a->region_base < (ptrdiff_t)L2_MIN_BLOCK_BYTES) {
        return NULL;
    }

    /* One big free block spanning the whole heap. */
    L2Block *b = (L2Block *)a->region_base;
    b->size      = (uint32_t)(a->region_end - a->region_base);
    b->prev_size = 0;
    b->flags     = 0;
    b->_pad      = 0;
    a->free_head = NULL;
    a->free_block_count = 0;
    fl_push(a, b);
    return a;
}

void l2_destroy(L2Alloc *a) {
    if (!a) return;
    /* No external state to free — the allocator lives inside region. */
    memset(a, 0, sizeof *a);
}

/* ----------------------------------------------------------------
 *  Alloc / free
 * ---------------------------------------------------------------- */

static uint32_t round_up_u32(uint32_t v, uint32_t align) {
    return (v + align - 1u) & ~(align - 1u);
}
static int is_pow2(size_t v) { return v != 0 && (v & (v - 1)) == 0; }

void *l2_alloc(L2Alloc *a, size_t size, size_t align) {
    if (!a || size == 0) return NULL;
    if (align == 0) align = L2_DEFAULT_ALIGN;
    if (!is_pow2(align)) return NULL;
    if (align < L2_DEFAULT_ALIGN) align = L2_DEFAULT_ALIGN;

    /* Required total block size: header + max(size, 16) rounded up
     * to alignment. Plus enough slop for the alignment shift inside
     * the payload — keep simple and round up generously. */
    uint32_t want_payload = (size < 16u) ? 16u : (uint32_t)size;
    want_payload = round_up_u32(want_payload, (uint32_t)align);
    uint32_t want_total = round_up_u32(want_payload + L2_HEADER_BYTES,
                                       L2_DEFAULT_ALIGN);

    /* First-fit. */
    L2Block *b = a->free_head;
    while (b && b->size < want_total) {
        b = fl(b)->next;
    }
    if (!b) return NULL;

    fl_remove(a, b);

    /* Split off the tail if there's enough left for another block. */
    uint32_t remaining = b->size - want_total;
    if (remaining >= L2_MIN_BLOCK_BYTES) {
        L2Block *tail = (L2Block *)((uint8_t *)b + want_total);
        tail->size      = remaining;
        tail->prev_size = want_total;
        tail->flags     = 0;
        tail->_pad      = 0;
        /* Update next-after-tail's prev_size if it exists. */
        L2Block *after = next_block(a, tail);
        if (after) after->prev_size = remaining;
        b->size = want_total;
        fl_push(a, tail);
    }

    b->flags |= L2_FLAG_IN_USE;
    a->alloc_count++;
    return block_payload(b);
}

void l2_free(L2Alloc *a, void *p) {
    if (!a || !p) return;
    L2Block *b = payload_block(p);
    if (!(b->flags & L2_FLAG_IN_USE)) return;   /* best-effort double-free guard */
    b->flags &= ~L2_FLAG_IN_USE;
    a->alloc_count--;

    /* Coalesce with previous if free. */
    L2Block *prev = prev_block(a, b);
    if (prev && !(prev->flags & L2_FLAG_IN_USE)) {
        fl_remove(a, prev);
        prev->size += b->size;
        L2Block *after = next_block(a, prev);
        if (after) after->prev_size = prev->size;
        b = prev;
    }

    /* Coalesce with next if free. */
    L2Block *next = next_block(a, b);
    if (next && !(next->flags & L2_FLAG_IN_USE)) {
        fl_remove(a, next);
        b->size += next->size;
        L2Block *after = next_block(a, b);
        if (after) after->prev_size = b->size;
    }

    fl_push(a, b);
}

/* ----------------------------------------------------------------
 *  Stats
 * ---------------------------------------------------------------- */

size_t l2_largest_free(const L2Alloc *a) {
    if (!a) return 0;
    size_t big = 0;
    for (L2Block *b = a->free_head; b; b = fl(b)->next) {
        if (b->size > big) big = b->size;
    }
    return (big > L2_HEADER_BYTES) ? (big - L2_HEADER_BYTES) : 0;
}

void l2_stats(const L2Alloc *a, L2Stats *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!a) return;
    size_t free_bytes = 0;
    for (L2Block *b = a->free_head; b; b = fl(b)->next) {
        free_bytes += b->size;
    }
    size_t total = (size_t)(a->region_end - a->region_base);
    out->used_bytes       = total - free_bytes;
    out->free_bytes       = free_bytes;
    out->alloc_count      = a->alloc_count;
    out->free_block_count = a->free_block_count;
}
