/* ============================================================
 *  audio_mixer.c — stage 3 implementation
 *  See audio_mixer.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_mixer.h"
#include "containers/ring_buffer.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================
 *  Internal structures (private to this file)
 * ============================================================ */

/* ============================================================
 *  Internal structures (private to this file)
 *
 *  Stage 3 adds resampling state per channel: phase accumulator,
 *  step value, interpolation tap window, loop position. The
 *  needs_resample flag is set at create-time when source rate
 *  differs from output rate; channels matching the output rate
 *  skip all the resampling machinery and use a fast 1:1 path.
 * ============================================================ */

/* Tap window size — cubic needs 4, linear needs 2. Storage is the
 * max of the two so the same field works regardless of interp mode. */
#define MIXER_TAPS_MAX 4

typedef struct {
    RingBuffer         rb;        /* per-channel source ring buffer    */
    MixerSourceFormat  format;
    q15_t              volume;
    q15_t              pan;
    q15_t              pan_gain_l;     /* precomputed from pan */
    q15_t              pan_gain_r;
    bool               muted;
    bool               active;
    bool               loop;
    bool               needs_resample; /* true if source rate != output rate */
    MixerInterpolation interp;

    /* Phase accumulator (q32.32). Integer part is the source frame
     * index relative to the start of the lookahead window. Fractional
     * part is the sub-sample interpolation position. Only used when
     * needs_resample is true. */
    uint64_t           phase;
    uint64_t           base_step;  /* nominal step, set at create     */
    uint64_t           step;       /* current effective step (base * correction) */

    /* Interpolation tap window: up to 4 q15 samples per side (L, R).
     * The window slides as phase advances past integer boundaries.
     * For linear interpolation, only taps[0..1] are populated.
     * For cubic, taps[0..3] are populated.
     *
     * The window represents source samples at consecutive positions.
     * When phase integer part advances by N, we shift the window left
     * by N positions and load N new samples into the right side. */
    q15_t              tap_l[MIXER_TAPS_MAX];
    q15_t              tap_r[MIXER_TAPS_MAX];
    bool               tap_primed;     /* true after first fill */

    /* For loop mode: in addition to the ring buffer storing samples,
     * we track a read position separate from the ring buffer's tail.
     * Looping doesn't consume from the ring buffer — it just reads
     * with a wrapping position. */
    size_t             loop_read_pos;
} MixerChannel;

struct AudioMixer {
    /* Allocator stored here so destroy can free everything later. */
    mixer_alloc_fn  alloc;
    mixer_free_fn   free_fn;

    /* Owned allocations (need to be tracked for destroy). */
    MixerChannel   *channels;       /* one alloc for the array        */
    size_t          channel_count;
    void          **channel_buffers; /* per-channel storage allocs    */

    /* Configuration */
    int               sample_rate;
    MixerOutputFormat out_format;
    uint8_t           headroom_bits;

    /* Precomputed output-write params (computed once at create) */
    uint8_t   out_shift;
    int32_t   out_offset;
    int32_t   out_min;
    int32_t   out_max;

    /* Sync state (v2). sync_enabled is true when the mixer was
     * created with mixer_create_with_sync. */
    bool      sync_enabled;
    q15_t     sync_correction;       /* current smoothed correction, q15 (diag) */
    /* High-precision correction accumulator. The smoothing filter and the
     * per-channel step adjustment run on THIS, not the q15 sync_correction:
     * a slow drift (e.g. FMV's ~0.16%) needs a target correction of only
     * ~52 in q15 units, and `error * smoothing / Q15_ONE` rounds that to 0 —
     * the rate would never ramp in. Carrying SYNC_ACC_BITS extra fractional
     * bits lets the correction accumulate and apply in tiny increments.
     * Units: q15 << SYNC_ACC_BITS. */
    int64_t   sync_corr_acc;
    q15_t     sync_smoothing;        /* low-pass coefficient, q15        */
    q15_t     sync_max_correction;   /* clamp magnitude, q15             */
    uint64_t  sync_external_ticks_per_second;
    bool      sync_have_last_obs;    /* false until first observation    */
    uint64_t  sync_last_internal;
    uint64_t  sync_last_external;
};

/* ============================================================
 *  Source-format helpers
 * ============================================================ */

/* Number of channels in the source (1 for mono, 2 for stereo). */
static int source_channels(MixerSourceFormat fmt) {
    switch (fmt) {
        case MIXER_SRC_PCM16_MONO:
        case MIXER_SRC_PCM8S_MONO:
        case MIXER_SRC_PCM8U_MONO:    return 1;
        case MIXER_SRC_PCM16_STEREO:
        case MIXER_SRC_PCM8S_STEREO:
        case MIXER_SRC_PCM8U_STEREO:  return 2;
    }
    return 0;
}

/* Bytes per sample (single channel sample, not per frame). */
static size_t source_bytes_per_sample(MixerSourceFormat fmt) {
    switch (fmt) {
        case MIXER_SRC_PCM16_MONO:
        case MIXER_SRC_PCM16_STEREO:  return 2;
        case MIXER_SRC_PCM8S_MONO:
        case MIXER_SRC_PCM8U_MONO:
        case MIXER_SRC_PCM8S_STEREO:
        case MIXER_SRC_PCM8U_STEREO:  return 1;
    }
    return 0;
}

/* Bytes per "frame" — that is, bytes for one mono sample or one
 * L/R stereo pair. This is the ring buffer's element_size for
 * this format: each rb_push/rb_pop transfers exactly one frame. */
static size_t source_bytes_per_frame(MixerSourceFormat fmt) {
    return source_bytes_per_sample(fmt) * source_channels(fmt);
}

