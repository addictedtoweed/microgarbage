/* ============================================================
 *  test_audio_arbiter.c — track arbitration tests
 *
 *  Uses a stub sink that records start/stop calls (and can be told
 *  to decline a start, to test rollback). Backed by a real audio
 *  pool so the refcount integration is exercised for real.
 *
 *  Build:
 *    cc -std=c11 -DAUDIO_POOL_BLOCK_SIZE=64 -DAUDIO_ARBITER_MAX_TRACKS=4 \
 *       -Iinclude -o t \
 *       src/audio/tests/test_audio_arbiter.c src/audio/audio_arbiter.c \
 *       src/audio/audio_pool.c
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_arbiter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

#define REGION_BYTES (64 * AUDIO_POOL_BLOCK_SIZE)

/* ---- stub sink ---- */
typedef struct {
    int  start_calls;
    int  stop_calls;
    int  last_start_track;
    int  last_stop_track;
    bool decline_next_start;   /* simulate a sink that can't start */
    /* which tracks the sink currently believes are playing */
    bool playing[AUDIO_ARBITER_MAX_TRACKS];
    /* which tracks the sink reports as finished (for reap tests) */
    bool done[AUDIO_ARBITER_MAX_TRACKS];
} StubSink;

static bool stub_start(void *ctx, uint32_t track, AudioVoiceKind kind,
                       AudioObjHandle object, const AudioVoiceParams *p) {
    (void)kind; (void)object; (void)p;
    StubSink *s = (StubSink *)ctx;
    if (s->decline_next_start) { s->decline_next_start = false; return false; }
    s->start_calls++;
    s->last_start_track = (int)track;
    s->playing[track] = true;
    return true;
}
static void stub_stop(void *ctx, uint32_t track) {
    StubSink *s = (StubSink *)ctx;
    s->stop_calls++;
    s->last_stop_track = (int)track;
    s->playing[track] = false;
}
static bool stub_is_done(void *ctx, uint32_t track) {
    StubSink *s = (StubSink *)ctx;
    return s->done[track];
}

/* ---- fixture ---- */
static AudioPool     g_pool;
static uint8_t      *g_region;
static AudioArbiter  g_arb;
static StubSink      g_sink;

static void setup(uint32_t tracks) {
    g_region = malloc(REGION_BYTES);
    audio_pool_init(&g_pool, g_region, REGION_BYTES);
    memset(&g_sink, 0, sizeof(g_sink));
    AudioArbiterSink sink = { stub_start, stub_stop, &g_sink, stub_is_done };
    audio_arbiter_init(&g_arb, tracks, &g_pool, &sink);
}
static void teardown(void) {
    audio_pool_destroy(&g_pool);
    free(g_region);
}
static AudioObjHandle obj(uint16_t vm) {
    AudioObjHandle h;
    audio_pool_alloc(&g_pool, 100, vm, &h);
    return h;
}

/* ---- basic play returns a voice + drives the sink ---- */
static void test_play_returns_voice(void) {
    setup(4);
    AudioObjHandle o = obj(1);
    AudioVoiceHandle v;
    AudioArbResult r = audio_arbiter_play(&g_arb, o, AUDIO_VOICE_SFX,
                                          &(AudioVoiceParams){0}, 1, &v);
    CHECK(r == AUDIO_ARB_OK, "play OK");
    CHECK(v != AUDIO_VOICE_NONE, "voice handle returned");
    CHECK(audio_arbiter_voice_valid(&g_arb, v), "voice valid");
    CHECK(g_sink.start_calls == 1, "sink.start called once");
    CHECK(audio_arbiter_active_count(&g_arb) == 1, "one active voice");
    CHECK(audio_arbiter_free_tracks(&g_arb) == 3, "three free tracks");
    CHECK(audio_arbiter_voice_object(&g_arb, v) == o, "voice maps to object");
    teardown();
}

/* ---- fill all tracks then REJECT ---- */
static void test_fcfs_reject_on_full(void) {
    setup(4);
    AudioVoiceHandle v[4];
    for (int i = 0; i < 4; i++) {
        AudioArbResult r = audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_SFX,
                                              &(AudioVoiceParams){0}, 1, &v[i]);
        CHECK(r == AUDIO_ARB_OK, "fill track");
    }
    CHECK(audio_arbiter_free_tracks(&g_arb) == 0, "all tracks busy");

    AudioVoiceHandle vr;
    AudioArbResult r = audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_SFX,
                                          &(AudioVoiceParams){0}, 1, &vr);
    CHECK(r == AUDIO_ARB_REJECTED, "play on full -> REJECTED");
    CHECK(vr == AUDIO_VOICE_NONE, "rejected -> no voice handle");
    CHECK(g_sink.start_calls == 4, "sink not started for rejected play");
    teardown();
}

