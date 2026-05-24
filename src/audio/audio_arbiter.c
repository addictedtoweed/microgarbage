/* ============================================================
 *  audio_arbiter.c — track arbitration implementation
 *
 *  Voice handle packing mirrors the pool's object handle:
 *  (generation << 16) | (track + 1), so a zero handle is always
 *  invalid and a stale handle to a reused track is detected via the
 *  generation.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_arbiter.h"

#include <string.h>

/* ---- handle pack/unpack ---- */

static AudioVoiceHandle pack_voice(uint32_t track, uint16_t gen) {
    return ((AudioVoiceHandle)gen << 16) | (track + 1u);
}
static bool unpack_voice(AudioVoiceHandle v, uint32_t *track, uint16_t *gen) {
    if (v == AUDIO_VOICE_NONE) return false;
    uint32_t t = (v & 0xFFFFu);
    if (t == 0) return false;
    *track = t - 1u;
    *gen   = (uint16_t)(v >> 16);
    return true;
}

/* Resolve a voice handle to a live track, or NULL. */
static AudioTrack *resolve(const AudioArbiter *a, AudioVoiceHandle v) {
    uint32_t track; uint16_t gen;
    if (!unpack_voice(v, &track, &gen)) return NULL;
    if (track >= a->track_count) return NULL;
    const AudioTrack *t = &a->tracks[track];
    if (!t->active) return NULL;
    if (t->generation != gen) return NULL;
    return (AudioTrack *)t;
}

/* ---- lifecycle ---- */

bool audio_arbiter_init(AudioArbiter *a, uint32_t track_count,
                        AudioPool *pool, const AudioArbiterSink *sink) {
    if (!a || !pool || !sink || !sink->start || !sink->stop) return false;
    if (track_count == 0) return false;
    if (track_count > AUDIO_ARBITER_MAX_TRACKS)
        track_count = AUDIO_ARBITER_MAX_TRACKS;

    memset(a, 0, sizeof(*a));
    a->pool        = pool;
    a->sink        = *sink;
    a->track_count = track_count;
    a->active_count = 0;
    for (uint32_t i = 0; i < AUDIO_ARBITER_MAX_TRACKS; i++) {
        a->generations[i] = 1;        /* start at 1, never 0 */
        a->tracks[i].active = false;
    }
    return true;
}

/* ---- play ---- */

static uint32_t find_free_track(const AudioArbiter *a) {
    for (uint32_t t = 0; t < a->track_count; t++) {
        if (!a->tracks[t].active) return t;
    }
    return AUDIO_ARBITER_MAX_TRACKS;   /* none */
}

AudioArbResult audio_arbiter_play(AudioArbiter *a,
                                  AudioObjHandle object,
                                  AudioVoiceKind kind,
                                  const AudioVoiceParams *params,
                                  uint16_t owner_vm,
                                  AudioVoiceHandle *out_voice) {
    if (!a || !out_voice) return AUDIO_ARB_INVALID_ARG;
    *out_voice = AUDIO_VOICE_NONE;

    if (!audio_pool_handle_valid(a->pool, object))
        return AUDIO_ARB_BAD_OBJECT;

    uint32_t track = find_free_track(a);
    if (track >= a->track_count)
        return AUDIO_ARB_REJECTED;     /* FCFS, reject-on-full */

    /* Take a reference on the object so it survives while playing,
     * even if its creator frees their handle. */
    if (audio_pool_ref(a->pool, object) != AUDIO_POOL_OK)
        return AUDIO_ARB_BAD_OBJECT;

    AudioVoiceParams p = params ? *params
                                : (AudioVoiceParams){ .gain = 0, .pan = 0,
                                                      .priority = 0, .loop = 0 };

    /* Drive the sink. If it declines, roll back the ref + claim. */
    if (!a->sink.start(a->sink.ctx, track, kind, object, &p)) {
        audio_pool_unref(a->pool, object, NULL);
        return AUDIO_ARB_REJECTED;
    }

    AudioTrack *t = &a->tracks[track];
    t->active     = true;
    t->object     = object;
    t->kind       = kind;
    t->owner_vm   = owner_vm;
    t->generation = a->generations[track];
    a->active_count++;

    *out_voice = pack_voice(track, t->generation);
    return AUDIO_ARB_OK;
}

