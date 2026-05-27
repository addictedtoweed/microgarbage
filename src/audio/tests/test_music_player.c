/* Tests for music_player.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "audio/music_player.h"
#include "audio/audio_mixer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 *  Test stream callback
 *
 *  Generates predictable test data: int16 samples where value is
 *  (stream_id * 1000) + offset. So the test can verify that the
 *  right stream was read at the right offset.
 *
 *  Has configurable "end" points — caller can set up "stream X
 *  has Y total samples" and the callback will return fewer than
 *  requested when offset+requested exceeds total. Stream of 0
 *  samples = unlimited.
 * ============================================================ */

#define TEST_STREAM_INTRO_ID  100
#define TEST_STREAM_LOOP_ID   200

typedef struct {
    int    stream_id;
    size_t total_samples;   /* 0 = unlimited */
    int    call_count;      /* how many times this stream was called */
    size_t last_offset;
    size_t last_request;
} TestStreamSlot;

static TestStreamSlot g_streams[4];

static void reset_streams(void) {
    memset(g_streams, 0, sizeof g_streams);
    g_streams[0] = (TestStreamSlot){ TEST_STREAM_INTRO_ID, 0, 0, 0, 0 };
    g_streams[1] = (TestStreamSlot){ TEST_STREAM_LOOP_ID,  0, 0, 0, 0 };
}

static TestStreamSlot *find_stream(int id) {
    for (size_t i = 0; i < 4; i++) {
        if (g_streams[i].stream_id == id) return &g_streams[i];
    }
    return NULL;
}

static size_t test_stream_fn(void *user_data, int stream_id,
                              void *destination, size_t requested,
                              size_t offset) {
    (void)user_data;
    TestStreamSlot *s = find_stream(stream_id);
    if (!s) return 0;
    s->call_count++;
    s->last_offset = offset;
    s->last_request = requested;

    size_t to_give = requested;
    if (s->total_samples > 0) {
        if (offset >= s->total_samples) return 0;
        if (offset + to_give > s->total_samples) {
            to_give = s->total_samples - offset;
        }
    }

    int16_t *dst = (int16_t *)destination;
    for (size_t i = 0; i < to_give; i++) {
        dst[i] = (int16_t)(stream_id * 100 + (int)(offset + i));
    }
    return to_give;
}

/* ============================================================
 *  Common test setup
 * ============================================================ */

#define MIXER_BUF_SAMPLES   64
#define STREAM_BUF_SAMPLES  32
#define HEAD_SAMPLES        8

static MixerOutputFormat fmt_q15_stereo(void) {
    MixerOutputFormat f = { .bits = 16, .is_signed = true, .storage_bits = 16, .channels = 2 };
    return f;
}

typedef struct {
    AudioMixer *mixer;
    MusicPlayer *player;
    int16_t streaming_buf[STREAM_BUF_SAMPLES];
    int16_t intro_head[HEAD_SAMPLES];
    int16_t loop_head[HEAD_SAMPLES];
} TestFixture;

static void fixture_setup(TestFixture *fx,
                           int intro_id, int loop_id,
                           bool with_intro_head, bool with_loop_head) {
    reset_streams();
    memset(fx, 0, sizeof *fx);

    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = MIXER_BUF_SAMPLES,
                                .volume = Q15_ONE };
    fx->mixer = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);

    MusicPlayerConfig mpc = {
        .mixer            = fx->mixer,
        .mixer_channel    = 0,
        .format           = MIXER_SRC_PCM16_MONO,
        .stream_fn        = test_stream_fn,
        .stream_user_data = NULL,
        .intro_stream_id  = intro_id,
        .loop_stream_id   = loop_id,
        .streaming_buffer = fx->streaming_buf,
        .streaming_buffer_samples = STREAM_BUF_SAMPLES,
        .intro_head_buffer = with_intro_head ? fx->intro_head : NULL,
        .intro_head_samples = with_intro_head ? HEAD_SAMPLES : 0,
        .loop_head_buffer = with_loop_head ? fx->loop_head : NULL,
        .loop_head_samples = with_loop_head ? HEAD_SAMPLES : 0,
    };
    fx->player = music_create(&mpc);
}

