/* ============================================================
 *  l2_alloc.h — first-fit, coalescing allocator over a byte region.
 *
 *  Built for the L2 PSRAM slice but the implementation is region-
 *  agnostic — give it a (region, size) and you can run as many
 *  instances as you want. Stage 2d uses one instance over the
 *  whole 3 MB slice; stage 3 carves the slice into per-VM regions
 *  and instantiates one allocator per VM, so each guest's L2
 *  allocations are isolated from every other guest's.
 *
 *  Design choices:
 *
 *    * First-fit + LIFO free-list. Game allocation profiles are
 *      typically tens of large objects with long-ish lifetimes; the
 *      worst-case O(n) scan cost is negligible. Best-fit would buy
 *      a marginal anti-fragmentation win at the cost of more code;
 *      not worth it here.
 *
 *    * Eager coalesce on free. Both neighbors are checked via the
 *      prev_size / size headers; merging is constant-time.
 *
 *    * 16-byte minimum alignment. Big enough to keep header
 *      arithmetic simple; matches malloc on 64-bit Windows and is
 *      generous on the MCU's 32-bit ARM where 8-byte would do.
 *
 *  Not thread-safe — the cooperative VM and the embedder both touch
 *  this from the same thread.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_L2_ALLOC_H
#define MGAPI_L2_ALLOC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct L2Alloc L2Alloc;

/* Wrap an instance over (region, region_size). The returned handle's
 * storage is carved out of `region` itself — no separate malloc.
 * Returns NULL if region is too small to hold the bookkeeping. */
L2Alloc *l2_create(void *region, size_t region_size);

/* Tear down. Leaves `region` in an undefined state (caller owns it). */
void l2_destroy(L2Alloc *a);

/* Alloc `size` bytes with `align` alignment (must be a power of two;
 * 0 selects the default 16-byte align). Returns NULL on OOM. */
void *l2_alloc(L2Alloc *a, size_t size, size_t align);

/* Free a pointer previously returned by l2_alloc on this instance.
 * NULL is a no-op. Double-free is detected best-effort (returns
 * silently) but not guaranteed. */
void l2_free(L2Alloc *a, void *p);

/* Bytes of contiguous free space currently available (largest single
 * allocation that would succeed, approximately — bookkeeping inflates
 * by one header per block). */
size_t l2_largest_free(const L2Alloc *a);

/* Stats — total free, total used, block counts. */
typedef struct {
    size_t   used_bytes;
    size_t   free_bytes;
    uint32_t alloc_count;
    uint32_t free_block_count;
} L2Stats;
void l2_stats(const L2Alloc *a, L2Stats *out);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_L2_ALLOC_H */
