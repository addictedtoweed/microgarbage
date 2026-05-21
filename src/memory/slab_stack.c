/* ============================================================
 *  slab_stack.c — implementation
 *  See memory/slab_stack.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "memory/slab_stack.h"
#include <string.h>

/* ============================================================
 *  Magic values
 * ============================================================ */

#define SLAB_MAGIC_ALLOCATED  0x4D4D4D4Du   /* "MMMM" — allocated block */
#define SLAB_MAGIC_FREED      0xDEADBEEFu   /* freed block */

/* ============================================================
 *  Block header layout
 *
 *  Without SLAB_NO_MAGIC:
 *    [ uint32_t magic ][ uint32_t bin_index ][ payload... ]
 *
 *  With SLAB_NO_MAGIC:
 *    [ uint32_t bin_index ][ uint32_t padding ][ payload... ]
 *
 *  Either way the header is 8 bytes, so the payload pointer is
 *  8-byte aligned (matches bump's default alignment).
 * ============================================================ */

typedef struct {
#ifndef SLAB_NO_MAGIC
    uint32_t magic;
#endif
    uint32_t bin_index;
#ifdef SLAB_NO_MAGIC
    uint32_t _pad;
#endif
} BlockHeader;

/* Convert user pointer ↔ block header. */
static inline BlockHeader *header_of(void *user_ptr) {
    return (BlockHeader *)((uint8_t *)user_ptr - SLAB_HEADER_SIZE);
}

static inline void *payload_of(BlockHeader *h) {
    return (uint8_t *)h + SLAB_HEADER_SIZE;
}

/* ============================================================
 *  Null locker
 * ============================================================ */

static uintptr_t null_lock(void *ctx)   { (void)ctx; return 0; }
static void      null_unlock(void *ctx, uintptr_t s) { (void)ctx; (void)s; }

const SlabLocker slab_null_locker = { null_lock, null_unlock, NULL };

/* ============================================================
 *  size → bin index
 *
 *  Returns the smallest bin index whose block_size >= requested.
 *  Returns SLAB_BIN_COUNT (an invalid value) if the request is
 *  larger than the biggest bin.
 *
 *  Bin sizes: 32 << N for N in 0..15
 *  Inverse:   bin_index = ceil(log2(size / 32))
 *           = ceil(log2(size)) - 5
 *
 *  We compute this using __builtin_clz when available, with a
 *  software fallback for portability.
 * ============================================================ */

#if defined(__GNUC__) || defined(__clang__)
#  define HAS_BUILTIN_CLZ 1
#endif

#ifndef HAS_BUILTIN_CLZ
static int sw_clz32(uint32_t x) {
    /* Software fallback: returns leading-zero count of a 32-bit
     * unsigned integer. Undefined for x == 0; caller must check. */
    int n = 0;
    if ((x & 0xFFFF0000u) == 0) { n += 16; x <<= 16; }
    if ((x & 0xFF000000u) == 0) { n +=  8; x <<=  8; }
    if ((x & 0xF0000000u) == 0) { n +=  4; x <<=  4; }
    if ((x & 0xC0000000u) == 0) { n +=  2; x <<=  2; }
    if ((x & 0x80000000u) == 0) { n +=  1; }
    return n;
}
#  define CLZ32(x) sw_clz32(x)
#else
#  define CLZ32(x) ((int)__builtin_clz((unsigned int)(x)))
#endif

/* Given a payload size in bytes, return the bin index that holds it,
 * or SLAB_BIN_COUNT if too large.
 *
 * We need a block that's at least (size + SLAB_HEADER_SIZE) bytes.
 * Bin sizes are 32 << N, so we want the smallest N with
 * (32 << N) >= total_bytes.
 *
 * For total_bytes <= 32: bin 0.
 * For total_bytes in 33..64: bin 1.
 * etc.
 */
