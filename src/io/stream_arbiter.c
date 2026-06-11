/* ============================================================
 *  stream_arbiter.c — see stream_arbiter.h for the contract.
 *
 *  Implementation notes
 *  --------------------
 *  Static slot table; register linearly searches for a free slot.
 *  Producer rotation: a single global index increments each call
 *  to stream_arbiter_tick(), then we walk STREAM_ARBITER_MAX
 *  slots starting at (rotation % MAX). That gives each registered
 *  stream an equal share of "first crack" at the next chunk read,
 *  with no per-stream priority/weight logic — flat round robin.
 *  If/when we need weighting (FMV reading 2 chunks for every 1
 *  music chunk, say), that lives here as a per-slot weight counter
 *  consulted inside the walk.
 *
 *  EOF semantics: producer sets s_eof atomically when
 *  vm_host_fs_route_read returns ≤0 or a short read. From that
 *  point the slot is skipped by tick(). Consumer's is_eof()
 *  returns true only when producer is EOF AND the consumer ring
 *  is empty — so a consumer draining the last few queued chunks
 *  doesn't see EOF prematurely.
 *
 *  Short reads on a non-EOF boundary: the file contract for our
 *  use cases (FMV files, WAV bodies) is that the read offset
 *  past any header is chunk-aligned, so short reads only happen
 *  at true EOF. We treat a short read as EOF and DROP the partial
 *  bytes — consumer never sees a partial chunk. Files that need
 *  partial-final-chunk delivery would need a different API.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "io/stream_arbiter.h"

#include "containers/spsc_ring.h"
#include "vm/vm_host_fs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Hard ceiling on chunk size. 64 KB covers a full FMV frame
 * (35596 B) plus any future-but-reasonable audio chunk. Beyond
 * this and per-stream storage (chunk × depth) starts being a
 * meaningful chunk of mgapi RAM. */
#define STREAM_ARBITER_MAX_CHUNK_BYTES (64u * 1024u)

typedef struct {
    bool       in_use;
    int        fd;
    uint32_t   chunk_bytes;
    uint32_t   depth;            /* slot count, power of two */
    void      *storage;          /* chunk_bytes * depth bytes */
    void      *scratch;          /* chunk_bytes, reused each tick */
    SpscRing   ring;
    atomic_int eof;              /* 1 once producer has seen EOF */
    uint64_t   bytes_read;       /* diag: total bytes pulled from fd */
    uint64_t   chunks_pushed;    /* diag */
    uint64_t   chunks_popped;    /* diag */
} StreamSlot;

static StreamSlot s_slots[STREAM_ARBITER_MAX];
static bool       s_initialized;
static uint32_t   s_rotation;

/* --- helpers ------------------------------------------------- */

static bool is_pow2(uint32_t v) {
    return v >= 2u && (v & (v - 1u)) == 0u;
}

static StreamSlot *slot_for(StreamHandle h) {
    if (h < 0 || h >= (int)STREAM_ARBITER_MAX) return NULL;
    StreamSlot *s = &s_slots[h];
    return s->in_use ? s : NULL;
}

/* --- public API ---------------------------------------------- */

bool stream_arbiter_init(void) {
    if (s_initialized) return true;
    memset(s_slots, 0, sizeof s_slots);
    s_rotation    = 0;
    s_initialized = true;
    return true;
}

void stream_arbiter_shutdown(void) {
    if (!s_initialized) return;
    for (int i = 0; i < STREAM_ARBITER_MAX; i++) {
        if (s_slots[i].in_use) {
            stream_arbiter_unregister((StreamHandle)i);
        }
    }
    s_initialized = false;
}

