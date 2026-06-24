/* ============================================================
 *  music_player.c — intro-and-loop music playback driver
 *  See music_player.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/music_player.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================
 *  Helpers — same shape as audio_mixer's internal helpers,
 *  duplicated here so we don't have to expose them.
 * ============================================================ */

static size_t fmt_bytes_per_frame(MixerSourceFormat fmt) {
    switch (fmt) {
        case MIXER_SRC_PCM16_MONO:    return 2;
        case MIXER_SRC_PCM8S_MONO:    return 1;
        case MIXER_SRC_PCM8U_MONO:    return 1;
        case MIXER_SRC_PCM16_STEREO:  return 4;
        case MIXER_SRC_PCM8S_STEREO:  return 2;
        case MIXER_SRC_PCM8U_STEREO:  return 2;
    }
    return 0;
}

/* ============================================================
 *  Internal state
 * ============================================================ */

typedef enum {
    SEG_NONE,
    SEG_INTRO,
    SEG_LOOP,
} CurrentSegment;

struct MusicPlayer {
    /* Configuration (mostly copy of MusicPlayerConfig). */
    MusicPlayerConfig cfg;

    /* Allocator (for destroy). */
    mixer_alloc_fn alloc;
    mixer_free_fn  free_fn;

    /* Public state. */
    MusicState state;

    /* Which segment we're playing right now (or were, when paused). */
    CurrentSegment segment;

    /* Absolute sample offset within the current segment. This is
     * what we pass to the stream callback when we need more data. */
    size_t segment_offset;

    /* How many pinned-head samples remain to be delivered to the
     * mixer at the start of the current segment. When this hits 0
     * we switch to pulling from the streaming buffer. */
    size_t pinned_remaining;

    /* Streaming buffer state. */
    size_t streaming_count;       /* samples currently in streaming_buffer */
    size_t streaming_read_pos;    /* read position (samples from start)    */

    /* Cached info derived from config. */
    size_t bytes_per_frame;
};

/* ============================================================
 *  Lifecycle
 * ============================================================ */

static int validate_config(const MusicPlayerConfig *cfg) {
    if (!cfg) return -1;
    if (!cfg->mixer) return -1;
    if (!cfg->stream_fn) return -1;
    if (!cfg->streaming_buffer || cfg->streaming_buffer_samples == 0) return -1;
    /* At least one of intro/loop must be configured. */
    if (cfg->intro_stream_id == MUSIC_STREAM_NONE &&
        cfg->loop_stream_id  == MUSIC_STREAM_NONE) return -1;
    /* If a pinned head buffer is given, samples must be > 0 too. */
    if (cfg->intro_head_buffer && cfg->intro_head_samples == 0) return -1;
    if (cfg->loop_head_buffer  && cfg->loop_head_samples  == 0) return -1;
    /* Pinned head can't exceed streaming buffer size (we'd never be
     * able to refill efficiently otherwise). Soft check — keep
     * generous. */
    if (cfg->intro_head_samples > cfg->streaming_buffer_samples) return -1;
    if (cfg->loop_head_samples  > cfg->streaming_buffer_samples) return -1;
    return 0;
}

MusicPlayer *music_create(const MusicPlayerConfig *cfg) {
    return music_create_with_allocator(cfg, malloc, free);
}

MusicPlayer *music_create_with_allocator(const MusicPlayerConfig *cfg,
                                          mixer_alloc_fn alloc,
                                          mixer_free_fn  free_fn) {
    if (!alloc)   alloc   = malloc;
    if (!free_fn) free_fn = free;

    if (validate_config(cfg) != 0) return NULL;

    MusicPlayer *mp = alloc(sizeof *mp);
    if (!mp) return NULL;
    memset(mp, 0, sizeof *mp);

    mp->cfg             = *cfg;
    mp->alloc           = alloc;
    mp->free_fn         = free_fn;
    mp->state           = MUSIC_STOPPED;
    mp->segment         = SEG_NONE;
    mp->bytes_per_frame = fmt_bytes_per_frame(cfg->format);

    return mp;
}

void music_destroy(MusicPlayer *mp) {
    if (!mp) return;
    /* Leave the mixer channel in a clean state for reuse. */
    mixer_channel_stop(mp->cfg.mixer, mp->cfg.mixer_channel);
    mixer_channel_reset(mp->cfg.mixer, mp->cfg.mixer_channel);
    mp->free_fn(mp);
}

/* ============================================================
 *  Pinning the heads
 *
 *  Calls the stream callback once to fill the configured pinned
 *  head buffer. Stream offset is 0 (start of segment). Returns 0
 *  on success, -1 if the callback returned fewer samples than
 *  requested (segment shorter than head, or callback error).
 * ============================================================ */