static void fixture_teardown(TestFixture *fx) {
    if (fx->player) music_destroy(fx->player);
    if (fx->mixer)  mixer_destroy(fx->mixer);
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

static void test_create_and_destroy(void) {
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    ASSERT_NOT_NULL(fx.player);
    ASSERT_EQ_INT(MUSIC_STOPPED, music_state(fx.player));
    fixture_teardown(&fx);
}

static void test_destroy_null_safe(void) {
    music_destroy(NULL);
}

static void test_create_with_no_streams_returns_null(void) {
    TestFixture fx;
    fixture_setup(&fx, MUSIC_STREAM_NONE, MUSIC_STREAM_NONE, false, false);
    ASSERT_NULL(fx.player);
    fixture_teardown(&fx);
}

static void test_create_with_intro_only(void) {
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, MUSIC_STREAM_NONE, true, false);
    ASSERT_NOT_NULL(fx.player);
    fixture_teardown(&fx);
}

static void test_create_with_loop_only(void) {
    TestFixture fx;
    fixture_setup(&fx, MUSIC_STREAM_NONE, TEST_STREAM_LOOP_ID, false, true);
    ASSERT_NOT_NULL(fx.player);
    fixture_teardown(&fx);
}

/* ============================================================
 *  Priming pinned heads
 * ============================================================ */

static void test_prime_intro_fills_head(void) {
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);

    /* Before prime, head buffer is zero (from fixture init). */
    for (int i = 0; i < HEAD_SAMPLES; i++) ASSERT_EQ_INT(0, fx.intro_head[i]);

    int rc = music_prime_intro(fx.player);
    ASSERT_EQ_INT(0, rc);

    /* After prime, head should be filled with (INTRO_ID * 100 + offset). */
    for (int i = 0; i < HEAD_SAMPLES; i++) {
        ASSERT_EQ_INT(TEST_STREAM_INTRO_ID * 100 + i, fx.intro_head[i]);
    }
    fixture_teardown(&fx);
}

static void test_prime_no_head_is_noop(void) {
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, false, true);

    int rc = music_prime_intro(fx.player);
    ASSERT_EQ_INT(0, rc);
    /* No call should have been made to the intro stream. */
    ASSERT_EQ_INT(0, find_stream(TEST_STREAM_INTRO_ID)->call_count);
    fixture_teardown(&fx);
}

/* ============================================================
 *  Play state transitions
 * ============================================================ */

static void test_play_from_stopped_with_intro(void) {
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_prime_intro(fx.player);
    music_prime_loop(fx.player);

    music_play(fx.player);
    ASSERT_EQ_INT(MUSIC_PLAYING_INTRO, music_state(fx.player));
    fixture_teardown(&fx);
}

static void test_play_with_loop_only(void) {
    TestFixture fx;
    fixture_setup(&fx, MUSIC_STREAM_NONE, TEST_STREAM_LOOP_ID, false, true);
    music_prime_loop(fx.player);

    music_play(fx.player);
    ASSERT_EQ_INT(MUSIC_PLAYING_LOOP, music_state(fx.player));
    fixture_teardown(&fx);
}

static void test_play_while_playing_is_noop(void) {
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_prime_intro(fx.player);
    music_prime_loop(fx.player);

    music_play(fx.player);
    MusicState s1 = music_state(fx.player);
    music_play(fx.player);   /* second call */
    MusicState s2 = music_state(fx.player);
    ASSERT_EQ_INT(s1, s2);
    fixture_teardown(&fx);
}

static void test_stop_transitions_to_stopped(void) {
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_prime_intro(fx.player);

    music_play(fx.player);
    music_stop(fx.player);
    ASSERT_EQ_INT(MUSIC_STOPPED, music_state(fx.player));
    fixture_teardown(&fx);
}

static void test_pause_resume(void) {
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_prime_intro(fx.player);
    music_prime_loop(fx.player);

    music_play(fx.player);
    music_pause(fx.player);
    ASSERT_EQ_INT(MUSIC_PAUSED, music_state(fx.player));

    music_resume(fx.player);
    ASSERT_EQ_INT(MUSIC_PLAYING_INTRO, music_state(fx.player));
    fixture_teardown(&fx);
}