/* ============================================================
 *  Source-read helpers
 *
 *  Three operations on per-channel source data:
 *
 *    1. convert_bytes_to_q15: given raw frame bytes plus a format,
 *       produce q15 L/R values. The conversion is the same
 *       regardless of where the bytes came from (rb_pop, rb_peek_at,
 *       or direct buffer access for loop mode).
 *
 *    2. pop_source_frame_q15: rb_pop one frame, convert. Used for
 *       the non-resampling fast path and for advancing the tap
 *       window in non-loop resampling mode.
 *
 *    3. peek_source_frame_q15_at: rb_peek_at offset, convert. Used
 *       to look ahead without consuming.
 *
 *  For loop mode, samples come from the ring buffer's storage
 *  directly (since we don't consume) using a separate helper.
 *
 *  Conversion math:
 *    int16  → q15: pass through (already q15)
 *    int8   → q15: sign-extend, shift left 8
 *    uint8  → q15: subtract 128 (silence offset), shift left 8
 * ============================================================ */

static void convert_bytes_to_q15(MixerSourceFormat fmt, const void *bytes,
                                  q15_t *out_l, q15_t *out_r) {
    switch (fmt) {
        case MIXER_SRC_PCM16_MONO: {
            int16_t s = *(const int16_t *)bytes;
            *out_l = (q15_t)s; *out_r = (q15_t)s;
            return;
        }
        case MIXER_SRC_PCM8S_MONO: {
            int8_t s = *(const int8_t *)bytes;
            q15_t v = (q15_t)((int16_t)s << 8);
            *out_l = v; *out_r = v;
            return;
        }
        case MIXER_SRC_PCM8U_MONO: {
            uint8_t s = *(const uint8_t *)bytes;
            q15_t v = (q15_t)(((int16_t)s - 128) << 8);
            *out_l = v; *out_r = v;
            return;
        }
        case MIXER_SRC_PCM16_STEREO: {
            const int16_t *s = (const int16_t *)bytes;
            *out_l = (q15_t)s[0]; *out_r = (q15_t)s[1];
            return;
        }
        case MIXER_SRC_PCM8S_STEREO: {
            const int8_t *s = (const int8_t *)bytes;
            *out_l = (q15_t)((int16_t)s[0] << 8);
            *out_r = (q15_t)((int16_t)s[1] << 8);
            return;
        }
        case MIXER_SRC_PCM8U_STEREO: {
            const uint8_t *s = (const uint8_t *)bytes;
            *out_l = (q15_t)(((int16_t)s[0] - 128) << 8);
            *out_r = (q15_t)(((int16_t)s[1] - 128) << 8);
            return;
        }
    }
    *out_l = 0; *out_r = 0;
}

/* Pop one frame from the ring buffer, convert to q15 L/R. Returns 1
 * on success, 0 if empty (in which case both outputs are 0). */
static int pop_source_frame_q15(MixerChannel *c, q15_t *out_l, q15_t *out_r) {
    uint8_t frame_bytes[4];   /* max 4 bytes per frame (16-bit stereo) */
    if (!rb_pop(&c->rb, frame_bytes)) { *out_l = 0; *out_r = 0; return 0; }
    convert_bytes_to_q15(c->format, frame_bytes, out_l, out_r);
    return 1;
}

/* Peek at frame at the given offset without consuming. Returns 1
 * on success, 0 if offset is out of range. */
static int peek_source_frame_q15_at(const MixerChannel *c, size_t offset,
                                     q15_t *out_l, q15_t *out_r) {
    uint8_t frame_bytes[4];
    if (!rb_peek_at(&c->rb, offset, frame_bytes)) {
        *out_l = 0; *out_r = 0; return 0;
    }
    convert_bytes_to_q15(c->format, frame_bytes, out_l, out_r);
    return 1;
}

/* For loop mode: read directly from the ring buffer's storage at
 * the given frame position (modulo buffer fill). The ring buffer
 * isn't consumed. */
static void read_loop_frame_q15(const MixerChannel *c, size_t loop_pos,
                                 q15_t *out_l, q15_t *out_r) {
    if (c->rb.count == 0) { *out_l = 0; *out_r = 0; return; }
    size_t pos = loop_pos % c->rb.count;
    /* Translate this position into a slot within the ring buffer's
     * storage. The buffer's logical "front" is at rb.tail. */
    size_t slot = (c->rb.tail + pos) % c->rb.capacity;
    const uint8_t *bytes = (const uint8_t *)c->rb.storage
                            + slot * c->rb.element_size;
    convert_bytes_to_q15(c->format, bytes, out_l, out_r);
}

/* ============================================================
 *  Output-format derived params
 *
 *  After the int32 accumulator is shifted by headroom_bits and
 *  saturated to q15 range, we convert to the output format.
 *
 *  q15 has 16-bit signed range (including sign bit). To produce
 *  an N-bit signed output we shift right by (16 - N). For an N-bit
 *  unsigned output, we shift then add (1 << (bits-1)) to center.
 *
 *  out_shift  = 16 - bits
 *  out_offset = 0 if signed, 1 << (bits-1) if unsigned
 *  out_min    = 0 if unsigned, -(1 << (bits-1)) if signed
 *  out_max    = (1 << bits) - 1 if unsigned, (1 << (bits-1)) - 1 if signed
 * ============================================================ */