static int prime_head(MusicPlayer *mp, int stream_id,
                       void *head_buf, size_t head_samples) {
    if (!head_buf || head_samples == 0) return 0;   /* no-op */
    if (stream_id == MUSIC_STREAM_NONE) return 0;   /* no segment */

    size_t got = mp->cfg.stream_fn(mp->cfg.stream_user_data,
                                    stream_id,
                                    head_buf,
                                    head_samples,
                                    0);
    return (got == head_samples) ? 0 : -1;
}

int music_prime_intro(MusicPlayer *mp) {
    if (!mp) return -1;
    return prime_head(mp, mp->cfg.intro_stream_id,
                       mp->cfg.intro_head_buffer,
                       mp->cfg.intro_head_samples);
}

int music_prime_loop(MusicPlayer *mp) {
    if (!mp) return -1;
    return prime_head(mp, mp->cfg.loop_stream_id,
                       mp->cfg.loop_head_buffer,
                       mp->cfg.loop_head_samples);
}

/* ============================================================
 *  Segment-start helpers
 *
 *  When starting (or restarting) a segment, we set up the state
 *  so that the first samples come from the pinned head (if
 *  configured) and subsequent samples come from the stream
 *  callback at the appropriate offset.
 * ============================================================ */

static void enter_segment(MusicPlayer *mp, CurrentSegment seg) {
    mp->segment        = seg;
    mp->segment_offset = 0;
    mp->streaming_count    = 0;
    mp->streaming_read_pos = 0;

    if (seg == SEG_INTRO) {
        mp->pinned_remaining = (mp->cfg.intro_head_buffer
                                ? mp->cfg.intro_head_samples : 0);
    } else if (seg == SEG_LOOP) {
        mp->pinned_remaining = (mp->cfg.loop_head_buffer
                                ? mp->cfg.loop_head_samples : 0);
    } else {
        mp->pinned_remaining = 0;
    }
}

static int current_segment_stream_id(const MusicPlayer *mp) {
    if (mp->segment == SEG_INTRO) return mp->cfg.intro_stream_id;
    if (mp->segment == SEG_LOOP)  return mp->cfg.loop_stream_id;
    return MUSIC_STREAM_NONE;
}

static void *current_pinned_buffer(const MusicPlayer *mp) {
    if (mp->segment == SEG_INTRO) return mp->cfg.intro_head_buffer;
    if (mp->segment == SEG_LOOP)  return mp->cfg.loop_head_buffer;
    return NULL;
}

static size_t current_pinned_size(const MusicPlayer *mp) {
    if (mp->segment == SEG_INTRO) return mp->cfg.intro_head_samples;
    if (mp->segment == SEG_LOOP)  return mp->cfg.loop_head_samples;
    return 0;
}

/* ============================================================
 *  Playback control
 * ============================================================ */

void music_play(MusicPlayer *mp) {
    if (!mp) return;

    if (mp->state == MUSIC_PAUSED) {
        /* Treat play as resume when paused. */
        music_resume(mp);
        return;
    }
    if (mp->state == MUSIC_PLAYING_INTRO ||
        mp->state == MUSIC_PLAYING_LOOP) {
        return;   /* no-op while playing */
    }

    /* From STOPPED. Pick starting segment: intro if configured,
     * else loop. */
    if (mp->cfg.intro_stream_id != MUSIC_STREAM_NONE) {
        enter_segment(mp, SEG_INTRO);
        mp->state = MUSIC_PLAYING_INTRO;
    } else if (mp->cfg.loop_stream_id != MUSIC_STREAM_NONE) {
        enter_segment(mp, SEG_LOOP);
        mp->state = MUSIC_PLAYING_LOOP;
    } else {
        return;   /* Shouldn't happen — validated at create. */
    }

    mixer_channel_reset(mp->cfg.mixer, mp->cfg.mixer_channel);
    mixer_channel_start(mp->cfg.mixer, mp->cfg.mixer_channel);
}

void music_stop(MusicPlayer *mp) {
    if (!mp) return;
    mixer_channel_stop(mp->cfg.mixer, mp->cfg.mixer_channel);
    mixer_channel_reset(mp->cfg.mixer, mp->cfg.mixer_channel);
    mp->state             = MUSIC_STOPPED;
    mp->segment           = SEG_NONE;
    mp->segment_offset    = 0;
    mp->pinned_remaining  = 0;
    mp->streaming_count   = 0;
    mp->streaming_read_pos = 0;
}

void music_pause(MusicPlayer *mp) {
    if (!mp) return;
    if (mp->state != MUSIC_PLAYING_INTRO && mp->state != MUSIC_PLAYING_LOOP) {
        return;   /* no-op from stopped or already paused */
    }
    /* Stop the mixer channel but preserve all our position state. */
    mixer_channel_stop(mp->cfg.mixer, mp->cfg.mixer_channel);
    mp->state = MUSIC_PAUSED;
}