static int size_to_bin(size_t payload_size) {
    /* Add header overhead. */
    size_t total = payload_size + SLAB_HEADER_SIZE;
    if (total <= SLAB_MIN_BLOCK) return 0;

    /* Find ceil(log2(total)). bit_length-1 if total is a power of 2,
     * else bit_length. Position of highest set bit of (total-1) + 1
     * gives ceil(log2(total)).
     *
     * On a 32-bit value, position-of-highest-set-bit = 31 - clz.
     * So ceil_log2 = 31 - clz(total - 1) + 1 = 32 - clz(total - 1). */
    uint32_t v = (uint32_t)(total - 1);
    int ceil_log2 = 32 - CLZ32(v);

    /* Bin N has size (32 << N) = 2^(5+N). So bin = ceil_log2 - 5. */
    int bin = ceil_log2 - 5;
    if (bin < 0) bin = 0;
    if (bin >= SLAB_BIN_COUNT) return SLAB_BIN_COUNT;
    return bin;
}

/* ============================================================
 *  slab_required_bytes
 *
 *  Layout:
 *    Per-bin pointer-stack arrays come first (caller-supplied
 *    region, after which the SlabAllocator struct lives separately,
 *    so the region only holds stacks + blocks).
 *
 *    Then bin block storage in increasing bin order. Each bin's
 *    blocks are contiguous: bucket_count[i] blocks of size
 *    (bin_size[i] + SLAB_HEADER_SIZE) each.
 *
 *  Required bytes = sum over all bins of:
 *      (stack: bucket_count * sizeof(void*))  +
 *      (blocks: bucket_count * (bin_size + SLAB_HEADER_SIZE))
 *
 *  Plus a small alignment buffer (we align bin blocks to 8 bytes).
 * ============================================================ */

size_t slab_required_bytes(const SlabConfig *cfg) {
    if (!cfg) return 0;
    size_t total = 0;
    for (int i = 0; i < SLAB_BIN_COUNT; i++) {
        uint16_t n = cfg->bucket_counts[i];
        if (n == 0) continue;
        size_t bin_block_size = (size_t)SLAB_MIN_BLOCK << i;
        total += (size_t)n * sizeof(void *);                    /* stack */
        total += (size_t)n * (bin_block_size + SLAB_HEADER_SIZE); /* blocks */
    }
    /* Add headroom for alignment fixups (worst case 8 bytes). */
    total += 8;
    return total;
}

/* Align a pointer up to the next 8-byte boundary. */
static uint8_t *align8(uint8_t *p) {
    uintptr_t v = (uintptr_t)p;
    v = (v + 7) & ~(uintptr_t)7;
    return (uint8_t *)v;
}

/* ============================================================
 *  slab_init
 *
 *  Partition the region:
 *    1. All bin stack arrays (pointer-sized slots), packed.
 *    2. All bin block storage, each bin block aligned to 8.
 *
 *  Initialize each bin's stack so all its blocks are pushed —
 *  the allocator starts with everything free.
 * ============================================================ */