/* ---- stop frees a track ---- */
static void test_stop_frees_track(void) {
    setup(4);
    AudioVoiceHandle v;
    audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 1, &v);
    AudioArbResult r = audio_arbiter_stop(&g_arb, v);
    CHECK(r == AUDIO_ARB_OK, "stop OK");
    CHECK(g_sink.stop_calls == 1, "sink.stop called");
    CHECK(audio_arbiter_active_count(&g_arb) == 0, "no active voices after stop");
    CHECK(audio_arbiter_free_tracks(&g_arb) == 4, "track freed");
    CHECK(!audio_arbiter_voice_valid(&g_arb, v), "voice handle now stale");
    /* double-stop is a no-op error */
    CHECK(audio_arbiter_stop(&g_arb, v) == AUDIO_ARB_BAD_VOICE, "double stop -> BAD_VOICE");
    teardown();
}

/* ---- stale voice handle after track reuse ---- */
static void test_stale_voice_handle(void) {
    setup(2);
    AudioVoiceHandle v1;
    audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 1, &v1);
    audio_arbiter_stop(&g_arb, v1);    /* frees track, bumps gen */

    AudioVoiceHandle v2;
    audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 1, &v2);
    CHECK(v2 != v1, "reused track yields a new voice handle");
    CHECK(audio_arbiter_voice_valid(&g_arb, v2), "new voice valid");
    CHECK(!audio_arbiter_voice_valid(&g_arb, v1), "old voice stale after reuse");
    CHECK(audio_arbiter_stop(&g_arb, v1) == AUDIO_ARB_BAD_VOICE, "stop stale -> BAD_VOICE");
    teardown();
}

/* ---- THE refcount integration: playing keeps the object alive ---- */
static void test_playing_keeps_object_alive(void) {
    setup(4);
    AudioObjHandle o = obj(1);
    CHECK(audio_pool_refcount(&g_pool, o) == 1, "object refcount 1 after alloc");

    AudioVoiceHandle v;
    audio_arbiter_play(&g_arb, o, AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 1, &v);
    CHECK(audio_pool_refcount(&g_pool, o) == 2, "playing took a pool ref (now 2)");

    /* The creating VM frees ITS handle while the voice still plays. */
    bool freed = false;
    audio_pool_unref(&g_pool, o, &freed);
    CHECK(!freed, "object NOT freed — voice still holds a ref");
    CHECK(audio_pool_handle_valid(&g_pool, o), "object still alive while playing");
    CHECK(audio_arbiter_voice_object(&g_arb, v) == o, "voice still references the object");

    /* Stopping the voice drops the last ref -> object frees. */
    audio_arbiter_stop(&g_arb, v);
    CHECK(!audio_pool_handle_valid(&g_pool, o), "object freed after last voice stops");
    CHECK(audio_pool_free_blocks(&g_pool) == audio_pool_block_count(&g_pool),
          "object's blocks reclaimed");
    teardown();
}

/* ---- multiple voices on the same object share via refcount ---- */
static void test_multiple_voices_same_object(void) {
    setup(4);
    AudioObjHandle o = obj(1);
    AudioVoiceHandle a, b, c;
    audio_arbiter_play(&g_arb, o, AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 1, &a);
    audio_arbiter_play(&g_arb, o, AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 1, &b);
    audio_arbiter_play(&g_arb, o, AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 1, &c);
    /* 1 (creator) + 3 voices = 4 */
    CHECK(audio_pool_refcount(&g_pool, o) == 4, "three voices + creator = refcount 4");

    audio_pool_unref(&g_pool, o, NULL);              /* creator frees */
    CHECK(audio_pool_refcount(&g_pool, o) == 3, "after creator frees, 3 voices hold it");
    audio_arbiter_stop(&g_arb, a);
    audio_arbiter_stop(&g_arb, b);
    CHECK(audio_pool_handle_valid(&g_pool, o), "object alive with one voice left");
    audio_arbiter_stop(&g_arb, c);
    CHECK(!audio_pool_handle_valid(&g_pool, o), "object freed when last voice stops");
    teardown();
}

/* ---- VM sweep stops that VM's voices ---- */
static void test_vm_sweep(void) {
    setup(4);
    AudioObjHandle o1 = obj(1), o2 = obj(2);
    AudioVoiceHandle a, b, c;
    audio_arbiter_play(&g_arb, o1, AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 1, &a); /* vm1 */
    audio_arbiter_play(&g_arb, o1, AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 1, &b); /* vm1 */
    audio_arbiter_play(&g_arb, o2, AUDIO_VOICE_SFX, &(AudioVoiceParams){0}, 2, &c); /* vm2 */
    CHECK(audio_arbiter_active_count(&g_arb) == 3, "3 voices before sweep");

    uint32_t stopped = 0;
    audio_arbiter_sweep_vm(&g_arb, 1, &stopped);
    CHECK(stopped == 2, "sweep stopped vm1's two voices");
    CHECK(!audio_arbiter_voice_valid(&g_arb, a), "vm1 voice a stopped");
    CHECK(!audio_arbiter_voice_valid(&g_arb, b), "vm1 voice b stopped");
    CHECK(audio_arbiter_voice_valid(&g_arb, c), "vm2 voice c survives");
    CHECK(audio_arbiter_active_count(&g_arb) == 1, "1 voice after sweep");
    teardown();
}