void music_resume(MusicPlayer *mp) {
    if (!mp) return;
    if (mp->state != MUSIC_PAUSED) return;
    /* Restore the corresponding playing state. */
    if (mp->segment == SEG_INTRO) mp->state = MUSIC_PLAYING_INTRO;
    else if (mp->segment == SEG_LOOP) mp->state = MUSIC_PLAYING_LOOP;
    else mp->state = MUSIC_STOPPED;   /* shouldn't happen */

    if (mp->state != MUSIC_STOPPED) {
        mixer_channel_start(mp->cfg.mixer, mp->cfg.mixer_channel);
    }
}

/* ============================================================
 *  music_update — the workhorse
 *
 *  Each call:
 *    1. If streaming buffer is below threshold, call stream_fn to
 *       refill it (pulling from the current segment's stream_id at
 *       the appropriate offset).
 *    2. Transfer samples to the mixer channel until either:
 *         - mixer channel is full
 *         - we run out of available samples
 *    3. If we hit end-of-segment, transition.
 *
 *  Transitions:
 *    - End of intro: switch to loop. If loop has a pinned head,
 *      start playing from it; otherwise pull from stream.
 *    - End of loop: wrap to start of loop (same pinned head reset
 *      as initial loop entry).
 *    - End of intro without a loop: stop.
 * ============================================================ */

/* Refill streaming buffer from current segment's stream. Returns
 * the number of samples added. May be 0 if at end-of-stream. */
static size_t refill_streaming_buffer(MusicPlayer *mp) {
    /* Compact the streaming buffer if read position has advanced. */
    if (mp->streaming_read_pos > 0 && mp->streaming_count > 0) {
        uint8_t *buf = (uint8_t *)mp->cfg.streaming_buffer;
        size_t remaining = mp->streaming_count;
        memmove(buf,
                buf + mp->streaming_read_pos * mp->bytes_per_frame,
                remaining * mp->bytes_per_frame);
    }
    mp->streaming_read_pos = 0;

    /* How much room to refill? */
    size_t room = mp->cfg.streaming_buffer_samples - mp->streaming_count;
    if (room == 0) return 0;

    int sid = current_segment_stream_id(mp);
    if (sid == MUSIC_STREAM_NONE) return 0;

    /* The offset we pass to the callback is the segment offset
     * NOT YET delivered to the mixer minus what's already cached
     * in the streaming buffer. In other words, the callback offset
     * should be (mp->segment_offset + pinned_remaining + streaming_count). */
    size_t callback_offset = mp->segment_offset
                              + mp->pinned_remaining
                              + mp->streaming_count;

    uint8_t *dest = (uint8_t *)mp->cfg.streaming_buffer
                     + mp->streaming_count * mp->bytes_per_frame;

    size_t got = mp->cfg.stream_fn(mp->cfg.stream_user_data,
                                    sid,
                                    dest,
                                    room,
                                    callback_offset);
    mp->streaming_count += got;
    return got;
}

/* Push N samples to the mixer channel from the given source pointer.
 * The mixer's write_channel takes care of moving them in. */
static size_t push_to_mixer(MusicPlayer *mp, const void *src, size_t n) {
    return mixer_write_channel(mp->cfg.mixer, mp->cfg.mixer_channel, src, n);
}

/* Transfer from pinned head and/or streaming buffer to the mixer
 * channel. Returns the number of samples transferred. */
static size_t pump_to_mixer(MusicPlayer *mp, size_t max_to_transfer) {
    size_t transferred = 0;

    /* 1. Pinned head first (if any remaining). */
    if (mp->pinned_remaining > 0) {
        size_t pinned_size  = current_pinned_size(mp);
        size_t pinned_start = pinned_size - mp->pinned_remaining;
        size_t take = mp->pinned_remaining;
        if (take > max_to_transfer - transferred) {
            take = max_to_transfer - transferred;
        }

        const uint8_t *src = (const uint8_t *)current_pinned_buffer(mp)
                              + pinned_start * mp->bytes_per_frame;
        size_t pushed = push_to_mixer(mp, src, take);
        mp->pinned_remaining -= pushed;
        transferred          += pushed;
        if (pushed < take) return transferred;  /* mixer full */
    }

    /* 2. Streaming buffer next. */
    while (transferred < max_to_transfer && mp->streaming_count > 0) {
        size_t take = mp->streaming_count;
        if (take > max_to_transfer - transferred) {
            take = max_to_transfer - transferred;
        }
        const uint8_t *src = (const uint8_t *)mp->cfg.streaming_buffer
                              + mp->streaming_read_pos * mp->bytes_per_frame;
        size_t pushed = push_to_mixer(mp, src, take);
        mp->streaming_read_pos += pushed;
        mp->streaming_count    -= pushed;
        mp->segment_offset     += pushed;
        transferred            += pushed;
        if (pushed < take) break;  /* mixer full */
    }

    return transferred;
}