SlabResult slab_init(SlabAllocator *a,
                     void *region, size_t bytes,
                     const SlabConfig *cfg,
                     SlabLocker locker) {
    if (!a || !region || !cfg) return SLAB_ERR_INVALID_ARG;
    if (bytes < slab_required_bytes(cfg)) return SLAB_ERR_NO_SPACE;

    /* Zero the entire allocator struct so any leftover state from
     * a prior use is cleared. */
    memset(a, 0, sizeof(*a));
    a->region       = (uint8_t *)region;
    a->region_bytes = bytes;
    a->locker       = locker;
    a->bitmap       = 0;

    /* Set bin block_size and bucket_count for ALL bins (even
     * unused ones — block_size is purely informational and helps
     * users inspect the struct). */
    for (int i = 0; i < SLAB_BIN_COUNT; i++) {
        a->bins[i].block_size    = (size_t)SLAB_MIN_BLOCK << i;
        a->bins[i].bucket_count  = cfg->bucket_counts[i];
        a->bins[i].blocks_in_use = 0;
        a->bins[i].peak_in_use   = 0;
        a->bins[i]._internal_top = 0;
        a->bins[i]._internal_stack = NULL;
        a->bins[i]._internal_blocks = NULL;
    }

    /* Carve out stack arrays first. */
    uint8_t *cursor = (uint8_t *)region;
    for (int i = 0; i < SLAB_BIN_COUNT; i++) {
        if (a->bins[i].bucket_count == 0) continue;
        a->bins[i]._internal_stack = (void **)cursor;
        cursor += (size_t)a->bins[i].bucket_count * sizeof(void *);
    }

    /* Carve out block storage for each bin, aligning to 8. */
    for (int i = 0; i < SLAB_BIN_COUNT; i++) {
        uint16_t n = a->bins[i].bucket_count;
        if (n == 0) continue;

        cursor = align8(cursor);
        a->bins[i]._internal_blocks = cursor;

        size_t bin_block_total = a->bins[i].block_size + SLAB_HEADER_SIZE;

        /* Push every block onto the free stack and initialize its
         * header to point at the bin. */
        for (uint16_t b = 0; b < n; b++) {
            uint8_t *block_start = cursor + (size_t)b * bin_block_total;
            BlockHeader *h = (BlockHeader *)block_start;
            h->bin_index = (uint32_t)i;
#ifndef SLAB_NO_MAGIC
            h->magic = SLAB_MAGIC_FREED;
#else
            h->_pad = 0;
#endif
            /* Push the USER pointer (past the header) onto the stack. */
            a->bins[i]._internal_stack[b] = payload_of(h);
        }
        a->bins[i]._internal_top = n;        /* stack starts full */
        a->bitmap |= (1u << i);              /* bin has free blocks */
        a->total_bytes_managed += (size_t)n * a->bins[i].block_size;

        cursor += (size_t)n * bin_block_total;
    }

    /* Sanity: we should not have overrun the region. */
    if ((size_t)(cursor - (uint8_t *)region) > bytes) {
        /* This shouldn't happen given the slab_required_bytes
         * check, but be defensive. */
        return SLAB_ERR_NO_SPACE;
    }

    return SLAB_OK;
}

void slab_destroy(SlabAllocator *a) {
    if (!a) return;
    /* Region is caller-owned; we just clear our state to make
     * use-after-destroy fail loudly. */
    memset(a, 0, sizeof(*a));
}

/* ============================================================
 *  slab_alloc
 * ============================================================ */

void *slab_alloc(SlabAllocator *a, size_t n) {
    if (!a || n == 0) return NULL;

    int bin = size_to_bin(n);
    if (bin >= SLAB_BIN_COUNT) {
        /* Too large for any bin. */
        uintptr_t saved = a->locker.lock(a->locker.ctx);
        a->failed_alloc_count++;
        a->locker.unlock(a->locker.ctx, saved);
        return NULL;
    }

    uintptr_t saved = a->locker.lock(a->locker.ctx);

    /* Check bitmap. */
    if (!(a->bitmap & (1u << bin))) {
        /* Target bin is empty. No fallback. */
        a->failed_alloc_count++;
        a->locker.unlock(a->locker.ctx, saved);
        return NULL;
    }

    SlabBinInfo *info = &a->bins[bin];

    /* Pop from the bin's stack. */
    void *user_ptr = info->_internal_stack[--info->_internal_top];

    /* If bin is now empty, clear its bit. */
    if (info->_internal_top == 0) {
        a->bitmap &= ~(1u << bin);
    }

    /* Mark the block as allocated. */
#ifndef SLAB_NO_MAGIC
    BlockHeader *h = header_of(user_ptr);
    h->magic = SLAB_MAGIC_ALLOCATED;
#endif
    /* bin_index was set at init; doesn't change. */

    /* Update stats. */
    info->blocks_in_use++;
    if (info->blocks_in_use > info->peak_in_use) {
        info->peak_in_use = info->blocks_in_use;
    }
    a->total_bytes_in_use += info->block_size;
    if (a->total_bytes_in_use > a->peak_bytes_in_use) {
        a->peak_bytes_in_use = a->total_bytes_in_use;
    }
    a->alloc_count++;

    a->locker.unlock(a->locker.ctx, saved);
    return user_ptr;
}

