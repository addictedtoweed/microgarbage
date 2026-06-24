/* ============================================================
 *  audio_mixer.h — multi-channel audio mixer
 *
 *  Capabilities through v2:
 *    - Mixes N input channels (8u, 8s, or 16s; mono or stereo)
 *      into a mono or stereo output of configurable bit width.
 *    - Output: 1-16 bit resolution, signed or unsigned, packed
 *      into 8-bit or 16-bit containers. 1 or 2 output channels.
 *    - Per-channel source rate with linear or cubic interpolation
 *      (covers the "store SFX at 11/22 kHz to save RAM" pattern).
 *    - Per-channel volume and pan.
 *    - Looping playback per channel.
 *    - Mute and active state.
 *    - Headroom-based saturation.
 *    - Startup-only allocation via a caller-supplied allocator
 *      (defaults to malloc/free). After create, no further
 *      allocations happen.
 *    - Optional drift correction (v2) against an external clock,
 *      with low-pass smoothing of the correction value.
 *
 *  ---------------------------------------------------------------
 *  Vocabulary
 *  ---------------------------------------------------------------
 *
 *  "Channel" in this header means an input voice — one independent
 *  audio stream that gets mixed into the output. Think mixing-board
 *  channel strips. A typical SNES cart setup might have:
 *    channel 0: streaming BGM, 16-bit stereo, large buffer
 *    channel 1: SFX slot, 8-bit unsigned mono, small buffer
 *    channel 2: SFX slot, 8-bit unsigned mono, small buffer
 *    ...
 *
 *  Output channels (mono = 1, stereo = 2) live in MixerOutputFormat.
 *
 *  ---------------------------------------------------------------
 *  Source format
 *  ---------------------------------------------------------------
 *
 *  Set per-channel. The mixer sizes the internal source buffer
 *  based on the format (1 byte/sample for 8-bit, 2 bytes for 16-bit,
 *  doubled for stereo). The caller writes samples into the channel
 *  via mixer_write_channel; the buffer-pointer's actual type must
 *  match the format:
 *    PCM16_MONO    → const int16_t* (or const q15_t*)
 *    PCM16_STEREO  → const int16_t* (interleaved L, R, L, R)
 *    PCM8S_MONO    → const int8_t*
 *    PCM8S_STEREO  → const int8_t* (interleaved L, R, L, R)
 *    PCM8U_MONO    → const uint8_t*
 *    PCM8U_STEREO  → const uint8_t* (interleaved L, R, L, R)
 *
 *  For stereo sources, `count` in mixer_write_channel is the number
 *  of frames (L+R pairs), not the number of individual samples.
 *
 *  ---------------------------------------------------------------
 *  Output format
 *  ---------------------------------------------------------------
 *
 *  MixerOutputFormat fmt = {
 *      .bits         = 12,    // DAC resolution
 *      .is_signed    = false, // unsigned (centered at 2^(bits-1))
 *      .storage_bits = 16,    // each sample in a uint16_t
 *      .channels     = 2,     // stereo
 *  };
 *
 *  Common cases:
 *    8-bit unsigned DAC, stereo:   { 8,  false, 8,  2 }
 *    12-bit unsigned DAC, mono:    { 12, false, 16, 1 }
 *    16-bit signed DAC, stereo:    { 16, true,  16, 2 } (q15 stereo)
 *
 *  Constraints:
 *    - bits 1..16
 *    - storage_bits 8 or 16
 *    - When storage_bits == 8, bits must be <= 8
 *    - channels 1 or 2
 *
 *  ---------------------------------------------------------------
 *  Output buffer
 *  ---------------------------------------------------------------
 *
 *  mixer_render takes a `void *output`. The actual buffer type:
 *    storage_bits == 8:   uint8_t* or int8_t* (per is_signed)
 *    storage_bits == 16:  uint16_t* or int16_t* (per is_signed)
 *
 *  Required size in bytes:
 *    frame_count * channels * (storage_bits / 8)
 *
 *  Mismatching the buffer type and configured format will write
 *  wrong bytes to memory. Caller's responsibility.
 *
 *  ---------------------------------------------------------------
 *  Stereo<->mono conversion
 *  ---------------------------------------------------------------
 *
 *  Mono source → stereo output: same sample to L and R.
 *  Stereo source → mono output: (L + R) / 2 — preserves perceived
 *  level on downmix. Standard convention.
 *  Mono → mono and stereo → stereo: 1:1 channel mapping.
 *
 *  ---------------------------------------------------------------
 *  Headroom and saturation
 *  ---------------------------------------------------------------
 *
 *  The int32 mix accumulator has headroom for ~65536 simultaneous
 *  q15-range samples. Before saturating to output range, the
 *  accumulator is right-shifted by `headroom_bits`:
 *    0 bits: max output level, clipping on loud peaks
 *    3 bits: 18 dB headroom — typical default
 *    6 bits: 36 dB headroom — very conservative
 *
 *  ---------------------------------------------------------------
 *  Convenience sizing macros
 *  ---------------------------------------------------------------
 *
 *  Override before including this header to change them:
 *    #define MIXER_LONG_BUFFER_SAMPLES   4096
 *    #include "audio_mixer.h"
 *
 *  Depends on: ring_buffer, fixed_point
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef AUDIO_MIXER_H
#define AUDIO_MIXER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "math/fixed_point.h"

/* ============================================================
 *  Convenience buffer-size hints
 * ============================================================ */

