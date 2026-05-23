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

/* ============================================================
 *  WAV reader (for drop-in assets)
 *
 *  Parses a canonical PCM WAV from a memory buffer (the bytes read
 *  the bytes read from a /host *.wav file). Zero-dep; the mirror of
 *  Accepts 8- or 16-bit PCM, mono or stereo, any sample rate. The
 *  caller converts to the format the audio pool/mixer want (the asset
 *  loader downmixes stereo->mono and 8->16 bit for the current SFX
 *  path).
 * ============================================================ */

typedef struct {
    uint16_t  format;        /* 1 = PCM (only PCM supported)        */
    uint16_t  channels;      /* 1 or 2                              */
    uint32_t  sample_rate;
    uint16_t  bits;          /* 8 or 16                             */
    const uint8_t *data;     /* points into the source buffer       */
    uint32_t  data_bytes;    /* size of the data chunk              */
} WavInfo;

typedef enum {
    WAV_OK = 0,
    WAV_ERR_TOO_SMALL,       /* buffer too small for a header       */
    WAV_ERR_BAD_MAGIC,       /* not RIFF/WAVE                       */
    WAV_ERR_NOT_PCM,         /* compressed / non-PCM format         */
    WAV_ERR_NO_DATA,         /* no data chunk found                 */
    WAV_ERR_UNSUPPORTED,     /* bits/channels we don't handle       */
} WavResult;

/* Parse a WAV from `buf` (`len` bytes). On success fills *out (its
 * `data` pointer aliases into `buf`, so keep `buf` alive). Walks the
 * chunk list to find fmt + data (tolerates extra chunks like LIST). */
WavResult wav_parse(const uint8_t *buf, size_t len, WavInfo *out);

/* Convert parsed WAV PCM into mono PCM16 (the current SFX format) in
 * `dst` (caller-allocated, room for `max_frames` int16). Downmixes
 * stereo->mono and promotes 8-bit->16-bit. Returns frames written. */
uint32_t wav_to_mono_pcm16(const WavInfo *info, int16_t *dst,
                           uint32_t max_frames);

#endif /* AUDIO_SINK_H */
