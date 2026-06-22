/* ============================================================
 *  stream_arbiter.c — see stream_arbiter.h for the contract.
 *
 *  Implementation notes
 *  --------------------
 *  Static slot table; register linearly searches for a free slot.
 *  Producer rotation: a single global index increments each call
 *  to stream_arbiter_tick(), then we walk STREAM_ARBITER_MAX
 *  slots starting at (rotation % MAX). Flat round robin.
 *
 *  Each stream is driven by a PRODUCER vtable (StreamProducer). The
 *  arbiter is agnostic to where elements come from: the built-in
 *  FILE_CHUNK producer reads fixed chunks from a vm_host_fs fd (the
 *  original behaviour, exposed via stream_arbiter_register), while
 *  other stream types (FMV complete-frame fetch, etc.) supply their
 *  own fill via stream_arbiter_register_producer. The producer fills
 *  a slot; the arbiter pushes it into the SPSC ring when there is
 *  room. On the MCU a producer's fill becomes "kick DMA / poll
 *  completion" — same scheduler, swap the producer.
 *
 *  EOF semantics: when a producer's fill() returns false the arbiter
 *  consults at_eof(); once true it sets the slot's atomic eof and
 *  skips the slot thereafter. Consumer's is_eof() returns true only
 *  when eof is set AND the ring is drained.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "io/stream_arbiter.h"

#include "containers/spsc_ring.h"
#include "vm/vm_host_fs.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* Hard ceiling on element size. 64 KB covers a full FMV chunk plus a
 * complete-frame descriptor (~30 KB). */
#define STREAM_ARBITER_MAX_CHUNK_BYTES (64u * 1024u)

typedef struct {
    bool           in_use;
    StreamProducer producer;     /* how this stream's slots get filled */
    uint32_t       chunk_bytes;  /* ring element size                  */
    uint32_t       depth;        /* slot count, power of two           */
    void          *storage;      /* chunk_bytes * depth bytes          */
    void          *scratch;      /* chunk_bytes, reused each tick      */
    SpscRing       ring;
    atomic_int     eof;          /* 1 once producer is exhausted       */
    uint64_t       bytes_read;   /* diag                               */
    uint64_t       chunks_pushed;
    uint64_t       chunks_popped;
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

/* --- built-in FILE_CHUNK producer ---------------------------- */

typedef struct {
    int      fd;
    uint32_t chunk_bytes;
    bool     eof;           /* producer-thread only (fill/at_eof in tick) */
} FileChunkCtx;

static bool file_chunk_fill(void *ctx, void *slot) {
    FileChunkCtx *f = (FileChunkCtx *)ctx;
    int32_t got = vm_host_fs_route_read(f->fd, slot, f->chunk_bytes);
    if (got <= 0 || (uint32_t)got < f->chunk_bytes) {
        /* True EOF (0), error (<0), or short read (partial last chunk) —
         * all EOF from the arbiter's POV; partial bytes are dropped. */
        f->eof = true;
        return false;
    }
    return true;
}

static bool file_chunk_at_eof(void *ctx) {
    return ((FileChunkCtx *)ctx)->eof;
}

static void file_chunk_close(void *ctx) {
    free(ctx);
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

StreamHandle stream_arbiter_register_producer(const StreamProducer *producer,
                                              uint32_t slot_bytes,
                                              uint32_t depth) {
    if (!s_initialized)                              return STREAM_HANDLE_INVALID;
    if (!producer || !producer->fill)               return STREAM_HANDLE_INVALID;
    if (slot_bytes == 0)                            return STREAM_HANDLE_INVALID;
    if (slot_bytes > STREAM_ARBITER_MAX_CHUNK_BYTES) return STREAM_HANDLE_INVALID;
    if (!is_pow2(depth))                            return STREAM_HANDLE_INVALID;

    int idx = -1;
    for (int i = 0; i < STREAM_ARBITER_MAX; i++) {
        if (!s_slots[i].in_use) { idx = i; break; }
    }
    if (idx < 0) return STREAM_HANDLE_INVALID;

    void *storage = calloc((size_t)depth, slot_bytes);
    if (!storage) return STREAM_HANDLE_INVALID;
    void *scratch = malloc(slot_bytes);
    if (!scratch) { free(storage); return STREAM_HANDLE_INVALID; }

    StreamSlot *s = &s_slots[idx];
    s->producer      = *producer;
    s->chunk_bytes   = slot_bytes;
    s->depth         = depth;
    s->storage       = storage;
    s->scratch       = scratch;
    atomic_store_explicit(&s->eof, 0, memory_order_relaxed);
    s->bytes_read    = 0;
    s->chunks_pushed = 0;
    s->chunks_popped = 0;

    if (!spsc_ring_init(&s->ring, storage, depth, slot_bytes)) {
        free(storage);
        free(scratch);
        s->storage = NULL;
        s->scratch = NULL;
        return STREAM_HANDLE_INVALID;
    }

    s->in_use = true;
    return (StreamHandle)idx;
}

StreamHandle stream_arbiter_register(int fd,
                                     uint32_t chunk_bytes,
                                     uint32_t depth) {
    if (fd < 0) return STREAM_HANDLE_INVALID;

    FileChunkCtx *f = (FileChunkCtx *)calloc(1, sizeof *f);
    if (!f) return STREAM_HANDLE_INVALID;
    f->fd          = fd;
    f->chunk_bytes = chunk_bytes;
    f->eof         = false;

    StreamProducer p;
    p.fill   = file_chunk_fill;
    p.at_eof = file_chunk_at_eof;
    p.close  = file_chunk_close;
    p.ctx    = f;

    StreamHandle h = stream_arbiter_register_producer(&p, chunk_bytes, depth);
    if (h == STREAM_HANDLE_INVALID) free(f);   /* we still own ctx on failure */
    return h;
}

void stream_arbiter_unregister(StreamHandle h) {
    StreamSlot *s = slot_for(h);
    if (!s) return;
    if (s->producer.close) s->producer.close(s->producer.ctx);
    free(s->storage);
    free(s->scratch);
    s->storage = NULL;
    s->scratch = NULL;
    s->in_use  = false;
    memset(&s->producer, 0, sizeof s->producer);
}

uint32_t stream_arbiter_tick(void) {
    if (!s_initialized) return 0;
    uint32_t pushed_total = 0;

    uint32_t start = s_rotation++;
    for (uint32_t i = 0; i < (uint32_t)STREAM_ARBITER_MAX; i++) {
        uint32_t idx = (start + i) % (uint32_t)STREAM_ARBITER_MAX;
        StreamSlot *s = &s_slots[idx];
        if (!s->in_use) continue;
        if (atomic_load_explicit(&s->eof, memory_order_acquire)) continue;

        /* Wait for the consumer to free a slot — the producer never
         * overwrites. */
        if (spsc_ring_full(&s->ring)) continue;

        if (!s->producer.fill(s->producer.ctx, s->scratch)) {
            /* Nothing produced this tick. If the source is exhausted,
             * latch EOF; otherwise leave the slot to retry next tick. */
            if (s->producer.at_eof && s->producer.at_eof(s->producer.ctx)) {
                atomic_store_explicit(&s->eof, 1, memory_order_release);
            }
            continue;
        }

        if (spsc_ring_push(&s->ring, s->scratch)) {
            s->bytes_read += s->chunk_bytes;
            s->chunks_pushed++;
            pushed_total++;
        }
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