/* ============================================================
 *  slab_free
 * ============================================================ */

SlabResult slab_free(SlabAllocator *a, void *p) {
    if (!a) return SLAB_ERR_INVALID_ARG;
    if (!p)  return SLAB_OK;   /* NULL is a no-op */

    BlockHeader *h = header_of(p);

    /* Sanity-check the pointer is within the region. We check
     * against the region bounds; we'd need full block-storage
     * boundary checks to be more precise. */
    uint8_t *block_addr = (uint8_t *)h;
    if (block_addr < a->region ||
        block_addr >= a->region + a->region_bytes) {
        return SLAB_ERR_FOREIGN_POINTER;
    }

#ifndef SLAB_NO_MAGIC
    /* Magic-check before taking the lock — these are read-only
     * checks of the header and don't touch shared state. */
    if (h->magic == SLAB_MAGIC_FREED) {
        return SLAB_ERR_DOUBLE_FREE;
    }
    if (h->magic != SLAB_MAGIC_ALLOCATED) {
        return SLAB_ERR_CORRUPTED;
    }
#endif

    uint32_t bin = h->bin_index;
    if (bin >= SLAB_BIN_COUNT) {
        /* Header says it's in a bin that doesn't exist. */
        return SLAB_ERR_CORRUPTED;
    }

    uintptr_t saved = a->locker.lock(a->locker.ctx);

    SlabBinInfo *info = &a->bins[bin];

    /* Push onto bin's free stack. Should always have room — the
     * stack is sized to hold all blocks. */
    if (info->_internal_top >= info->bucket_count) {
        /* This means too many frees have occurred for this bin —
         * a sign of double-free that snuck past the magic check.
         * Refuse rather than overflow the stack. */
        a->locker.unlock(a->locker.ctx, saved);
        return SLAB_ERR_DOUBLE_FREE;
    }
    info->_internal_stack[info->_internal_top++] = p;

    /* Bin definitely has free blocks now; set the bit. */
    a->bitmap |= (1u << bin);

    /* Mark the block as freed. */
#ifndef SLAB_NO_MAGIC
    h->magic = SLAB_MAGIC_FREED;
#endif

    /* Update stats. */
    if (info->blocks_in_use > 0) info->blocks_in_use--;
    if (a->total_bytes_in_use >= info->block_size) {
        a->total_bytes_in_use -= info->block_size;
    } else {
        a->total_bytes_in_use = 0;
    }
    a->free_count++;

    a->locker.unlock(a->locker.ctx, saved);
    return SLAB_OK;
}

/* ============================================================
 *  slab_block_size — introspect an allocation
 *
 *  Returns the bin's block size (the "rounded up" allocation
 *  size, which is the usable payload size) for `p`, or 0 if
 *  `p` isn't a live block from this allocator. Used by the
 *  guest-visible SYS_ALLOC_SIZE syscall to support realloc()
 *  with correct copy bounds.
 * ============================================================ */

size_t slab_block_size(const SlabAllocator *a, const void *p) {
    if (!a || !p) return 0;

    BlockHeader *h = header_of((void *)p);

    /* Region-bounds check. */
    const uint8_t *block_addr = (const uint8_t *)h;
    if (block_addr < a->region ||
        block_addr >= a->region + a->region_bytes) {
        return 0;
    }

#ifndef SLAB_NO_MAGIC
    /* Only ALLOCATED blocks have valid bin_index. A freed block
     * has the magic set but its bin_index is undefined (we never
     * clear it but we also can't trust it). */
    if (h->magic != SLAB_MAGIC_ALLOCATED) {
        return 0;
    }
#endif

    uint32_t bin = h->bin_index;
    if (bin >= SLAB_BIN_COUNT) return 0;
    return a->bins[bin].block_size;
}