#ifndef MIXER_LONG_BUFFER_SAMPLES
#  define MIXER_LONG_BUFFER_SAMPLES   2048   /* for streaming channels */
#endif

#ifndef MIXER_SHORT_BUFFER_SAMPLES
#  define MIXER_SHORT_BUFFER_SAMPLES   256   /* for one-shot SFX */
#endif

/* ============================================================
 *  Opaque mixer handle. Allocated by mixer_create*, freed by
 *  mixer_destroy. Internal structure is private to audio_mixer.c.
 * ============================================================ */

typedef struct AudioMixer AudioMixer;

/* ============================================================
 *  Source format (per input channel)
 * ============================================================ */

typedef enum {
    MIXER_SRC_PCM16_MONO,     /* int16_t / q15_t,         2 bytes/sample      */
    MIXER_SRC_PCM8S_MONO,     /* int8_t signed,           1 byte/sample       */
    MIXER_SRC_PCM8U_MONO,     /* uint8_t, 128 = silence,  1 byte/sample       */
    MIXER_SRC_PCM16_STEREO,   /* int16_t, interleaved,    4 bytes/frame       */
    MIXER_SRC_PCM8S_STEREO,   /* int8_t signed,           2 bytes/frame       */
    MIXER_SRC_PCM8U_STEREO,   /* uint8_t, 128 = silence,  2 bytes/frame       */
} MixerSourceFormat;

/* ============================================================
 *  Output format (one per mixer)
 * ============================================================ */

typedef struct {
    uint8_t bits;          /* output resolution, 1..16          */
    bool    is_signed;     /* signed (centered 0) vs unsigned   */
    uint8_t storage_bits;  /* 8 or 16                            */
    uint8_t channels;      /* 1 (mono) or 2 (stereo)             */
} MixerOutputFormat;

/* ============================================================
 *  Interpolation modes for source-rate conversion
 *
 *  When a channel's source rate differs from the mixer's output
 *  rate, the mixer must resample. The interpolation mode trades
 *  cycles for quality:
 *
 *  Cycles per channel per output frame (approximate):
 *
 *                  M0 @48   M3 @72   M4 @150 (DSP)
 *  LINEAR:           ~8       ~5         ~3
 *  CUBIC:            ~50      ~35        ~20
 *
 *  Guidance:
 *    M0: use LINEAR everywhere; cubic is too expensive at 8+
 *        channels.
 *    M3: LINEAR for SFX, CUBIC for music/voice.
 *    M4: CUBIC everywhere is comfortable.
 *
 *  CUBIC is the practical quality ceiling for upsampling — playing
 *  lower-rate stored content into a higher-rate output mix. The
 *  audible difference between cubic and higher-tap sinc kernels
 *  is negligible for this use case.
 *
 *  When source rate == output rate (or source_sample_rate == 0),
 *  the mixer skips interpolation entirely regardless of this
 *  setting. The fast 1:1 path is used.
 * ============================================================ */

typedef enum {
    MIXER_INTERP_LINEAR,
    MIXER_INTERP_CUBIC,
} MixerInterpolation;

/* ============================================================
 *  Per-channel input config (passed to mixer_create)
 *
 *  The mixer allocates the source buffer at create time using
 *  the configured allocator. `buffer_samples` is in:
 *    - samples for mono sources
 *    - frames (L+R pairs) for stereo sources
 *
 *  `source_sample_rate` of 0 means "same as output rate" and bypasses
 *  the interpolator entirely.
 *
 *  `pan` is q15: -Q15_ONE = full left, 0 = center, +Q15_ONE = full
 *  right. Linear panning law (simpler than equal-power, sufficient
 *  for game audio). Ignored when output is mono.
 *
 *  `loop` makes the channel's buffer cycle indefinitely. The buffer
 *  contents become a circular playback array — samples are not
 *  consumed, just read with a wrapping position. Use mixer_write_channel
 *  to load loop content, then mixer_channel_start. To stop the loop,
 *  mixer_channel_stop or mixer_channel_reset.
 * ============================================================ */

