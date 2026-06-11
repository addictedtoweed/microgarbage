/* ============================================================
 *  stream_arbiter.h — round-robin SD/host-file stream arbiter
 *
 *  PURPOSE
 *  -------
 *  A handful of guest VMs (or guest threads) want to read from
 *  large files in fixed-size chunks at roughly steady rates. On
 *  the MCU port that's one SDMMC peripheral; reads serialize at
 *  the bus level whether we want them to or not. Rather than
 *  hand-rolling that scheduling per-consumer (each demo doing its
 *  own pre-read into its own buffer, fighting the others under
 *  load), the arbiter owns the scheduling explicitly:
 *
 *    - Up to STREAM_ARBITER_MAX active streams.
 *    - Each registered stream has a fixed chunk size and an SPSC
 *      ring of that-sized slots between the arbiter (producer)
 *      and the consuming guest (consumer).
 *    - stream_arbiter_tick() walks the registered streams in
 *      rotated order; for each one whose ring has free space and
 *      whose file isn't EOF, it reads exactly one chunk and
 *      pushes it. The rotation is incremented per tick so no
 *      stream is structurally starved.
 *
 *  WHY THIS LAYER
 *  --------------
 *  - Pattern parity with the existing music streaming path: that
 *    code already does "read chunks ahead, mixer pulls from ring."
 *    The arbiter generalizes it so an FMV stream and N music
 *    streams share the same backpressure + scheduling.
 *  - One seam, two backends. On Windows the file read is
 *    synchronous (vm_host_fs_route_read inside the worker tick).
 *    On the MCU it becomes "kick SDMMC DMA, the next tick checks
 *    completion." Same scheduler, swap the read function.
 *  - The consumer side is dead simple: stream_arbiter_consume
 *    pops one chunk if available, returns false if empty. The
 *    blocking behaviour (yield-until-chunk-ready) is layered
 *    above this by the ecall handler — the arbiter itself is
 *    purely non-blocking.
 *
 *  THREAD MODEL
 *  ------------
 *  - Producer: stream_arbiter_tick(), called from the mgapi
 *    worker thread once per vblank, BEFORE vm_step. This is the
 *    one and only writer to each ring's head.
 *  - Consumer: stream_arbiter_consume(), called from the same
 *    worker thread inside ecall handlers during vm_step. Single
 *    writer to each ring's tail.
 *  - Same-thread producer/consumer is the trivial case for SPSC.
 *    The acquire/release semantics in spsc_ring also make this
 *    safe if a future port splits producer onto a DMA-ISR
 *    context with separate cache (see spsc_ring.h cross-core
 *    note).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef STREAM_ARBITER_H
#define STREAM_ARBITER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Upper bound on concurrent streams. 4 covers the stated need
 * (one FMV + up to 3 audio streams) with one slot of headroom.
 * Increase if a new use case requires more — there is no other
 * structural reason for this number. */
#ifndef STREAM_ARBITER_MAX
#define STREAM_ARBITER_MAX 4
#endif

/* Opaque-ish per-stream handle. Internal layout is in the .c. */
typedef int StreamHandle;
#define STREAM_HANDLE_INVALID (-1)

/* Initialize the global arbiter. Idempotent; calling twice is a
 * no-op (returns true). NOT concurrency-safe — call once from
 * mgapi_init before the worker thread starts. */
bool stream_arbiter_init(void);

/* Tear down: closes any streams still registered, frees ring
 * storage. NOT concurrency-safe — call from mgapi_shutdown after
 * the worker thread is joined. */
void stream_arbiter_shutdown(void);

/* Register a stream against an already-open vm_host_fs fd.
 *   fd          : open file descriptor in the vm_host_fs table.
 *                 The arbiter does NOT own this fd; the caller
 *                 still must fs_close it. Closing the fd while
 *                 the stream is registered yields short reads
 *                 then EOF; safer to unregister first.
 *   chunk_bytes : fixed read amount per ring slot. Must be > 0
 *                 and ≤ STREAM_ARBITER_MAX_CHUNK_BYTES.
 *   depth       : ring slot count. Must be a power of two ≥ 2
 *                 (per spsc_ring contract). Usable depth is one
 *                 less than this — so depth=4 means up to 3 chunks
 *                 buffered ahead.
 * Returns a StreamHandle ≥ 0 on success, STREAM_HANDLE_INVALID on
 * any failure (no slots, bad args, allocation failure). */
StreamHandle stream_arbiter_register(int fd,
                                     uint32_t chunk_bytes,
                                     uint32_t depth);

/* Unregister and free ring storage. Idempotent for an invalid
 * handle. Does NOT close the fd. */
void stream_arbiter_unregister(StreamHandle h);

/* Producer step. Walks registered streams in rotated order; for
 * each one whose ring has free space and isn't past EOF, reads
 * one chunk via vm_host_fs_route_read and pushes it. Cheap when
 * all rings are full (early-exits each stream). Returns the
 * number of chunks pushed this call (mostly for diagnostics). */
uint32_t stream_arbiter_tick(void);

/* Non-blocking consume. Pops one chunk into `dst` (must be at
 * least chunk_bytes long, set at register time). Returns true if
 * a chunk was popped, false if the ring is empty. EOF is signaled
 * separately via stream_arbiter_is_eof — a false return with
 * is_eof() true means "no more data ever." */
bool stream_arbiter_consume(StreamHandle h, void *dst);

/* True if the producer has hit EOF AND the consumer ring is
 * empty (i.e., nothing more will ever come out). The "consumer
 * ring empty" half is checked here so a guest's consume-loop
 * doesn't quit while there are still chunks queued behind a
 * partial last read. */
bool stream_arbiter_is_eof(StreamHandle h);

/* Diagnostic observers. Best-effort under concurrency (same
 * caveats as spsc_ring observers). */
uint32_t stream_arbiter_chunks_available(StreamHandle h);
uint32_t stream_arbiter_chunk_bytes(StreamHandle h);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* STREAM_ARBITER_H */