AudioArbResult audio_arbiter_play_external(AudioArbiter *a,
                                           const AudioVoiceParams *params,
                                           uint16_t owner_vm,
                                           AudioVoiceHandle *out_voice) {
    if (!a || !out_voice) return AUDIO_ARB_INVALID_ARG;
    *out_voice = AUDIO_VOICE_NONE;

    uint32_t track = find_free_track(a);
    if (track >= a->track_count)
        return AUDIO_ARB_REJECTED;     /* FCFS, reject-on-full */

    AudioVoiceParams p = params ? *params
                                : (AudioVoiceParams){ .gain = 0, .pan = 0,
                                                      .priority = 0, .loop = 0 };

    /* No pool object: the sink owns the audio source itself. */
    if (!a->sink.start(a->sink.ctx, track, AUDIO_VOICE_MUSIC,
                       AUDIO_POOL_HANDLE_NONE, &p))
        return AUDIO_ARB_REJECTED;

    AudioTrack *t = &a->tracks[track];
    t->active     = true;
    t->object     = AUDIO_POOL_HANDLE_NONE;
    t->kind       = AUDIO_VOICE_MUSIC;
    t->owner_vm   = owner_vm;
    t->generation = a->generations[track];
    a->active_count++;

    *out_voice = pack_voice(track, t->generation);
    return AUDIO_ARB_OK;
}

/* ---- internal stop (shared by stop / finished / sweep) ---- */

static void stop_track(AudioArbiter *a, uint32_t track) {
    AudioTrack *t = &a->tracks[track];
    if (!t->active) return;

    a->sink.stop(a->sink.ctx, track);
    /* External (file-stream) voices carry no pool object. */
    if (t->object != AUDIO_POOL_HANDLE_NONE)
        audio_pool_unref(a->pool, t->object, NULL);

    /* bump generation so outstanding voice handles go stale */
    a->generations[track] = (uint16_t)(a->generations[track] + 1u);
    if (a->generations[track] == 0) a->generations[track] = 1;

    memset(t, 0, sizeof(*t));
    t->active = false;
    a->active_count--;
}

AudioArbResult audio_arbiter_stop(AudioArbiter *a, AudioVoiceHandle voice) {
    if (!a) return AUDIO_ARB_INVALID_ARG;
    uint32_t track; uint16_t gen;
    if (!unpack_voice(voice, &track, &gen)) return AUDIO_ARB_BAD_VOICE;
    AudioTrack *t = resolve(a, voice);
    if (!t) return AUDIO_ARB_BAD_VOICE;
    stop_track(a, track);
    return AUDIO_ARB_OK;
}

AudioArbResult audio_arbiter_voice_finished(AudioArbiter *a,
                                            AudioVoiceHandle voice) {
    /* Same mechanics as stop; distinct entry point for clarity at the
     * call site (playback ended naturally vs explicit stop). */
    return audio_arbiter_stop(a, voice);
}

uint32_t audio_arbiter_reap(AudioArbiter *a) {
    if (!a || !a->sink.is_done) return 0;
    uint32_t reaped = 0;
    for (uint32_t t = 0; t < a->track_count; t++) {
        AudioTrack *tr = &a->tracks[t];
        /* Only one-shot SFX. Music loops forever and is freed explicitly
         * (stop / VM sweep), so never reap it here. */
        if (!tr->active || tr->kind != AUDIO_VOICE_SFX) continue;
        if (a->sink.is_done(a->sink.ctx, t)) {
            stop_track(a, t);
            reaped++;
        }
    }
    return reaped;
}

AudioArbResult audio_arbiter_sweep_vm(AudioArbiter *a, uint16_t owner_vm,
                                      uint32_t *out_stopped) {
    if (!a) return AUDIO_ARB_INVALID_ARG;
    uint32_t stopped = 0;
    for (uint32_t t = 0; t < a->track_count; t++) {
        if (a->tracks[t].active && a->tracks[t].owner_vm == owner_vm) {
            stop_track(a, t);
            stopped++;
        }
    }
    if (out_stopped) *out_stopped = stopped;
    return AUDIO_ARB_OK;
}

/* ---- introspection ---- */

bool audio_arbiter_voice_valid(const AudioArbiter *a, AudioVoiceHandle v) {
    return a && resolve(a, v) != NULL;
}

uint32_t audio_arbiter_active_count(const AudioArbiter *a) {
    return a ? a->active_count : 0;
}

uint32_t audio_arbiter_free_tracks(const AudioArbiter *a) {
    if (!a) return 0;
    return a->track_count - a->active_count;
}

AudioObjHandle audio_arbiter_voice_object(const AudioArbiter *a,
                                          AudioVoiceHandle v) {
    if (!a) return AUDIO_POOL_HANDLE_NONE;
    AudioTrack *t = resolve(a, v);
    return t ? t->object : AUDIO_POOL_HANDLE_NONE;
}