StreamHandle stream_arbiter_register(int fd,
                                     uint32_t chunk_bytes,
                                     uint32_t depth) {
    if (!s_initialized) return STREAM_HANDLE_INVALID;
    if (fd < 0)                                         return STREAM_HANDLE_INVALID;
    if (chunk_bytes == 0)                               return STREAM_HANDLE_INVALID;
    if (chunk_bytes > STREAM_ARBITER_MAX_CHUNK_BYTES)   return STREAM_HANDLE_INVALID;
    if (!is_pow2(depth))                                return STREAM_HANDLE_INVALID;

    /* Find a free slot. */
    int idx = -1;
    for (int i = 0; i < STREAM_ARBITER_MAX; i++) {
        if (!s_slots[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return STREAM_HANDLE_INVALID;

    /* Allocate per-stream ring storage. The SPSC ring needs
     * chunk_bytes-sized elements; total storage = chunk * depth.
     * For depth=4 and chunk=35596 that's ~140 KB per FMV stream;
     * for a music stream at chunk=16384 depth=4 it's 64 KB. */
    void *storage = calloc((size_t)depth, chunk_bytes);
    if (!storage) return STREAM_HANDLE_INVALID;
    /* Per-slot scratch buffer for the file-read landing zone.
     * spsc_ring_push copies-in from a caller-owned element, so we
     * need this whether ring storage exists or not. One alloc at
     * register time avoids per-tick churn. */
    void *scratch = malloc(chunk_bytes);
    if (!scratch) { free(storage); return STREAM_HANDLE_INVALID; }

    StreamSlot *s = &s_slots[idx];
    s->fd            = fd;
    s->chunk_bytes   = chunk_bytes;
    s->depth         = depth;
    s->storage       = storage;
    s->scratch       = scratch;
    atomic_store_explicit(&s->eof, 0, memory_order_relaxed);
    s->bytes_read    = 0;
    s->chunks_pushed = 0;
    s->chunks_popped = 0;

    if (!spsc_ring_init(&s->ring, storage, depth, chunk_bytes)) {
        free(storage);
        free(scratch);
        s->storage = NULL;
        s->scratch = NULL;
        return STREAM_HANDLE_INVALID;
    }

    s->in_use = true;
    return (StreamHandle)idx;
}

void stream_arbiter_unregister(StreamHandle h) {
    StreamSlot *s = slot_for(h);
    if (!s) return;
    free(s->storage);
    free(s->scratch);
    s->storage = NULL;
    s->scratch = NULL;
    s->in_use  = false;
    /* Leave diagnostic counters around — slot will be zeroed on
     * next register if reused. */
}

uint32_t stream_arbiter_tick(void) {
    if (!s_initialized) return 0;
    uint32_t pushed_total = 0;

    /* Walk all slots starting at rotation. Note we examine every
     * slot every tick — the rotation only changes WHICH slot
     * gets first crack. A starved stream would only happen if
     * we early-exited at a full ring, which we don't. */
    uint32_t start = s_rotation++;
    for (uint32_t i = 0; i < (uint32_t)STREAM_ARBITER_MAX; i++) {
        uint32_t idx = (start + i) % (uint32_t)STREAM_ARBITER_MAX;
        StreamSlot *s = &s_slots[idx];
        if (!s->in_use) continue;
        if (atomic_load_explicit(&s->eof, memory_order_acquire)) continue;

        /* Skip if the ring has no room for another chunk. The
         * producer never overwrites; we wait until the consumer
         * frees a slot. */
        if (spsc_ring_full(&s->ring)) continue;

        /* Pull one chunk's worth of bytes from the fd into the
         * per-slot scratch buffer, then push the whole thing as
         * one ring element. The push is a memcpy into the ring's
         * storage; scratch is reused next tick. */
        int32_t got = vm_host_fs_route_read(s->fd, s->scratch,
                                            s->chunk_bytes);
        if (got <= 0 || (uint32_t)got < s->chunk_bytes) {
            /* True EOF (got==0), error (got<0), or short read
             * (partial last chunk). All three are EOF from the
             * arbiter's POV. Don't push partial bytes. */
            atomic_store_explicit(&s->eof, 1, memory_order_release);
            continue;
        }

        s->bytes_read += (uint64_t)got;

        if (spsc_ring_push(&s->ring, s->scratch)) {
            s->chunks_pushed++;
            pushed_total++;
        }
        /* If push failed (ring went full between full-check and
         * push), the bytes are dropped — the file offset advanced
         * but the consumer never sees them. This shouldn't happen
         * since we hold the only producer, but defensively the
         * spsc_ring_full() check above prevents it. */
    }
    return pushed_total;
}

bool stream_arbiter_consume(StreamHandle h, void *dst) {
    StreamSlot *s = slot_for(h);
    if (!s || !dst) return false;
    bool ok = spsc_ring_pop(&s->ring, dst);
    if (ok) s->chunks_popped++;
    return ok;
}

bool stream_arbiter_is_eof(StreamHandle h) {
    StreamSlot *s = slot_for(h);
    if (!s) return true;   /* not registered = no more data */
    if (!atomic_load_explicit(&s->eof, memory_order_acquire)) return false;
    /* Producer has set EOF. Real EOF only when ring is also drained. */
    return spsc_ring_empty(&s->ring);
}

uint32_t stream_arbiter_chunks_available(StreamHandle h) {
    StreamSlot *s = slot_for(h);
    if (!s) return 0;
    return (uint32_t)spsc_ring_count(&s->ring);
}

uint32_t stream_arbiter_chunk_bytes(StreamHandle h) {
    StreamSlot *s = slot_for(h);
    if (!s) return 0;
    return s->chunk_bytes;
}
