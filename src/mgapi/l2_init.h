/* ============================================================
 *  l2_init.h — bring up the shared L2 allocator.
 *
 *  Stage 2d wraps a single L2Alloc over the whole 3 MB PSRAM L2
 *  slice. Stage 3 will replace this with per-VM L2Alloc instances
 *  carved out of the same slice, but the API surface stays similar
 *  (init at boot, alloc/free during runtime, shutdown at teardown).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_L2_INIT_H
#define MGAPI_L2_INIT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  mgapi_l2_init(void *region, size_t region_size);
void mgapi_l2_shutdown(void);

/* Stage 2d: direct access for the host test. Stage 3 routes guest
 * SYS_L2_ALLOC / SYS_L2_FREE through the per-VM allocator instead. */
void *mgapi_l2_host_alloc(size_t size, size_t align);
void  mgapi_l2_host_free(void *p);

/* Stats for the host test + dev export. */
typedef struct {
    size_t   used_bytes;
    size_t   free_bytes;
    uint32_t alloc_count;
    uint32_t free_block_count;
    size_t   largest_free_bytes;
} MgapiL2Stats;
int  mgapi_l2_stats(MgapiL2Stats *out);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_L2_INIT_H */