static int compute_output_params(MixerOutputFormat fmt,
                                  uint8_t *out_shift,
                                  int32_t *out_offset,
                                  int32_t *out_min,
                                  int32_t *out_max) {
    if (fmt.bits < 1 || fmt.bits > 16) return -1;
    if (fmt.storage_bits != 8 && fmt.storage_bits != 16) return -1;
    if (fmt.storage_bits == 8 && fmt.bits > 8) return -1;
    if (fmt.channels != 1 && fmt.channels != 2) return -1;

    *out_shift = (uint8_t)(16 - fmt.bits);

    if (fmt.is_signed) {
        *out_offset = 0;
        *out_min    = -((int32_t)1 << (fmt.bits - 1));
        *out_max    =  ((int32_t)1 << (fmt.bits - 1)) - 1;
    } else {
        *out_offset = (int32_t)1 << (fmt.bits - 1);
        *out_min    = 0;
        *out_max    = ((int32_t)1 << fmt.bits) - 1;
    }
    return 0;
}

/* ============================================================
 *  Saturation helper for the output stage
 * ============================================================ */

static inline int32_t finalize_output(int32_t accum,
                                       uint8_t headroom_bits,
                                       uint8_t out_shift,
                                       int32_t out_offset,
                                       int32_t out_min,
                                       int32_t out_max) {
    /* accum is at q15-with-headroom scale; first remove headroom. */
    int32_t v = accum >> headroom_bits;
    /* Saturate to q15 range to handle accumulated overshoot. */
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    /* Reduce to output resolution and apply unsigned offset. */
    v = (v >> out_shift) + out_offset;
    /* Final clamp to output's format range. */
    if (v > out_max) v = out_max;
    if (v < out_min) v = out_min;
    return v;
}

/* ============================================================
 *  Pan-gain precomputation
 *
 *  Linear panning law. q15 pan: -Q15_ONE = full L, 0 = center,
 *  +Q15_ONE = full R. Result gains are applied later via q15_sat_mul.
 *
 *  At center: l_gain = r_gain = Q15_ONE
 *  Full L:    l_gain = Q15_ONE, r_gain = 0
 *  Full R:    l_gain = 0,       r_gain = Q15_ONE
 * ============================================================ */

static void compute_pan_gains(q15_t pan, q15_t *l_gain, q15_t *r_gain) {
    if (pan >= 0) {
        *l_gain = (q15_t)(Q15_ONE - pan);
        *r_gain = Q15_ONE;
    } else {
        *l_gain = Q15_ONE;
        *r_gain = (q15_t)(Q15_ONE + pan);
    }
}

/* ============================================================
 *  Resampling-state setup
 *
 *  step = source_rate / output_rate, expressed in q32.32.
 *  Computed at create time from configured rates.
 *
 *  For source_rate == output_rate (or source_rate == 0), the
 *  channel skips resampling entirely.
 * ============================================================ */