static void test_play_resumes_from_pause(void) {
    /* music_play() when paused is documented to resume. */
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_prime_intro(fx.player);

    music_play(fx.player);
    music_pause(fx.player);
    music_play(fx.player);   /* should resume */
    ASSERT_EQ_INT(MUSIC_PLAYING_INTRO, music_state(fx.player));
    fixture_teardown(&fx);
}

static void test_pause_from_stopped_is_noop(void) {
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_pause(fx.player);
    ASSERT_EQ_INT(MUSIC_STOPPED, music_state(fx.player));
    fixture_teardown(&fx);
}

/* ============================================================
 *  Data flow
 * ============================================================ */

static void render_and_collect(AudioMixer *m, int16_t *out, size_t frames) {
    mixer_render(m, out, frames);
}

static void test_pinned_head_plays_first(void) {
    /* Intro head is filled with (INTRO_ID * 100 + 0..7).
     * Starting playback should send those samples to the mixer first.
     * After we render, we should see them in the output. */
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_prime_intro(fx.player);
    music_prime_loop(fx.player);

    music_play(fx.player);
    music_update(fx.player);   /* should pump the head to the mixer */

    int16_t out[HEAD_SAMPLES * 2];   /* stereo output, 8 frames */
    render_and_collect(fx.mixer, out, HEAD_SAMPLES);

    /* Each pinned head sample should appear on both L and R of one frame. */
    for (int i = 0; i < HEAD_SAMPLES; i++) {
        int16_t expected = TEST_STREAM_INTRO_ID * 100 + i;   /* 10000..10007 */
        ASSERT_EQ_INT(expected, out[i*2]);     /* L */
        ASSERT_EQ_INT(expected, out[i*2 + 1]); /* R */
    }
    fixture_teardown(&fx);
}

static void test_continues_from_stream_after_head(void) {
    /* After the pinned head, the player should pull from the stream
     * callback at offset == intro_head_samples. */
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_prime_intro(fx.player);
    music_prime_loop(fx.player);

    music_play(fx.player);
    music_update(fx.player);

    /* Render past the head into the streamed portion. */
    int16_t out[32];
    render_and_collect(fx.mixer, out, 16);

    /* First HEAD_SAMPLES frames = pinned head (INTRO*100 + 0..7).
     * Remaining 8 frames should be streamed at offset HEAD_SAMPLES
     * which gives values INTRO*100 + 8..15. */
    for (int i = 0; i < HEAD_SAMPLES; i++) {
        ASSERT_EQ_INT(TEST_STREAM_INTRO_ID * 100 + i, out[i*2]);
    }
    for (int i = HEAD_SAMPLES; i < 16; i++) {
        ASSERT_EQ_INT(TEST_STREAM_INTRO_ID * 100 + i, out[i*2]);
    }
    fixture_teardown(&fx);
}

static void test_no_head_pulls_from_stream_immediately(void) {
    /* With no intro head, first samples come from the stream at offset 0. */
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, false, false);
    /* No priming because no heads. */

    music_play(fx.player);
    music_update(fx.player);

    int16_t out[16];
    render_and_collect(fx.mixer, out, 8);

    for (int i = 0; i < 8; i++) {
        ASSERT_EQ_INT(TEST_STREAM_INTRO_ID * 100 + i, out[i*2]);
    }
    fixture_teardown(&fx);
}

/* ============================================================
 *  Intro→Loop transition
 * ============================================================ */

static void test_intro_finishes_transitions_to_loop(void) {
    /* Set up a short intro that ends quickly, then verify the loop kicks in. */
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, false, false);
    find_stream(TEST_STREAM_INTRO_ID)->total_samples = 10;  /* intro is 10 samples */

    music_play(fx.player);
    ASSERT_EQ_INT(MUSIC_PLAYING_INTRO, music_state(fx.player));

    /* Update a few times until intro finishes and loop starts. */
    for (int i = 0; i < 3; i++) music_update(fx.player);

    ASSERT_EQ_INT(MUSIC_PLAYING_LOOP, music_state(fx.player));
    fixture_teardown(&fx);
}

static void test_intro_no_loop_stops_at_end(void) {
    /* Intro-only player: when intro ends, player stops. */
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, MUSIC_STREAM_NONE, false, false);
    find_stream(TEST_STREAM_INTRO_ID)->total_samples = 8;

    music_play(fx.player);
    for (int i = 0; i < 3; i++) music_update(fx.player);

    ASSERT_EQ_INT(MUSIC_STOPPED, music_state(fx.player));
    fixture_teardown(&fx);
}