typedef struct {
    MixerSourceFormat  format;
    size_t             buffer_samples;
    q15_t              volume;
    int                source_sample_rate;   /* Hz; 0 = same as output */
    q15_t              pan;                  /* -Q15_ONE..+Q15_ONE     */
    bool               loop;
    MixerInterpolation interp;
} MixerChannelConfig;

/* ============================================================
 *  Allocator function pointers
 * ============================================================ */

typedef void *(*mixer_alloc_fn)(size_t n);
typedef void  (*mixer_free_fn)(void *p);

/* ============================================================
 *  Sync configuration (v2)
 *
 *  Enables drift correction between the mixer's internal sample
 *  clock and an external timing source. When enabled, all channels
 *  go through the resampling path so the rate correction applies
 *  uniformly — including channels that would otherwise be 1:1
 *  matched to the output rate.
 *
 *  The application periodically calls mixer_observe_sync with the
 *  current pair of counters: how many output frames the mixer has
 *  rendered (internal) and how many external ticks have elapsed.
 *  The mixer computes the drift and applies a smoothed correction
 *  to per-channel playback rates.
 *
 *  Sync is opt-in via mixer_create_with_sync. The existing create
 *  functions produce non-sync-enabled mixers as before.
 *
 *  Smoothing: a low-pass filter on the drift measurement. The
 *  smoothing coefficient (q15) controls how fast the correction
 *  reacts to new observations. Smaller = slower, smoother. Typical:
 *  ~Q15_ONE / 32 (about 3% per observation).
 *
 *  Max correction: clamps the magnitude of the correction so a bad
 *  measurement can't make playback wildly wrong. Typical: ~Q15_ONE / 100
 *  (1% rate adjustment) — drift between two real oscillators is
 *  almost always under 100 PPM (~0.01%).
 * ============================================================ */

typedef struct {
    /* The expected rate of the external clock, in ticks per second.
     * For an SNES at 21.477 MHz this would be 21477272. */
    uint64_t external_ticks_per_second;

    /* Low-pass filter coefficient as q15. Smaller = smoother but
     * slower to react. 0 disables correction entirely. */
    q15_t correction_smoothing;

    /* Maximum correction magnitude (q15). Clamps |correction| at
     * this value. Typical: Q15_ONE / 100 (1%). */
    q15_t max_correction;
} MixerSyncConfig;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Create a mixer with the given channel configuration and output
 * format. Uses malloc/free internally. Returns NULL on allocation
 * failure or invalid output format. */
AudioMixer *mixer_create(const MixerChannelConfig *channels,
                         size_t channel_count,
                         int output_sample_rate,
                         MixerOutputFormat out_format,
                         uint8_t headroom_bits);

/* Same as mixer_create but uses a custom allocator. Pass NULL for
 * either function pointer to use the default (malloc / free). The
 * allocator pair is stored on the mixer and used for the mixer's
 * entire lifetime, including the eventual mixer_destroy. */
AudioMixer *mixer_create_with_allocator(
                         const MixerChannelConfig *channels,
                         size_t channel_count,
                         int output_sample_rate,
                         MixerOutputFormat out_format,
                         uint8_t headroom_bits,
                         mixer_alloc_fn alloc,
                         mixer_free_fn  free_fn);

/* Create a mixer with sync (drift correction) enabled.
 *
 * All channels go through the resampling path so correction applies
 * uniformly. The application is expected to call mixer_observe_sync
 * periodically with the current pair of internal and external
 * counter values.
 *
 * Pass NULL for alloc/free to use malloc/free. */
AudioMixer *mixer_create_with_sync(
                         const MixerChannelConfig *channels,
                         size_t channel_count,
                         int output_sample_rate,
                         MixerOutputFormat out_format,
                         uint8_t headroom_bits,
                         const MixerSyncConfig *sync,
                         mixer_alloc_fn alloc,
                         mixer_free_fn  free_fn);

/* Free everything the mixer allocated, including the mixer struct
 * itself. Safe to call with NULL. */
void mixer_destroy(AudioMixer *m);