static uint64_t compute_step_q32_32(int source_rate, int output_rate) {
    /* step = (source_rate * 2^32) / output_rate
     * Done in 64-bit to avoid overflow. */
    uint64_t num = (uint64_t)source_rate << 32;
    return num / (uint64_t)output_rate;
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* ============================================================
 *  Internal create — accepts an optional sync config
 * ============================================================ */

static AudioMixer *mixer_create_internal(
        const MixerChannelConfig *channels,
        size_t channel_count,
        int output_sample_rate,
        MixerOutputFormat out_format,
        uint8_t headroom_bits,
        const MixerSyncConfig *sync,
        mixer_alloc_fn alloc,
        mixer_free_fn  free_fn) {
    if (!alloc)   alloc   = malloc;
    if (!free_fn) free_fn = free;

    /* Validate the output format before touching the allocator. */
    uint8_t out_shift;
    int32_t out_offset, out_min, out_max;
    if (compute_output_params(out_format, &out_shift,
                              &out_offset, &out_min, &out_max) != 0) {
        return NULL;
    }

    /* Validate sync config if provided. */
    if (sync) {
        if (sync->external_ticks_per_second == 0) return NULL;
        if (sync->correction_smoothing < 0) return NULL;
        if (sync->max_correction < 0) return NULL;
    }

    /* Allocate the mixer struct. */
    AudioMixer *m = alloc(sizeof *m);
    if (!m) return NULL;
    memset(m, 0, sizeof *m);

    m->alloc         = alloc;
    m->free_fn       = free_fn;
    m->sample_rate   = output_sample_rate;
    m->out_format    = out_format;
    m->headroom_bits = headroom_bits;
    m->out_shift     = out_shift;
    m->out_offset    = out_offset;
    m->out_min       = out_min;
    m->out_max       = out_max;
    m->channel_count = channel_count;

    if (sync) {
        m->sync_enabled                    = true;
        m->sync_correction                 = 0;
        m->sync_corr_acc                   = 0;
        m->sync_smoothing                  = sync->correction_smoothing;
        m->sync_max_correction             = sync->max_correction;
        m->sync_external_ticks_per_second  = sync->external_ticks_per_second;
        m->sync_have_last_obs              = false;
        m->sync_last_internal              = 0;
        m->sync_last_external              = 0;
    }

    /* Allocate the channel array. */
    if (channel_count > 0) {
        m->channels = alloc(channel_count * sizeof(MixerChannel));
        if (!m->channels) {
            free_fn(m);
            return NULL;
        }
        memset(m->channels, 0, channel_count * sizeof(MixerChannel));

        /* Allocate the buffer-pointer array for destroy bookkeeping. */
        m->channel_buffers = alloc(channel_count * sizeof(void *));
        if (!m->channel_buffers) {
            free_fn(m->channels);
            free_fn(m);
            return NULL;
        }
        memset(m->channel_buffers, 0, channel_count * sizeof(void *));
    }

    /* Allocate each channel's source buffer and init the ring buffer. */
    for (size_t i = 0; i < channel_count; i++) {
        size_t frame_bytes = source_bytes_per_frame(channels[i].format);
        size_t buf_bytes   = channels[i].buffer_samples * frame_bytes;
        void *buf = alloc(buf_bytes);
        if (!buf) {
            for (size_t j = 0; j < i; j++) {
                free_fn(m->channel_buffers[j]);
            }
            free_fn(m->channel_buffers);
            free_fn(m->channels);
            free_fn(m);
            return NULL;
        }
        m->channel_buffers[i] = buf;

        MixerChannel *c = &m->channels[i];
        rb_init(&c->rb, buf, channels[i].buffer_samples, frame_bytes);
        c->format = channels[i].format;
        c->volume = channels[i].volume;
        c->pan    = channels[i].pan;
        compute_pan_gains(c->pan, &c->pan_gain_l, &c->pan_gain_r);
        c->muted  = false;
        c->active = false;
        c->loop   = channels[i].loop;
        c->interp = channels[i].interp;

        /* Set up resampling state.
         *
         * Without sync: needs_resample is true iff source rate differs
         * from output rate.
         *
         * With sync (option A): ALL channels go through the resampling
         * path so the correction applies uniformly. Channels at the
         * native rate get base_step = 1.0; the runtime correction
         * adjusts it. */
        int src_rate = channels[i].source_sample_rate;
        if (sync) {
            /* All channels resample under sync. */
            c->needs_resample = true;
            c->phase = 0;
            if (src_rate > 0 && src_rate != output_sample_rate) {
                c->base_step = compute_step_q32_32(src_rate, output_sample_rate);
            } else {
                c->base_step = (uint64_t)1 << 32;   /* 1.0 in q32.32 */
            }
            c->step = c->base_step;   /* correction starts at 0 */
        } else {
            if (src_rate <= 0 || src_rate == output_sample_rate) {
                c->needs_resample = false;
                c->phase = 0;
                c->base_step = 0;
                c->step      = 0;
            } else {
                c->needs_resample = true;
                c->phase = 0;
                c->base_step = compute_step_q32_32(src_rate, output_sample_rate);
                c->step      = c->base_step;
            }
        }
        for (int t = 0; t < MIXER_TAPS_MAX; t++) {
            c->tap_l[t] = 0;
            c->tap_r[t] = 0;
        }
        c->tap_primed = false;
        c->loop_read_pos = 0;
    }

    return m;
}

AudioMixer *mixer_create(const MixerChannelConfig *channels,
                         size_t channel_count,
                         int output_sample_rate,
                         MixerOutputFormat out_format,
                         uint8_t headroom_bits) {
    return mixer_create_internal(channels, channel_count,
                                  output_sample_rate, out_format,
                                  headroom_bits, NULL,
                                  malloc, free);
}

AudioMixer *mixer_create_with_allocator(
                         const MixerChannelConfig *channels,
                         size_t channel_count,
                         int output_sample_rate,
                         MixerOutputFormat out_format,
                         uint8_t headroom_bits,
                         mixer_alloc_fn alloc,
                         mixer_free_fn  free_fn) {
    return mixer_create_internal(channels, channel_count,
                                  output_sample_rate, out_format,
                                  headroom_bits, NULL,
                                  alloc, free_fn);
}

AudioMixer *mixer_create_with_sync(
                         const MixerChannelConfig *channels,
                         size_t channel_count,
                         int output_sample_rate,
                         MixerOutputFormat out_format,
                         uint8_t headroom_bits,
                         const MixerSyncConfig *sync,
                         mixer_alloc_fn alloc,
                         mixer_free_fn  free_fn) {
    if (!sync) return NULL;   /* the sync-create variant requires it */
    return mixer_create_internal(channels, channel_count,
                                  output_sample_rate, out_format,
                                  headroom_bits, sync,
                                  alloc, free_fn);
}

void mixer_destroy(AudioMixer *m) {
    if (!m) return;

    if (m->channel_buffers) {
        for (size_t i = 0; i < m->channel_count; i++) {
            if (m->channel_buffers[i]) {
                m->free_fn(m->channel_buffers[i]);
            }
        }
        m->free_fn(m->channel_buffers);
    }
    if (m->channels) {
        m->free_fn(m->channels);
    }
    m->free_fn(m);
}

/* ============================================================
 *  Channel control
 * ============================================================ */

void mixer_set_volume(AudioMixer *m, size_t channel, q15_t volume) {
    if (channel >= m->channel_count) return;
    m->channels[channel].volume = volume;
}

void mixer_set_pan(AudioMixer *m, size_t channel, q15_t pan) {
    if (channel >= m->channel_count) return;
    MixerChannel *c = &m->channels[channel];
    c->pan = pan;
    compute_pan_gains(pan, &c->pan_gain_l, &c->pan_gain_r);
}

void mixer_mute(AudioMixer *m, size_t channel, bool muted) {
    if (channel >= m->channel_count) return;
    m->channels[channel].muted = muted;
}

void mixer_set_source_rate(AudioMixer *m, size_t channel, uint32_t source_rate) {
    if (!m || channel >= m->channel_count) return;
    MixerChannel *c = &m->channels[channel];
    /* source_rate == 0 or == output_rate: identity step, no resample.
     * Otherwise compute the q32.32 step and arm the interpolator.
     * Reset phase + interpolation taps so the new rate kicks in cleanly
     * at the next sample fed to the channel — callers typically pair
     * this with mixer_channel_reset before feeding a new sample. */
    if (m->sync_enabled) {
        /* Under sync (drift correction) ALL channels stay on the
         * resampling path so the runtime step correction applies to
         * them. Native-rate channels get base_step = 1.0; the next
         * mixer_observe_sync trims step around it. Otherwise a 44.1 kHz
         * PCM stream would be flagged passthrough (step=0) and the
         * drift correction could never reach it. */
        c->needs_resample = true;
        if (source_rate == 0 || (int)source_rate == m->sample_rate) {
            c->base_step = (uint64_t)1 << 32;          /* 1.0 in q32.32 */
        } else {
            c->base_step = compute_step_q32_32((int)source_rate,
                                                m->sample_rate);
        }
        c->step = c->base_step;   /* correction reapplied on next observe */
    } else if (source_rate == 0 || (int)source_rate == m->sample_rate) {
        c->needs_resample = false;
        c->base_step      = 0;
        c->step           = 0;
    } else {
        c->needs_resample = true;
        c->base_step      = compute_step_q32_32((int)source_rate,
                                                 m->sample_rate);
        c->step           = c->base_step;
    }
    c->phase = 0;
    for (int t = 0; t < MIXER_TAPS_MAX; t++) {
        c->tap_l[t] = 0;
        c->tap_r[t] = 0;
    }
    c->tap_primed = false;
}

void mixer_channel_start(AudioMixer *m, size_t channel) {
    if (channel >= m->channel_count) return;
    MixerChannel *c = &m->channels[channel];
    c->active = true;
    /* Reset resampling state so playback starts cleanly from the
     * current buffer contents. Phase = 0, tap window not yet primed. */
    c->phase         = 0;
    c->tap_primed    = false;
    c->loop_read_pos = 0;
}

void mixer_channel_stop(AudioMixer *m, size_t channel) {
    if (channel >= m->channel_count) return;
    m->channels[channel].active = false;
}

void mixer_channel_reset(AudioMixer *m, size_t channel) {
    if (channel >= m->channel_count) return;
    MixerChannel *c = &m->channels[channel];
    c->active = false;
    rb_reset(&c->rb);
    c->phase         = 0;
    c->tap_primed    = false;
    c->loop_read_pos = 0;
}

/* ============================================================
 *  Source data flow
 *
 *  The caller passes a buffer typed appropriately for the
 *  channel's source format. We push frame-by-frame into the ring
 *  buffer. The ring buffer's element_size matches the frame size
 *  (1 byte for 8-bit mono, 2 for 8-bit stereo or 16-bit mono,
 *  4 for 16-bit stereo), so each rb_push copies one whole frame.
 * ============================================================ */

size_t mixer_write_channel(AudioMixer *m, size_t channel,
                            const void *samples, size_t count) {
    if (channel >= m->channel_count) return 0;
    MixerChannel *c = &m->channels[channel];
    size_t frame_bytes = source_bytes_per_frame(c->format);
    const uint8_t *bytes = (const uint8_t *)samples;
    for (size_t i = 0; i < count; i++) {
        rb_push(&c->rb, bytes + i * frame_bytes);
    }
    return count;
}

size_t mixer_channel_free_frames(const AudioMixer *m, size_t channel) {
    if (!m || channel >= m->channel_count) return 0;
    const MixerChannel *c = &m->channels[channel];
    if (c->rb.capacity == 0) return 0;
    /* rb_push overwrites on full, so "free" is the capacity-minus-count
     * delta — the number of frames that can be pushed without
     * displacing already-queued data. Callers (PCM-stream FEED) use
     * this for back-pressure. */
    if (c->rb.count >= c->rb.capacity) return 0;
    return c->rb.capacity - c->rb.count;
}

/* ============================================================
 *  Tap window management for interpolation
 *
 *  Window layout:
 *
 *    LINEAR: taps[0..1] hold consecutive source samples. The
 *            fractional phase in [0,1) interpolates between
 *            tap[0] (current) and tap[1] (next).
 *
 *    CUBIC:  taps[0..3] hold consecutive source samples. The
 *            fractional phase in [0,1) interpolates between
 *            tap[1] (current) and tap[2] (next). tap[0] is one
 *            sample of history; tap[3] is one sample lookahead.
 *            At the start of playback, tap[0] is zero (silence)
 *            since no history exists yet.
 *
 *  Phase advance: when phase's integer part increases by N, the
 *  window shifts left by N positions and N new source samples
 *  are loaded into the right side.
 * ============================================================ */

static int taps_for_interp(MixerInterpolation interp) {
    return (interp == MIXER_INTERP_CUBIC) ? 4 : 2;
}

/* The current-sample tap index (interpolating between this tap and
 * the next). For linear: 0. For cubic: 1. */
static int current_tap_index(MixerInterpolation interp) {
    return (interp == MIXER_INTERP_CUBIC) ? 1 : 0;
}

/* Load a single source sample into tap[t] from offset `src_off`.
 * For loop mode, src_off is the position within the loop buffer.
 * For non-loop, it's the peek offset into the ring buffer. */
static void load_tap(MixerChannel *c, int t, size_t src_off) {
    q15_t l, r;
    if (c->loop) {
        read_loop_frame_q15(c, c->loop_read_pos + src_off, &l, &r);
    } else {
        if (!peek_source_frame_q15_at(c, src_off, &l, &r)) {
            l = 0; r = 0;
        }
    }
    c->tap_l[t] = l;
    c->tap_r[t] = r;
}

/* Initial fill of the tap window when starting/resuming a channel.
 * For cubic, tap[0] is set to silence (no history at start). */
static void prime_tap_window(MixerChannel *c) {
    int n_taps  = taps_for_interp(c->interp);
    int cur_idx = current_tap_index(c->interp);

    /* Zero everything first. */
    for (int t = 0; t < MIXER_TAPS_MAX; t++) {
        c->tap_l[t] = 0;
        c->tap_r[t] = 0;
    }
    /* Load taps[cur_idx..n_taps-1] from source positions 0..(n_taps-1-cur_idx). */
    for (int t = cur_idx; t < n_taps; t++) {
        load_tap(c, t, (size_t)(t - cur_idx));
    }
    c->tap_primed = true;
}

/* Advance the tap window by n_advance integer positions.
 *
 * Non-loop: pops n_advance frames from the ring buffer, then peeks
 * forward to fill the right-side taps with the new samples after
 * the advance.
 *
 * Loop: just advances loop_read_pos. The taps are then re-read from
 * the loop buffer at the new positions. */
static void advance_tap_window(MixerChannel *c, int n_advance) {
    int n_taps  = taps_for_interp(c->interp);
    int cur_idx = current_tap_index(c->interp);
    if (n_advance <= 0) return;
    /* Clamp to a full window refresh — anything more would be wasted
     * shifts. */
    if (n_advance >= n_taps) n_advance = n_taps;

    /* Shift existing taps left. */
    for (int t = 0; t < n_taps - n_advance; t++) {
        c->tap_l[t] = c->tap_l[t + n_advance];
        c->tap_r[t] = c->tap_r[t + n_advance];
    }

    if (c->loop) {
        c->loop_read_pos += (size_t)n_advance;
        /* Re-load right-side taps from the new loop position. */
        for (int t = n_taps - n_advance; t < n_taps; t++) {
            load_tap(c, t, (size_t)(t - cur_idx));
        }
    } else {
        /* Consume n_advance frames from the ring buffer. */
        uint8_t scratch[4];
        for (int i = 0; i < n_advance; i++) {
            (void)rb_pop(&c->rb, scratch);
        }
        /* Refill the right-side taps from the new buffer positions. */
        for (int t = n_taps - n_advance; t < n_taps; t++) {
            load_tap(c, t, (size_t)(t - cur_idx));
        }
    }
}

/* ============================================================
 *  Interpolation kernels
 *
 *  Both kernels take the current tap window and the fractional
 *  position from phase (q32.32 → low 32 bits represent [0, 1) in q32
 *  scaling). They return one interpolated q15 sample.
 * ============================================================ */

/* Linear: result = a + frac * (b - a).
 * We convert the q32 fractional to q16.16 internally for the math. */
static q15_t interp_linear_q15(q15_t a, q15_t b, uint32_t frac_q32) {
    uint32_t f = frac_q32 >> 16;   /* now q16: 0..0xFFFF */
    int32_t diff = (int32_t)b - (int32_t)a;
    /* diff (q15) * f (q16) → q31; shift back to q15 via >> 16. */
    int32_t scaled = (diff * (int32_t)f) >> 16;
    return (q15_t)((int32_t)a + scaled);
}

/* Catmull-Rom cubic interpolation. 4-point cubic Hermite spline.
 *
 *   p(t) = 0.5 * ( 2*p1
 *                + (-p0 + p2) * t
 *                + (2*p0 - 5*p1 + 4*p2 - p3) * t^2
 *                + (-p0 + 3*p1 - 3*p2 + p3) * t^3 )
 *
 * where p0..p3 are taps and t is the fractional position in [0, 1)
 * interpolating between p1 (current) and p2 (next).
 *
 * Math done in int32 with q15 intermediates; result saturated. */
static q15_t interp_cubic_q15(q15_t p0, q15_t p1, q15_t p2, q15_t p3,
                               uint32_t frac_q32) {
    /* Convert to q15 fractional (top 15 bits of frac). */
    int32_t t = (int32_t)(frac_q32 >> 17);   /* 0..0x7FFF */

    int32_t t2 = (t * t) >> 15;
    int32_t t3 = (t2 * t) >> 15;

    int32_t a = -(int32_t)p0 +  3*(int32_t)p1 - 3*(int32_t)p2 + (int32_t)p3;
    int32_t b =  2*(int32_t)p0 - 5*(int32_t)p1 + 4*(int32_t)p2 -    (int32_t)p3;
    int32_t c = -(int32_t)p0                     + (int32_t)p2;
    int32_t d =  2*(int32_t)p1;

    int32_t v = (a * t3) >> 15;
    v       += (b * t2) >> 15;
    v       += (c * t)  >> 15;
    v       += d;

    /* The formula above is doubled (0.5 factored out). Halve. */
    v >>= 1;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (q15_t)v;
}

/* ============================================================
 *  Per-channel sample production
 *
 *  Three internal paths based on channel state:
 *    - Fast (no resample, no loop):  pop one frame from ring buffer.
 *    - Resampling (no loop):         tap-window with consume.
 *    - Loop (with or without resample): tap-window with wrap.
 *
 *  Returns 1 on success, 0 on underrun (outputs set to 0).
 * ============================================================ */

static int produce_channel_sample(MixerChannel *c, q15_t *out_l, q15_t *out_r) {
    /* Fast path: no resample, no loop. */
    if (!c->needs_resample && !c->loop) {
        return pop_source_frame_q15(c, out_l, out_r);
    }

    /* Slow path: tap window + interpolation. Need at least one frame
     * in the buffer before we can produce anything. */
    if (rb_count(&c->rb) == 0 && !c->tap_primed) {
        *out_l = 0; *out_r = 0;
        return 0;
    }
    if (!c->tap_primed) {
        prime_tap_window(c);
    }

    /* Interpolate using the current tap window and fractional phase. */
    uint32_t frac = (uint32_t)(c->phase & 0xFFFFFFFFu);
    int cur = current_tap_index(c->interp);

    if (c->interp == MIXER_INTERP_CUBIC) {
        *out_l = interp_cubic_q15(c->tap_l[0], c->tap_l[1],
                                   c->tap_l[2], c->tap_l[3], frac);
        *out_r = interp_cubic_q15(c->tap_r[0], c->tap_r[1],
                                   c->tap_r[2], c->tap_r[3], frac);
    } else {
        *out_l = interp_linear_q15(c->tap_l[cur], c->tap_l[cur+1], frac);
        *out_r = interp_linear_q15(c->tap_r[cur], c->tap_r[cur+1], frac);
    }

    /* Advance phase. Use step for resampling, else 1.0 exact. */
    uint64_t step = c->needs_resample ? c->step : ((uint64_t)1 << 32);
    uint64_t old_phase = c->phase;
    c->phase += step;

    uint32_t old_int = (uint32_t)(old_phase >> 32);
    uint32_t new_int = (uint32_t)(c->phase  >> 32);
    int n_advance = (int)(new_int - old_int);

    if (n_advance > 0) {
        advance_tap_window(c, n_advance);
        /* Keep phase's integer part at 0 so it doesn't grow unbounded.
         * Fractional part is preserved. */
        c->phase -= ((uint64_t)n_advance << 32);
    }
    return 1;
}

/* ============================================================
 *  Render — the hot loop
 *
 *  For each output frame:
 *    1. Walk every channel. Produce one (L, R) sample per channel
 *       via produce_channel_sample, which handles fast/resample/loop.
 *    2. Apply per-channel volume and pan.
 *    3. Sum into int32 L and R accumulators.
 *    4. Apply headroom and output format, write to output buffer.
 * ============================================================ */

void mixer_render(AudioMixer *m, void *output, size_t frame_count) {
    const uint8_t out_channels = m->out_format.channels;
    const uint8_t storage_bits = m->out_format.storage_bits;

    for (size_t f = 0; f < frame_count; f++) {
        int32_t accum_l = 0;
        int32_t accum_r = 0;

        for (size_t i = 0; i < m->channel_count; i++) {
            MixerChannel *c = &m->channels[i];

            if (!c->active) continue;

            q15_t l, r;
            int got = produce_channel_sample(c, &l, &r);

            if (c->muted || !got) {
                continue;
            }

            /* Apply volume. Unity bypass avoids the 1-LSB loss from
             * multiplying by Q15_ONE (which is 0x7FFF, not 1.0). */
            if (c->volume != Q15_ONE) {
                l = q15_sat_mul(l, c->volume);
                r = q15_sat_mul(r, c->volume);
            }

            /* Apply pan. Unity bypass on each side if its gain is
             * Q15_ONE (covers the pan == 0 center case for free). */
            if (c->pan_gain_l != Q15_ONE) l = q15_sat_mul(l, c->pan_gain_l);
            if (c->pan_gain_r != Q15_ONE) r = q15_sat_mul(r, c->pan_gain_r);

            accum_l += l;
            accum_r += r;
        }

        /* Convert to output. For mono output, average L and R. */
        int32_t out_v0, out_v1;
        if (out_channels == 1) {
            int32_t mono = (accum_l + accum_r) / 2;
            out_v0 = finalize_output(mono, m->headroom_bits,
                                     m->out_shift, m->out_offset,
                                     m->out_min, m->out_max);
            out_v1 = 0;
        } else {
            out_v0 = finalize_output(accum_l, m->headroom_bits,
                                     m->out_shift, m->out_offset,
                                     m->out_min, m->out_max);
            out_v1 = finalize_output(accum_r, m->headroom_bits,
                                     m->out_shift, m->out_offset,
                                     m->out_min, m->out_max);
        }

        /* Write to output buffer. */
        if (storage_bits == 8) {
            uint8_t *out8 = (uint8_t *)output;
            out8[f * out_channels] = (uint8_t)out_v0;
            if (out_channels == 2) out8[f * 2 + 1] = (uint8_t)out_v1;
        } else {
            uint16_t *out16 = (uint16_t *)output;
            out16[f * out_channels] = (uint16_t)out_v0;
            if (out_channels == 2) out16[f * 2 + 1] = (uint16_t)out_v1;
        }
    }
}

/* ============================================================
 *  Introspection
 * ============================================================ */

size_t mixer_channel_buffered(const AudioMixer *m, size_t channel) {
    if (channel >= m->channel_count) return 0;
    return rb_count(&m->channels[channel].rb);
}

size_t mixer_channel_capacity(const AudioMixer *m, size_t channel) {
    if (channel >= m->channel_count) return 0;
    return m->channels[channel].rb.capacity;
}

/* ============================================================
 *  Sync (v2) — observation and reset
 *
 *  Drift correction is an exponentially-smoothed low-pass filter
 *  on the observed drift between clocks. Each observation:
 *
 *    1. Compute deltas: how much the internal and external counters
 *       advanced since the last observation.
 *    2. Convert delta_external - expected_external to a fractional
 *       rate error (q15).
 *    3. Pull the current smoothed correction toward this raw value
 *       by a fraction (smoothing coefficient).
 *    4. Clamp to ±max_correction.
 *    5. Recompute each resampling channel's step.
 *
 *  With constant drift, the correction converges to a steady state
 *  matching the true drift. The mixer's effective playback rate
 *  becomes base_step * (1 + correction), keeping pace with the
 *  external clock.
 *
 *  Note that the correction reflects clock drift between the
 *  observer's clocks, not residual after correction — adjusting
 *  playback rate doesn't affect the external counter's ticking
 *  pattern, so each new observation still sees the true drift.
 *  The smoothing pulls toward the observed value rather than
 *  accumulating, which is the correct behavior for this feedback
 *  shape.
 *
 *  Skipping observations with zero internal_delta avoids division
 *  by zero in degenerate cases.
 * ============================================================ */

/* Extra fractional bits the correction accumulator carries beyond q15, so a
 * slow drift can ramp in/apply in increments smaller than a q15 unit (which
 * the per-observation smoothing would otherwise round to zero). */
#define SYNC_ACC_BITS 16
#define SYNC_ACC_ONE  ((int64_t)Q15_ONE << SYNC_ACC_BITS)

static void update_channel_steps(AudioMixer *m) {
    /* Recompute c->step for every resampling channel from c->base_step
     * and the current correction value. */
    for (size_t i = 0; i < m->channel_count; i++) {
        MixerChannel *c = &m->channels[i];
        if (!c->needs_resample) continue;

        /* delta = base_step * corr_acc / SYNC_ACC_ONE. Using the high-precision
         * accumulator (not the q15 sync_correction) means the step adjusts in
         * tiny continuous increments instead of 30-PPM q15 jumps. base_step is
         * up to a few × 2^32 and corr_acc is clamped to ~2% × 2^16, so the
         * product stays well within int64. */
        int64_t delta = (int64_t)c->base_step * m->sync_corr_acc;
        delta /= SYNC_ACC_ONE;
        int64_t new_step = (int64_t)c->base_step + delta;
        if (new_step < 1) new_step = 1;   /* never go negative or zero */
        c->step = (uint64_t)new_step;
    }
}

int32_t mixer_observe_sync(AudioMixer *m,
                            uint64_t internal_frames,
                            uint64_t external_ticks) {
    if (!m || !m->sync_enabled) return 0;

    /* First observation: stash baseline, no correction update. */
    if (!m->sync_have_last_obs) {
        m->sync_last_internal = internal_frames;
        m->sync_last_external = external_ticks;
        m->sync_have_last_obs = true;
        return 0;
    }

    /* Compute deltas. Use signed arithmetic for safety even though
     * counters should only increase. */
    int64_t delta_internal = (int64_t)internal_frames - (int64_t)m->sync_last_internal;
    int64_t delta_external = (int64_t)external_ticks  - (int64_t)m->sync_last_external;

    /* Stash for next call. */
    m->sync_last_internal = internal_frames;
    m->sync_last_external = external_ticks;

    if (delta_internal <= 0) return 0;   /* skip degenerate observations */

    /* Expected external ticks for this delta_internal. */
    int64_t expected_external =
        (int64_t)(delta_internal * (int64_t)m->sync_external_ticks_per_second
                  / (int64_t)m->sample_rate);
    if (expected_external <= 0) return 0;

    int64_t drift = delta_external - expected_external;

    /* Reject phase-jitter outliers. internal_frames and external_ticks come
     * from two free-running clocks/threads; a single observation can sample
     * them mid-burst, so its implied rate error swings far past any real clock
     * drift (which is well under 1%). Those swings, fed to the filter, make the
     * playback rate warble. Real drift is small and persistent, so it survives
     * many observations — discard any whose drift exceeds ~6% of expected and
     * let the clean ones through. (Also covers the delta_external==0 "internal
     * ran ahead of external" case, which would otherwise read as -100%.) */
    int64_t reject = expected_external / 16;   /* ~6.25% */
    if (drift > reject || drift < -reject)
        return (int32_t)(m->sync_corr_acc * 1000000 / SYNC_ACC_ONE);

    /* Convert drift to a high-precision fractional rate error (q15 << ACC).
     *   raw = drift * SYNC_ACC_ONE / expected_external
     * Sign matters: positive drift = external advanced more than expected =
     * external clock is faster = mixer needs to play faster too. Computing in
     * the accumulator's units (not q15) preserves sub-q15 drift so the loop
     * doesn't quantize a slow drift to zero. */
    int64_t raw_correction = (drift * SYNC_ACC_ONE) / expected_external;

    /* Exponential smoothing (low-pass filter) toward the raw value, in the
     * high-precision accumulator:
     *   new = old + smoothing * (raw - old)
     * With constant drift, this converges to a steady state where new == raw.
     * Smoothing in (0, Q15_ONE]: smaller = slower convergence, smoother. */
    int64_t error = raw_correction - m->sync_corr_acc;
    int64_t step  = (error * (int64_t)m->sync_smoothing) / Q15_ONE;
    int64_t new_acc = m->sync_corr_acc + step;

    /* Clamp to max magnitude (q15 max promoted to accumulator units). */
    int64_t max_acc = (int64_t)m->sync_max_correction << SYNC_ACC_BITS;
    if (new_acc >  max_acc) new_acc =  max_acc;
    if (new_acc < -max_acc) new_acc = -max_acc;

    m->sync_corr_acc   = new_acc;
    m->sync_correction = (q15_t)(new_acc >> SYNC_ACC_BITS);   /* diag/inspection */
    update_channel_steps(m);

    /* Return PPM (parts per million) for diagnostics, from the full-precision
     * accumulator. 1.0 == SYNC_ACC_ONE, so PPM = acc * 1e6 / SYNC_ACC_ONE. */
    return (int32_t)(m->sync_corr_acc * 1000000 / SYNC_ACC_ONE);
}

void mixer_reset_sync(AudioMixer *m) {
    if (!m || !m->sync_enabled) return;
    m->sync_correction    = 0;
    m->sync_corr_acc      = 0;
    m->sync_have_last_obs = false;
    m->sync_last_internal = 0;
    m->sync_last_external = 0;
    update_channel_steps(m);   /* resets c->step to c->base_step everywhere */
}

void mixer_set_drift_ppm(AudioMixer *m, int32_t ppm) {
    if (!m || !m->sync_enabled) return;
    /* ppm -> accumulator units (q15 << SYNC_ACC_BITS). */
    int64_t acc = (int64_t)ppm * SYNC_ACC_ONE / 1000000;
    int64_t max_acc = (int64_t)m->sync_max_correction << SYNC_ACC_BITS;
    if (acc >  max_acc) acc =  max_acc;
    if (acc < -max_acc) acc = -max_acc;
    m->sync_corr_acc   = acc;
    m->sync_correction = (q15_t)(acc >> SYNC_ACC_BITS);
    update_channel_steps(m);
}