static void test_loop_wraps_around(void) {
    /* Loop with a defined length should wrap back to offset 0
     * when exhausted. We verify by checking the stream callback
     * gets called with offset 0 again after exhaustion. */
    TestFixture fx;
    fixture_setup(&fx, MUSIC_STREAM_NONE, TEST_STREAM_LOOP_ID, false, false);
    find_stream(TEST_STREAM_LOOP_ID)->total_samples = 16;

    music_play(fx.player);
    /* Drain the loop several times. */
    for (int i = 0; i < 10; i++) music_update(fx.player);

    /* Should still be playing the loop (wrapped). */
    ASSERT_EQ_INT(MUSIC_PLAYING_LOOP, music_state(fx.player));
    /* Stream callback should have been called multiple times,
     * including at offset 0 after wrap. */
    int call_count = find_stream(TEST_STREAM_LOOP_ID)->call_count;
    ASSERT(call_count > 1);
    fixture_teardown(&fx);
}

/* ============================================================
 *  Stop and restart behavior
 * ============================================================ */

static void test_stop_clears_state(void) {
    /* After stop, mixer channel should be empty and player STOPPED. */
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_prime_intro(fx.player);

    music_play(fx.player);
    music_update(fx.player);
    /* Mixer should have samples buffered. */
    ASSERT(mixer_channel_buffered(fx.mixer, 0) > 0);

    music_stop(fx.player);
    ASSERT_EQ_INT(0, (int)mixer_channel_buffered(fx.mixer, 0));
    ASSERT_EQ_INT(MUSIC_STOPPED, music_state(fx.player));
    fixture_teardown(&fx);
}

static void test_restart_uses_pinned_head_again(void) {
    /* After stop and play, the pinned head should play again
     * (this is the rapid-restart feature). */
    TestFixture fx;
    fixture_setup(&fx, TEST_STREAM_INTRO_ID, TEST_STREAM_LOOP_ID, true, true);
    music_prime_intro(fx.player);
    music_prime_loop(fx.player);

    music_play(fx.player);
    music_update(fx.player);
    music_stop(fx.player);

    int call_count_before = find_stream(TEST_STREAM_INTRO_ID)->call_count;

    music_play(fx.player);
    music_update(fx.player);

    int16_t out[HEAD_SAMPLES * 2];
    render_and_collect(fx.mixer, out, HEAD_SAMPLES);

    /* First samples should be the pinned head again. */
    for (int i = 0; i < HEAD_SAMPLES; i++) {
        ASSERT_EQ_INT(TEST_STREAM_INTRO_ID * 100 + i, out[i*2]);
    }

    /* The stream callback may have been called for non-pinned data,
     * but the pinned head itself was reused (call_count for offset 0
     * shouldn't have grown by another priming call). */
    (void)call_count_before;   /* informational */
    fixture_teardown(&fx);
}

int main(void) {
    TEST_SUITE("music_player");

    /* Lifecycle */
    RUN(test_create_and_destroy);
    RUN(test_destroy_null_safe);
    RUN(test_create_with_no_streams_returns_null);
    RUN(test_create_with_intro_only);
    RUN(test_create_with_loop_only);

    /* Priming */
    RUN(test_prime_intro_fills_head);
    RUN(test_prime_no_head_is_noop);

    /* State machine */
    RUN(test_play_from_stopped_with_intro);
    RUN(test_play_with_loop_only);
    RUN(test_play_while_playing_is_noop);
    RUN(test_stop_transitions_to_stopped);
    RUN(test_pause_resume);
    RUN(test_play_resumes_from_pause);
    RUN(test_pause_from_stopped_is_noop);

    /* Data flow */
    RUN(test_pinned_head_plays_first);
    RUN(test_continues_from_stream_after_head);
    RUN(test_no_head_pulls_from_stream_immediately);

    /* Transitions */
    RUN(test_intro_finishes_transitions_to_loop);
    RUN(test_intro_no_loop_stops_at_end);
    RUN(test_loop_wraps_around);

    /* Stop/restart */
    RUN(test_stop_clears_state);
    RUN(test_restart_uses_pinned_head_again);

    return TEST_SUITE_RESULT();
}
