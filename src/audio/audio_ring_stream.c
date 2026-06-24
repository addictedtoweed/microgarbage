/* ============================================================
 *  audio_ring_stream.c — push-fed SPSC ring music_stream_fn source.
 *  See audio_ring_stream.h.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "audio/audio_ring_stream.h"

#include <string.h>

#define RS_MASK (AUDIO_RING_STREAM_FRAMES - 1u)
_Static_assert((AUDIO_RING_STREAM_FRAMES & RS_MASK) == 0,
               "AUDIO_RING_STREAM_FRAMES must be a power of two");

void audio_ring_stream_reset(AudioRingStream *s) {
    if (!s) return;
    atomic_store_explicit(&s->w, 0u, memory_order_relaxed);
    atomic_store_explicit(&s->r, 0u, memory_order_relaxed);
    atomic_store_explicit(&s->underruns, 0u, memory_order_relaxed);
    atomic_store_explicit(&s->eof, false, memory_order_release);
}

size_t audio_ring_stream_free(const AudioRingStream *s) {
    if (!s) return 0;
    uint32_t w = atomic_load_explicit(&s->w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&s->r, memory_order_acquire);
    return (size_t)(AUDIO_RING_STREAM_FRAMES - (w - r));
}

size_t audio_ring_stream_push(AudioRingStream *s, const int16_t *frames, size_t n) {
    if (!s || !frames || n == 0) return 0;
    uint32_t w = atomic_load_explicit(&s->w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&s->r, memory_order_acquire);
    uint32_t freef = AUDIO_RING_STREAM_FRAMES - (w - r);
    if (n > freef) n = freef;
    for (size_t i = 0; i < n; i++) {
        uint32_t idx = (w + (uint32_t)i) & RS_MASK;
        s->buf[idx * 2 + 0] = frames[i * 2 + 0];
        s->buf[idx * 2 + 1] = frames[i * 2 + 1];
    }
    atomic_store_explicit(&s->w, w + (uint32_t)n, memory_order_release);
    return n;
}

void audio_ring_stream_set_eof(AudioRingStream *s) {
    if (s) atomic_store_explicit(&s->eof, true, memory_order_release);
}

void audio_ring_stream_set_external_clock(AudioRingStream *s, uint64_t ticks) {
    if (s) atomic_store_explicit(&s->external_clock, ticks, memory_order_relaxed);
}

uint64_t audio_ring_stream_external_clock(const AudioRingStream *s) {
    if (!s) return 0;
    return atomic_load_explicit(&s->external_clock, memory_order_relaxed);
}

uint32_t audio_ring_stream_underruns(const AudioRingStream *s) {
    if (!s) return 0;
    return atomic_load_explicit(&s->underruns, memory_order_relaxed);
}

uint32_t audio_ring_stream_avail(const AudioRingStream *s) {
    if (!s) return 0;
    uint32_t w = atomic_load_explicit(&s->w, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&s->r, memory_order_relaxed);
    return w - r;
}

size_t audio_ring_stream_read(void *user_data, int stream_id, void *destination,
                              size_t requested_samples, size_t offset_in_stream) {
    (void)stream_id;
    (void)offset_in_stream;
    AudioRingStream *s = (AudioRingStream *)user_data;
    int16_t *dst = (int16_t *)destination;
    if (!s || !dst || requested_samples == 0) return 0;

    uint32_t r = atomic_load_explicit(&s->r, memory_order_relaxed);
    uint32_t w = atomic_load_explicit(&s->w, memory_order_acquire);
    uint32_t avail = w - r;
    size_t take = (avail < requested_samples) ? (size_t)avail : requested_samples;

    for (size_t i = 0; i < take; i++) {
        uint32_t idx = (r + (uint32_t)i) & RS_MASK;
        dst[i * 2 + 0] = s->buf[idx * 2 + 0];
        dst[i * 2 + 1] = s->buf[idx * 2 + 1];
    }
    atomic_store_explicit(&s->r, r + (uint32_t)take, memory_order_release);

    if (take < requested_samples) {
        if (!atomic_load_explicit(&s->eof, memory_order_acquire)) {
            /* Producer momentarily behind — pad silence, claim the full
             * request so music_player doesn't read this as end-of-stream. */
            atomic_fetch_add_explicit(&s->underruns, 1u, memory_order_relaxed);
            memset(dst + take * 2, 0,
                   (requested_samples - take) * 2u * sizeof(int16_t));
            return requested_samples;
        }
        /* eof + short read → music_player ends the segment. */
    }
    return take;
}