/* Check if the current segment is exhausted. We can't know in
 * general (the callback's job to signal end-of-stream), but we
 * approximate: end-of-segment happens when our last refill
 * returned 0 AND our streaming buffer is empty AND pinned is done.
 *
 * Implementation: we track end_of_stream by detecting a refill
 * that returns less than requested. For simplicity in stage 1 of
 * music_player, we treat any refill returning 0 as end-of-segment
 * (provided the callback isn't supposed to return silence-on-error,
 * which is documented behavior). */
static bool segment_exhausted(MusicPlayer *mp, bool last_refill_was_zero) {
    return (last_refill_was_zero
            && mp->streaming_count == 0
            && mp->pinned_remaining == 0);
}

void music_update(MusicPlayer *mp) {
    if (!mp) return;
    if (mp->state != MUSIC_PLAYING_INTRO &&
        mp->state != MUSIC_PLAYING_LOOP) {
        return;   /* stopped or paused — no work */
    }

    /* Decide how much to transfer this call. We use the mixer
     * channel's capacity minus its current fill — i.e., fill the
     * channel as full as it'll go. We don't know the channel's
     * capacity directly from the public API, but mixer_channel_buffered
     * tells us its current fill, and we can pump until push_to_mixer
     * stops accepting (mixer's overwrite-on-full prevents the call
     * from failing, but for safety we use a finite per-call cap). */

    /* Pump only as much as the mixer channel can actually hold without
     * overwriting unplayed audio. mixer_write_channel returns `count`
     * regardless of the ring's free space (it overwrites when full), so
     * a blind per-call target would advance the source far faster than
     * the channel drains — the music races (plays many seconds of source
     * per render quantum). Tie the pump to the channel's free space so
     * the source advances at the render/drain rate. Capped to the
     * streaming buffer too. */
    size_t cap  = mixer_channel_capacity(mp->cfg.mixer, mp->cfg.mixer_channel);
    /* Cap how full we let the channel run when the caller wants bounded
     * output latency (FMV A/V sync). Without this the channel fills to its
     * full capacity (e.g. 743 ms for a 32768-frame SFX ring), which is fine
     * for music but puts the audio ~0.8 s behind a near-instant video path. */
    if (mp->cfg.max_channel_fill_samples &&
        mp->cfg.max_channel_fill_samples < cap)
        cap = mp->cfg.max_channel_fill_samples;
    size_t fill = mixer_channel_buffered(mp->cfg.mixer, mp->cfg.mixer_channel);
    size_t target = (cap > fill) ? (cap - fill) : 0;
    if (target > mp->cfg.streaming_buffer_samples)
        target = mp->cfg.streaming_buffer_samples;
    if (target == 0) return;   /* channel full this tick — nothing to do */

    /* Loop: refill streaming buffer as needed, pump to mixer, detect
     * segment-end, transition. We iterate up to a few times to allow
     * for one transition per call. */
    for (int iter = 0; iter < 2; iter++) {
        /* Refill if streaming buffer is below half. */
        bool last_refill_zero = false;
        size_t half = mp->cfg.streaming_buffer_samples / 2;
        if (mp->streaming_count < half) {
            size_t prev = mp->streaming_count;
            (void)refill_streaming_buffer(mp);
            size_t got = mp->streaming_count - prev;
            size_t want = mp->cfg.streaming_buffer_samples - prev;
            if (got < want) last_refill_zero = (got == 0);
            /* Note: got < want could be "callback declined to fill all"
             * OR "end of stream." For now we treat got == 0 as the
             * end-of-stream signal (matches documented contract). */
        }

        /* Pump samples to mixer. */
        pump_to_mixer(mp, target);

        /* If segment is exhausted, transition. */
        if (segment_exhausted(mp, last_refill_zero)) {
            if (mp->segment == SEG_INTRO &&
                mp->cfg.loop_stream_id != MUSIC_STREAM_NONE) {
                enter_segment(mp, SEG_LOOP);
                mp->state = MUSIC_PLAYING_LOOP;
                /* Loop iteration: next pass refills loop. */
                continue;
            }
            if (mp->segment == SEG_LOOP) {
                /* Wrap to start of loop. */
                enter_segment(mp, SEG_LOOP);
                continue;
            }
            /* Intro with no loop, exhausted: stop. */
            music_stop(mp);
            return;
        }

        /* No transition needed. */
        break;
    }
}

/* ============================================================
 *  Introspection
 * ============================================================ */

MusicState music_state(const MusicPlayer *mp) {
    if (!mp) return MUSIC_STOPPED;
    return mp->state;
}