/* ---- sink declines start -> clean rollback ---- */
static void test_sink_declines_rollback(void) {
    setup(4);
    AudioObjHandle o = obj(1);
    CHECK(audio_pool_refcount(&g_pool, o) == 1, "refcount 1 before");
    g_sink.decline_next_start = true;

    AudioVoiceHandle v;
    AudioArbResult r = audio_arbiter_play(&g_arb, o, AUDIO_VOICE_SFX,
                                          &(AudioVoiceParams){0}, 1, &v);
    CHECK(r == AUDIO_ARB_REJECTED, "declined start -> REJECTED");
    CHECK(v == AUDIO_VOICE_NONE, "no voice handle on decline");
    CHECK(audio_pool_refcount(&g_pool, o) == 1, "pool ref rolled back on decline");
    CHECK(audio_arbiter_free_tracks(&g_arb) == 4, "track not consumed on decline");
    teardown();
}

/* ---- bad object handle ---- */
static void test_bad_object(void) {
    setup(4);
    AudioVoiceHandle v;
    AudioArbResult r = audio_arbiter_play(&g_arb, AUDIO_POOL_HANDLE_NONE,
                                          AUDIO_VOICE_SFX,
                                          &(AudioVoiceParams){0}, 1, &v);
    CHECK(r == AUDIO_ARB_BAD_OBJECT, "play with invalid object -> BAD_OBJECT");
    CHECK(g_sink.start_calls == 0, "sink not called for bad object");
    teardown();
}

/* ---- reap frees finished one-shot SFX so triggers don't leak ---- */
static void test_reap_frees_finished_sfx(void) {
    setup(4);
    AudioVoiceHandle v[4];
    for (int i = 0; i < 4; i++)
        audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_SFX,
                           &(AudioVoiceParams){0}, 1, &v[i]);   /* tracks 0..3 */
    CHECK(audio_arbiter_free_tracks(&g_arb) == 0, "all 4 tracks busy");

    /* with everything busy a new trigger is rejected (the reported bug) */
    AudioVoiceHandle vr;
    CHECK(audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_SFX,
                             &(AudioVoiceParams){0}, 1, &vr) == AUDIO_ARB_REJECTED,
          "trigger rejected while full");

    /* mark tracks 0 and 1 finished; reap should free exactly those two */
    g_sink.done[0] = true;
    g_sink.done[1] = true;
    uint32_t reaped = audio_arbiter_reap(&g_arb);
    CHECK(reaped == 2, "reap freed the 2 finished SFX");
    CHECK(audio_arbiter_free_tracks(&g_arb) == 2, "2 tracks now free");
    CHECK(audio_arbiter_active_count(&g_arb) == 2, "2 voices still active");
    CHECK(!audio_arbiter_voice_valid(&g_arb, v[0]), "reaped voice 0 now stale");
    CHECK(!audio_arbiter_voice_valid(&g_arb, v[1]), "reaped voice 1 now stale");
    CHECK(audio_arbiter_voice_valid(&g_arb, v[2]), "unfinished voice 2 still valid");
    CHECK(g_sink.stop_calls == 2, "sink.stop called for each reaped track");

    /* and now a fresh trigger succeeds again */
    CHECK(audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_SFX,
                             &(AudioVoiceParams){0}, 1, &vr) == AUDIO_ARB_OK,
          "trigger OK after reap reclaimed a track");
    teardown();
}

/* ---- reap never touches looping music voices ---- */
static void test_reap_skips_music(void) {
    setup(4);
    AudioVoiceHandle vm, vs;
    audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_MUSIC,
                       &(AudioVoiceParams){0}, 1, &vm);   /* track 0 = music */
    audio_arbiter_play(&g_arb, obj(1), AUDIO_VOICE_SFX,
                       &(AudioVoiceParams){0}, 1, &vs);   /* track 1 = sfx  */
    /* sink claims BOTH are finished — reap must still spare the music. */
    g_sink.done[0] = true;
    g_sink.done[1] = true;
    uint32_t reaped = audio_arbiter_reap(&g_arb);
    CHECK(reaped == 1, "only the SFX voice reaped");
    CHECK(audio_arbiter_voice_valid(&g_arb, vm), "music voice kept");
    CHECK(!audio_arbiter_voice_valid(&g_arb, vs), "sfx voice reaped");
    teardown();
}

int main(void) {
    test_play_returns_voice();
    test_fcfs_reject_on_full();
    test_reap_frees_finished_sfx();
    test_reap_skips_music();
    test_stop_frees_track();
    test_stale_voice_handle();
    test_playing_keeps_object_alive();
    test_multiple_voices_same_object();
    test_vm_sweep();
    test_sink_declines_rollback();
    test_bad_object();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
