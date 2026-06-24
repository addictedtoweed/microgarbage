/* ============================================================
 *  audio_ring_stream.h — a music_player stream source backed by a
 *  push-fed SPSC ring of interleaved stereo PCM16.
 *
 *  One thread PUSHES (e.g. the FMV video producer extracting each
 *  unit's muxed audio chunk on the mgapi worker thread); the audio
 *  service's music_update PULLS via the music_stream_fn contract.
 *  This decouples a producer that delivers audio in bursts from the
 *  mixer that consumes it smoothly, and it lets the FMV clip's audio
 *  drop into the SAME machinery as the file/pool sources (so it gets
 *  the pinned head + streaming buffer + drift handling for free).
 *
 *  Generic — not FMV-specific. Single-producer / single-consumer:
 *  exactly one thread calls push/set_eof, exactly one calls read.
 *
 *  Output is INTERLEAVED STEREO PCM16 (the mixer's music format).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef AUDIO_RING_STREAM_H
#define AUDIO_RING_STREAM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>

/* Ring capacity in stereo frames. Power of two so cursor masking stays
 * cheap; ~743 ms at 44.1 kHz — comfortably more than the video ring's
 * look-ahead plus the music_player streaming buffer's refill. */
#define AUDIO_RING_STREAM_FRAMES 32768u

typedef struct {
    int16_t     buf[AUDIO_RING_STREAM_FRAMES * 2];  /* L,R interleaved */
    atomic_uint w;     /* frames written (wraps at u32; count = w - r)  */
    atomic_uint r;     /* frames read                                   */
    atomic_bool eof;   /* producer signalled end-of-stream              */

    /* Diagnostic: count of reads that had to pad silence because the
     * producer was momentarily behind (NOT eof) — i.e. ring underruns, the
     * classic cause of an occasional click. Monotonic; cleared by reset(). */
    _Atomic uint32_t underruns;

    /* External master-clock feedback for A/V drift sync. The platform
     * writes a cumulative tick count, in the mixer's sample-rate units,
     * representing how far the *video/SNES* clock has advanced (on the
     * desktop: bsnes' audio-pull cadence; on the H745: the M4's SNES
     * master-clock feedback). The audio service reads it each render and
     * feeds the mixer's drift PLL so WASAPI-paced playback tracks the
     * SNES clock instead of free-running. Monotonic; survives reset(). */
    _Atomic uint64_t external_clock;
} AudioRingStream;

/* Reset to empty + clear eof. Call before (re)starting a stream. */
void audio_ring_stream_reset(AudioRingStream *s);

/* PRODUCER: push `n` interleaved-stereo PCM16 frames. Returns frames
 * accepted (< n if the ring is full). */
size_t audio_ring_stream_push(AudioRingStream *s, const int16_t *frames, size_t n);

/* PRODUCER: free frames the ring can still accept (backpressure hint). */
size_t audio_ring_stream_free(const AudioRingStream *s);

/* PRODUCER: signal no more data will arrive (end of clip). */
void audio_ring_stream_set_eof(AudioRingStream *s);

/* PLATFORM: publish the external master clock (cumulative ticks, mixer
 * sample-rate units). Called from the audio-output cadence (e.g. each
 * bsnes audio pull). Lock-free; safe to call from a different thread than
 * the reader. */
void audio_ring_stream_set_external_clock(AudioRingStream *s, uint64_t ticks);

/* CONSUMER: read the latest external clock (0 if never set). */
uint64_t audio_ring_stream_external_clock(const AudioRingStream *s);

/* DIAG: cumulative underrun (silence-pad) count, and current fill in frames. */
uint32_t audio_ring_stream_underruns(const AudioRingStream *s);
uint32_t audio_ring_stream_avail(const AudioRingStream *s);

/* music_stream_fn: pop up to `requested_samples` FRAMES into `destination`
 * (requested_samples*2 int16). `offset_in_stream` / `stream_id` are ignored —
 * playback is sequential, one stream. If the ring is short but NOT at eof, it
 * pads the remainder with silence and returns `requested_samples` (so a
 * momentarily-behind producer doesn't look like end-of-stream). At eof it
 * returns only what remains (< requested → music_player ends the segment). */
size_t audio_ring_stream_read(void *user_data, int stream_id, void *destination,
                              size_t requested_samples, size_t offset_in_stream);

#endif /* AUDIO_RING_STREAM_H */