/* ============================================================
 *  Channel control
 *
 *  All channel functions silently no-op on out-of-range indices.
 * ============================================================ */

void mixer_set_volume(AudioMixer *m, size_t channel, q15_t volume);
void mixer_set_pan(AudioMixer *m, size_t channel, q15_t pan);
void mixer_mute(AudioMixer *m, size_t channel, bool muted);

/* Update a channel's source sample rate. Recomputes the q32.32 step
 * and arms linear/cubic interpolation (per docs/audio-architecture.md
 * "per-channel source rate + linear/cubic interpolation"). Pass 0 to
 * disable resampling — the channel will treat each source frame as
 * one output frame, the legacy default. Resets interpolation taps so
 * the change takes effect cleanly on the next mixer_render. Typically
 * called before mixer_channel_reset + feeding a new sample. */
void mixer_set_source_rate(AudioMixer *m, size_t channel, uint32_t source_rate);

void mixer_channel_start(AudioMixer *m, size_t channel);
void mixer_channel_stop(AudioMixer *m, size_t channel);
void mixer_channel_reset(AudioMixer *m, size_t channel);

/* ============================================================
 *  Source data flow
 *
 *  Push samples into a channel's buffer. The buffer pointer's
 *  underlying type must match the channel's configured source
 *  format (see top-of-file vocabulary).
 *
 *  `count` is the number of samples for mono sources, or the
 *  number of frames (L+R pairs) for stereo sources.
 *
 *  Returns the number of samples/frames written.
 * ============================================================ */

size_t mixer_write_channel(AudioMixer *m, size_t channel,
                            const void *samples, size_t count);

/* Free frames in this channel's source ring (capacity - count). Lets
 * a caller (e.g. SYS_AUDIO_PCM_STREAM_FEED's handler) check before
 * writing so it can implement back-pressure — feed only what fits,
 * report the count actually fed, and let the guest retry the rest.
 * Returns 0 if `channel` is out of range or the channel has no ring. */
size_t mixer_channel_free_frames(const AudioMixer *m, size_t channel);

/* ============================================================
 *  Render
 *
 *  Render `frame_count` output frames. Each output frame contains
 *  out_format.channels samples (1 for mono, 2 for stereo).
 *
 *  Output buffer requirements:
 *    type: matches storage_bits and is_signed (see top-of-file)
 *    size: frame_count * channels * (storage_bits / 8) bytes
 * ============================================================ */

void mixer_render(AudioMixer *m, void *output, size_t frame_count);

/* ============================================================
 *  Sync feedback (only meaningful for sync-enabled mixers)
 * ============================================================ */

/* Report a pair of counter values: the mixer's internal frame count
 * (how many output frames have been rendered since some reference
 * point) and the external clock value at the same moment.
 *
 * For non-sync mixers, this is a no-op.
 *
 * Returns the current correction value in PPM (parts per million),
 * useful for diagnostics. Positive means the mixer is playing
 * faster than nominal; negative means slower. 0 for non-sync mixers
 * or before enough observations have been made. */
int32_t mixer_observe_sync(AudioMixer *m,
                            uint64_t internal_frames,
                            uint64_t external_ticks);

/* Reset the sync correction state. Useful when the external counter
 * resets (e.g., game level change). Sets correction to 0 and clears
 * the last-observation history.
 *
 * For non-sync mixers, this is a no-op. */
void mixer_reset_sync(AudioMixer *m);

/* Set the playback-rate correction DIRECTLY, in parts-per-million (positive =
 * play faster, negative = slower), bypassing the observe_sync drift estimator.
 * For callers that derive the correction from their own control loop — e.g. an
 * FMV A/V sync that steers playback to hold a producer ring at a steady fill
 * (a smooth signal), rather than from noisy clock-counter snapshots. Clamped to
 * the mixer's max_correction. No-op for non-sync mixers. */
void mixer_set_drift_ppm(AudioMixer *m, int32_t ppm);

/* ============================================================
 *  Introspection
 * ============================================================ */

/* Returns the channel's current source buffer fill, in samples
 * (mono) or frames (stereo). */
size_t mixer_channel_buffered(const AudioMixer *m, size_t channel);

/* Returns the channel's source buffer capacity (max fill), in the same
 * units as mixer_channel_buffered. Lets a feeder pump only the free
 * space (capacity - buffered) so it never overwrites unplayed audio. */
size_t mixer_channel_capacity(const AudioMixer *m, size_t channel);

#endif /* AUDIO_MIXER_H */
