/* ============================================================
 *  mg_l2.h — guest-side L2 allocator API.
 *
 *  alloc_l2 returns a real guest pointer the VM translates into the
 *  system-wide PSRAM backing. Dereference it like any other guest
 *  pointer — *p = x just works. The pointer lives in the upper half
 *  of the SHARED region (0xE000_0000+); the VM core's translation
 *  picks up the right backing automatically.
 *
 *  L2 is intended for bulk asset storage where access latency is
 *  acceptable (textures waiting to be DMA'd, level data, decoded
 *  audio buffers staging for the mixer). Tight inner loops should
 *  copy into L1 (regular malloc'd memory) for speed.
 *
 *  All allocations come from one global pool — every VM sees the
 *  same memory at the same VA — so the same pointer is valid in
 *  any process that's still alive. Use cooperatively.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef GUEST_MG_L2_H
#define GUEST_MG_L2_H

#include "vm_runtime.h"
#include <stdint.h>
#include <stddef.h>

/* Allocate `size` bytes from the L2 pool with `align` byte
 * alignment (must be a power of two; 0 selects 16-byte default).
 * Returns NULL on OOM. The returned pointer is in the 0xE000_0000
 * VA range, dereferenceable like any other guest pointer. */
static inline void *alloc_l2(uint32_t size, uint32_t align) {
    uint32_t va = _vm_sys2(SYS_L2_ALLOC, size, align);
    return (void *)(uintptr_t)va;
}

/* Free a pointer previously returned by alloc_l2. NULL is a no-op. */
static inline void free_l2(void *p) {
    (void)_vm_sys1(SYS_L2_FREE, (uint32_t)(uintptr_t)p);
}

/* Snapshot of the L2 pool's state. The host writes this struct into
 * the guest's buffer; layout is versioned (the host's version 1
 * matches this declaration). Future versions may append fields. */
typedef struct {
    uint32_t version;          /* = 1                                   */
    uint32_t reserved;
    uint64_t total_bytes;
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t largest_free_bytes;
    uint32_t alloc_count;
    uint32_t free_block_count;
} mg_l2_stats;

/* Fill `out` with the current pool stats. Returns bytes written
 * (== sizeof *out on success) or a negative errno. */
static inline int mg_l2_get_stats(mg_l2_stats *out) {
    return (int)_vm_sys1(SYS_L2_STATS, (uint32_t)(uintptr_t)out);
}

#endif /* GUEST_MG_L2_H */
