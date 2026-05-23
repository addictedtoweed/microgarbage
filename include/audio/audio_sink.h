/* ============================================================
 *  audio_sink.h — desktop audio output backend seam
 *
 *  A tiny swappable seam between "the service renders mixed PCM" and
 *  "something consumes it" — the desktop analogue of the H745's
 *  SAI+DMA->DAC path. Two implementations live behind it:
 *
 *    wav_dump   — writes the mixed output to a .wav file. Fully
 *                 testable without any sound hardware: render, dump,
 *                 read back, compare. This is the bedrock — it proves
 *                 the engine produces correct samples independent of
 *                 any OS audio stack. You can also just open the .wav
 *                 in any player to listen.
 *
 *    waveout    — live output on Windows via the winmm waveOut API
 *                 (NO extra library — winmm ships with Windows; mingw
 *                 has the headers). Structurally correct but its
 *                 audibility is verified on real Windows, not here.
 *
 *  Zero external dependencies by design: the WAV writer hand-rolls
 *  the RIFF header; the waveOut backend calls Win32 directly. Matches
 *  the repo's no-extra-libs philosophy (no PortAudio/SDL/libsndfile).
 *
 *  Format is fixed to what audio_service_render produces: interleaved
 *  signed 16-bit stereo at the service sample rate.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef AUDIO_SINK_H
#define AUDIO_SINK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* The output format is fixed: 16-bit signed, 2 channels (stereo). */
#define AUDIO_SINK_CHANNELS  2
#define AUDIO_SINK_BITS      16

typedef struct AudioSink AudioSink;

/* Backend vtable. A backend implements these; callers use the
 * AudioSink wrappers below and never touch the vtable directly. */
typedef struct {
    const char *name;
    /* Open the device/file for `sample_rate` Hz stereo s16. Returns a
     * backend context pointer, or NULL on failure. */
    void *(*open)(uint32_t sample_rate);
    /* Write `frames` interleaved-stereo s16 frames (frames*2 samples).
     * Returns the number of frames accepted (may be < frames if a
     * device buffer is momentarily full; callers retry). Returns -1 on
     * error. */
    int   (*write)(void *ctx, const int16_t *interleaved, uint32_t frames);
    /* Flush any buffered audio and close. */
    void  (*close)(void *ctx);
} AudioSinkBackend;

struct AudioSink {
    const AudioSinkBackend *backend;
    void                   *ctx;
    uint32_t                sample_rate;
};

/* Open a sink using the named backend ("wav" or "wave"/"waveout").
 * For the WAV backend, `path` is the output file; for live backends
 * `path` is ignored (may be NULL). Returns false on failure. */
bool audio_sink_open(AudioSink *sink, const char *backend_name,
                     const char *path, uint32_t sample_rate);

/* Write interleaved stereo s16 frames. Returns frames accepted or -1. */
int  audio_sink_write(AudioSink *sink, const int16_t *interleaved,
                      uint32_t frames);

/* Flush + close. */
void audio_sink_close(AudioSink *sink);

/* ---- backends (exposed so tests can use them directly) ---- */
extern const AudioSinkBackend audio_sink_wav;       /* always available */
#if defined(_WIN32)
extern const AudioSinkBackend audio_sink_waveout;   /* Windows only     */
#endif

#endif /* AUDIO_SINK_H */
