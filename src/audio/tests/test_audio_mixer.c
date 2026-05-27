/* Tests for audio_mixer, stage 2.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "audio/audio_mixer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================
 *  Tracking allocator — counts alloc/free calls so tests can
 *  verify the mixer cleans up everything it allocated.
 * ============================================================ */

static int g_alloc_calls = 0;
static int g_free_calls = 0;

static void *tracking_alloc(size_t n) {
    g_alloc_calls++;
    return malloc(n);
}
static void tracking_free(void *p) {
    if (p) g_free_calls++;
    free(p);
}
static void reset_alloc_tracking(void) {
    g_alloc_calls = 0;
    g_free_calls = 0;
}

/* ============================================================
 *  Test fixtures — common output formats
 * ============================================================ */

static MixerOutputFormat fmt_q15_stereo(void) {
    MixerOutputFormat f = { .bits = 16, .is_signed = true, .storage_bits = 16, .channels = 2 };
    return f;
}
static MixerOutputFormat fmt_q15_mono(void) {
    MixerOutputFormat f = { .bits = 16, .is_signed = true, .storage_bits = 16, .channels = 1 };
    return f;
}
static MixerOutputFormat fmt_8u_stereo(void) {
    MixerOutputFormat f = { .bits = 8, .is_signed = false, .storage_bits = 8, .channels = 2 };
    return f;
}
static MixerOutputFormat fmt_8s_stereo(void) {
    MixerOutputFormat f = { .bits = 8, .is_signed = true, .storage_bits = 8, .channels = 2 };
    return f;
}
static MixerOutputFormat fmt_12u_stereo(void) {
    MixerOutputFormat f = { .bits = 12, .is_signed = false, .storage_bits = 16, .channels = 2 };
    return f;
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

static void test_create_and_destroy(void) {
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    ASSERT_NOT_NULL(m);
    mixer_destroy(m);
}

static void test_destroy_null_safe(void) {
    mixer_destroy(NULL);
}

static void test_create_invalid_format_returns_null(void) {
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE };
    MixerOutputFormat bad = { .bits = 0, .is_signed = true, .storage_bits = 16, .channels = 2 };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, bad, 0);
    ASSERT_NULL(m);

    /* storage_bits = 8 with bits = 12 — also invalid */
    bad.bits = 12;
    bad.storage_bits = 8;
    m = mixer_create(&cfg, 1, 44100, bad, 0);
    ASSERT_NULL(m);
}

static void test_custom_allocator_routes_through(void) {
    reset_alloc_tracking();

    MixerChannelConfig cfg[2] = {
        { .format = MIXER_SRC_PCM16_MONO,   .buffer_samples = 64, .volume = Q15_ONE },
        { .format = MIXER_SRC_PCM8U_STEREO, .buffer_samples = 32, .volume = Q15_ONE },
    };
    AudioMixer *m = mixer_create_with_allocator(cfg, 2, 44100,
                                                 fmt_q15_stereo(), 0,
                                                 tracking_alloc, tracking_free);
    ASSERT_NOT_NULL(m);
    /* Expected allocs: mixer struct, channels array, channel_buffers array,
     * 2 per-channel buffers = 5. */
    ASSERT_EQ_INT(5, g_alloc_calls);

    mixer_destroy(m);
    /* Destroy frees everything it allocated. */
    ASSERT_EQ_INT(g_alloc_calls, g_free_calls);
}

static void test_null_allocator_uses_default(void) {
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE };
    AudioMixer *m = mixer_create_with_allocator(&cfg, 1, 44100,
                                                 fmt_q15_stereo(), 0,
                                                 NULL, NULL);
    ASSERT_NOT_NULL(m);
    mixer_destroy(m);
}

/* ============================================================
 *  Helper: make a simple mixer with one channel
 * ============================================================ */

static AudioMixer *make_simple_mixer(MixerSourceFormat src_fmt,
                                      size_t buf_samples,
                                      MixerOutputFormat out_fmt,
                                      uint8_t headroom) {
    MixerChannelConfig cfg = { .format = src_fmt,
                                .buffer_samples = buf_samples,
                                .volume = Q15_ONE };
    return mixer_create(&cfg, 1, 44100, out_fmt, headroom);
}

/* ============================================================
 *  Basic stereo output with 16-bit mono source
 * ============================================================ */

static void test_16mono_to_q15_stereo(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    int16_t samples[8] = { 100, 200, 300, 400, 500, 600, 700, 800 };
    mixer_write_channel(m, 0, samples, 8);
    mixer_channel_start(m, 0);

    int16_t out[16];   /* 8 frames × 2 channels */
    mixer_render(m, out, 8);

    /* Mono source → stereo output: same value on L and R. */
    for (int f = 0; f < 8; f++) {
        ASSERT_EQ_INT(samples[f], out[f*2]);
        ASSERT_EQ_INT(samples[f], out[f*2 + 1]);
    }
    mixer_destroy(m);
}

/* ============================================================
 *  8-bit source formats
 * ============================================================ */

static void test_8s_mono_source(void) {
    /* 8-bit signed: -128..127 maps to q15 range. */
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM8S_MONO, 64,
                                       fmt_q15_stereo(), 0);
    int8_t samples[4] = { 100, 50, -50, -100 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);

    /* 8s → q15: sign-extend, shift left 8 → value * 256. */
    ASSERT_EQ_INT(100  * 256, out[0]);
    ASSERT_EQ_INT(50   * 256, out[2]);
    ASSERT_EQ_INT(-50  * 256, out[4]);
    ASSERT_EQ_INT(-100 * 256, out[6]);
    mixer_destroy(m);
}

static void test_8u_mono_source(void) {
    /* 8-bit unsigned: 128 is silence. */
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM8U_MONO, 64,
                                       fmt_q15_stereo(), 0);
    uint8_t samples[4] = { 128, 200, 64, 0 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);

    /* 8u → q15: (s - 128) << 8. */
    ASSERT_EQ_INT(((int)128 - 128) * 256, out[0]);  /* 0 */
    ASSERT_EQ_INT(((int)200 - 128) * 256, out[2]);  /* 18432 */
    ASSERT_EQ_INT(((int)64  - 128) * 256, out[4]);  /* -16384 */
    ASSERT_EQ_INT(((int)0   - 128) * 256, out[6]);  /* -32768 */
    mixer_destroy(m);
}

/* ============================================================
 *  Stereo source formats
 * ============================================================ */

static void test_16stereo_source(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_STEREO, 64,
                                       fmt_q15_stereo(), 0);
    /* 4 frames, each L R interleaved. */
    int16_t samples[8] = { 100, 200,   300, 400,   500, 600,   700, 800 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);

    /* Stereo source → stereo out: L stays L, R stays R. */
    for (int f = 0; f < 4; f++) {
        ASSERT_EQ_INT(samples[f*2],     out[f*2]);
        ASSERT_EQ_INT(samples[f*2 + 1], out[f*2 + 1]);
    }
    mixer_destroy(m);
}

static void test_8s_stereo_source(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM8S_STEREO, 64,
                                       fmt_q15_stereo(), 0);
    int8_t samples[8] = { 10, 20,   30, 40,   -10, -20,   100, -100 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);

    for (int f = 0; f < 4; f++) {
        ASSERT_EQ_INT(samples[f*2]     * 256, out[f*2]);
        ASSERT_EQ_INT(samples[f*2 + 1] * 256, out[f*2 + 1]);
    }
    mixer_destroy(m);
}

static void test_8u_stereo_source(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM8U_STEREO, 64,
                                       fmt_q15_stereo(), 0);
    uint8_t samples[4] = { 128, 200,   64, 0 };   /* 2 frames */
    mixer_write_channel(m, 0, samples, 2);
    mixer_channel_start(m, 0);

    int16_t out[4];
    mixer_render(m, out, 2);

    /* Frame 0: silence (128 = 0), 200 = (200-128)<<8 = 18432 */
    ASSERT_EQ_INT(0, out[0]);
    ASSERT_EQ_INT(18432, out[1]);
    /* Frame 1: 64 = (64-128)<<8 = -16384, 0 = (0-128)<<8 = -32768 */
    ASSERT_EQ_INT(-16384, out[2]);
    ASSERT_EQ_INT(-32768, out[3]);
    mixer_destroy(m);
}

/* ============================================================
 *  Channel-conversion tests
 * ============================================================ */

static void test_stereo_source_to_mono_output(void) {
    /* Stereo source → mono out: output is (L+R)/2. */
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_STEREO, 64,
                                       fmt_q15_mono(), 0);
    int16_t samples[4] = { 100, 200,   400, 600 };   /* 2 frames */
    mixer_write_channel(m, 0, samples, 2);
    mixer_channel_start(m, 0);

    int16_t out[2];
    mixer_render(m, out, 2);

    ASSERT_EQ_INT(150, out[0]);   /* (100 + 200) / 2 */
    ASSERT_EQ_INT(500, out[1]);   /* (400 + 600) / 2 */
    mixer_destroy(m);
}

static void test_mono_source_to_mono_output(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_mono(), 0);
    int16_t samples[4] = { 100, 200, 300, 400 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[4];
    mixer_render(m, out, 4);

    for (int i = 0; i < 4; i++) ASSERT_EQ_INT(samples[i], out[i]);
    mixer_destroy(m);
}

/* ============================================================
 *  Output bit-depth conversions
 * ============================================================ */

static void test_q15_to_8u_output(void) {
    /* q15 silence (0) → 8u silence (128). */
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_8u_stereo(), 0);
    int16_t samples[4] = { 0, 16384, -16384, 32767 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    uint8_t out[8];
    mixer_render(m, out, 4);

    /* q15 → 8u: (sample >> 8) + 128, clamped to 0..255.
     * 0      → 0 + 128 = 128.
     * 16384  → 64 + 128 = 192.
     * -16384 → -64 + 128 = 64.
     * 32767  → 127 + 128 = 255. */
    ASSERT_EQ_INT(128, out[0]);
    ASSERT_EQ_INT(192, out[2]);
    ASSERT_EQ_INT(64,  out[4]);
    ASSERT_EQ_INT(255, out[6]);
    mixer_destroy(m);
}

static void test_q15_to_8s_output(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_8s_stereo(), 0);
    int16_t samples[4] = { 0, 12800, -12800, 32767 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int8_t out[8];
    mixer_render(m, out, 4);

    /* q15 → 8s: sample >> 8, clamped to -128..127.
     * 0      → 0
     * 12800  → 50
     * -12800 → -50
     * 32767  → 127 */
    ASSERT_EQ_INT(0,    out[0]);
    ASSERT_EQ_INT(50,   out[2]);
    ASSERT_EQ_INT(-50,  out[4]);
    ASSERT_EQ_INT(127,  out[6]);
    mixer_destroy(m);
}

static void test_q15_to_12u_output(void) {
    /* 12-bit unsigned in uint16. */
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_12u_stereo(), 0);
    int16_t samples[4] = { 0, 16384, -16384, 32767 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    uint16_t out[8];
    mixer_render(m, out, 4);

    /* q15 → 12: sample >> 4. Unsigned offset: + 2048.
     * 0      → 0 + 2048 = 2048.
     * 16384  → 1024 + 2048 = 3072.
     * -16384 → -1024 + 2048 = 1024.
     * 32767  → 2047 + 2048 = 4095. */
    ASSERT_EQ_INT(2048, out[0]);
    ASSERT_EQ_INT(3072, out[2]);
    ASSERT_EQ_INT(1024, out[4]);
    ASSERT_EQ_INT(4095, out[6]);
    mixer_destroy(m);
}

/* ============================================================
 *  Multi-channel mixing
 * ============================================================ */

static void test_two_channels_summed(void) {
    MixerChannelConfig cfg[2] = {
        { .format = MIXER_SRC_PCM16_MONO, .buffer_samples = 64, .volume = Q15_ONE },
        { .format = MIXER_SRC_PCM16_MONO, .buffer_samples = 64, .volume = Q15_ONE },
    };
    AudioMixer *m = mixer_create(cfg, 2, 44100, fmt_q15_stereo(), 0);
    ASSERT_NOT_NULL(m);

    int16_t a[4] = { 100, 200, 300, 400 };
    int16_t b[4] = { 50, 50, 50, 50 };
    mixer_write_channel(m, 0, a, 4);
    mixer_write_channel(m, 1, b, 4);
    mixer_channel_start(m, 0);
    mixer_channel_start(m, 1);

    int16_t out[8];
    mixer_render(m, out, 4);

    ASSERT_EQ_INT(150, out[0]);
    ASSERT_EQ_INT(250, out[2]);
    ASSERT_EQ_INT(350, out[4]);
    ASSERT_EQ_INT(450, out[6]);
    mixer_destroy(m);
}

/* ============================================================
 *  Headroom and saturation
 * ============================================================ */

static void test_saturation_no_headroom(void) {
    MixerChannelConfig cfg[4] = {0};
    for (int i = 0; i < 4; i++) {
        cfg[i].format = MIXER_SRC_PCM16_MONO;
        cfg[i].buffer_samples = 64;
        cfg[i].volume = Q15_ONE;
    }
    AudioMixer *m = mixer_create(cfg, 4, 44100, fmt_q15_stereo(), 0);
    int16_t loud[4] = { Q15_MAX, Q15_MAX, Q15_MAX, Q15_MAX };
    for (int i = 0; i < 4; i++) {
        mixer_write_channel(m, i, loud, 4);
        mixer_channel_start(m, i);
    }

    int16_t out[8];
    mixer_render(m, out, 4);

    /* 4 * Q15_MAX overflows q15; saturates. */
    for (int f = 0; f < 4; f++) {
        ASSERT_EQ_INT(Q15_MAX, out[f*2]);
        ASSERT_EQ_INT(Q15_MAX, out[f*2 + 1]);
    }
    mixer_destroy(m);
}

static void test_headroom_prevents_clipping(void) {
    MixerChannelConfig cfg[4] = {0};
    for (int i = 0; i < 4; i++) {
        cfg[i].format = MIXER_SRC_PCM16_MONO;
        cfg[i].buffer_samples = 64;
        cfg[i].volume = Q15_ONE;
    }
    /* 3 bits of headroom — 4 × Q15_MAX = 131068, >> 3 = 16383, no clip. */
    AudioMixer *m = mixer_create(cfg, 4, 44100, fmt_q15_stereo(), 3);
    int16_t loud[4] = { Q15_MAX, Q15_MAX, Q15_MAX, Q15_MAX };
    for (int i = 0; i < 4; i++) {
        mixer_write_channel(m, i, loud, 4);
        mixer_channel_start(m, i);
    }

    int16_t out[8];
    mixer_render(m, out, 4);

    for (int f = 0; f < 4; f++) {
        ASSERT(out[f*2] >= 16370 && out[f*2] <= 16390);
    }
    mixer_destroy(m);
}

/* ============================================================
 *  Channel control
 * ============================================================ */

static void test_inactive_channel_silent(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    int16_t samples[4] = { 1000, 2000, 3000, 4000 };
    mixer_write_channel(m, 0, samples, 4);
    /* Note: NOT calling channel_start */

    int16_t out[8];
    mixer_render(m, out, 4);
    for (int i = 0; i < 8; i++) ASSERT_EQ_INT(0, out[i]);
    mixer_destroy(m);
}

static void test_muted_channel_consumes_samples(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    int16_t samples[16] = {0};
    for (int i = 0; i < 16; i++) samples[i] = 1000 + i;
    mixer_write_channel(m, 0, samples, 16);
    mixer_channel_start(m, 0);
    mixer_mute(m, 0, true);

    ASSERT_EQ_INT(16, (int)mixer_channel_buffered(m, 0));

    int16_t out[16];
    mixer_render(m, out, 8);

    /* Output is silent, but buffer drained */
    for (int i = 0; i < 16; i++) ASSERT_EQ_INT(0, out[i]);
    ASSERT_EQ_INT(8, (int)mixer_channel_buffered(m, 0));

    /* Unmute, render the rest — these should be the *later* samples
     * (1008..1015), since 1000..1007 were consumed while muted. */
    mixer_mute(m, 0, false);
    mixer_render(m, out, 8);
    for (int f = 0; f < 8; f++) {
        ASSERT_EQ_INT(1008 + f, out[f*2]);
    }
    mixer_destroy(m);
}

static void test_buffer_underrun_silent(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    int16_t samples[2] = { 500, 600 };
    mixer_write_channel(m, 0, samples, 2);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);

    ASSERT_EQ_INT(500, out[0]);
    ASSERT_EQ_INT(600, out[2]);
    /* Remaining frames are silence (underrun). */
    ASSERT_EQ_INT(0, out[4]);
    ASSERT_EQ_INT(0, out[6]);
    mixer_destroy(m);
}

static void test_channel_reset_clears_buffer(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    int16_t samples[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    mixer_write_channel(m, 0, samples, 8);
    mixer_channel_start(m, 0);
    mixer_channel_reset(m, 0);

    ASSERT_EQ_INT(0, (int)mixer_channel_buffered(m, 0));
    mixer_destroy(m);
}

/* ============================================================
 *  Out-of-range safety
 * ============================================================ */

static void test_out_of_range_indices_safe(void) {
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);

    mixer_set_volume(m, 999, Q15_ONE);
    mixer_mute(m, 999, true);
    mixer_channel_start(m, 999);
    mixer_channel_stop(m, 999);
    mixer_channel_reset(m, 999);

    int16_t sample = 0;
    ASSERT_EQ_INT(0, (int)mixer_write_channel(m, 999, &sample, 1));
    ASSERT_EQ_INT(0, (int)mixer_channel_buffered(m, 999));

    mixer_destroy(m);
}

/* ============================================================
 *  Volume control
 * ============================================================ */

static void test_volume_zero(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    mixer_set_volume(m, 0, 0);
    int16_t samples[4] = { 0x4000, 0x4000, 0x4000, 0x4000 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);
    for (int i = 0; i < 8; i++) ASSERT_EQ_INT(0, out[i]);
    mixer_destroy(m);
}

static void test_volume_half(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    mixer_set_volume(m, 0, Q15_ONE / 2);
    int16_t samples[4] = { 0x4000, 0x4000, 0x4000, 0x4000 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);
    /* Roughly 0x4000 * 0.5 = 0x2000, with q15 quantization slack. */
    for (int f = 0; f < 4; f++) {
        ASSERT(out[f*2] >= 0x1FF0 && out[f*2] <= 0x2010);
    }
    mixer_destroy(m);
}

/* ============================================================
 *  Stage 3: pan
 * ============================================================ */

static void test_pan_center_unchanged(void) {
    /* Pan at 0 should not affect output. */
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    mixer_set_pan(m, 0, 0);
    int16_t samples[4] = { 1000, 2000, 3000, 4000 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);

    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_INT(samples[i], out[i*2]);     /* L */
        ASSERT_EQ_INT(samples[i], out[i*2 + 1]); /* R */
    }
    mixer_destroy(m);
}

static void test_pan_full_left(void) {
    /* Pan = -Q15_ONE: L gets full, R gets zero. */
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    mixer_set_pan(m, 0, -Q15_ONE);
    int16_t samples[4] = { 1000, 2000, 3000, 4000 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);

    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_INT(samples[i], out[i*2]);   /* L unchanged */
        ASSERT_EQ_INT(0,          out[i*2+1]); /* R silenced */
    }
    mixer_destroy(m);
}

static void test_pan_full_right(void) {
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    mixer_set_pan(m, 0, Q15_ONE);
    int16_t samples[4] = { 1000, 2000, 3000, 4000 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[8];
    mixer_render(m, out, 4);

    for (int i = 0; i < 4; i++) {
        ASSERT_EQ_INT(0,          out[i*2]);   /* L silenced */
        ASSERT_EQ_INT(samples[i], out[i*2+1]); /* R unchanged */
    }
    mixer_destroy(m);
}

/* ============================================================
 *  Stage 3: looping
 * ============================================================ */

static void test_loop_no_resample_wraps(void) {
    /* Load 4 samples and set loop. Render 12 frames. Output should
     * repeat the 4 samples 3 times. */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 16,
                                .volume = Q15_ONE,
                                .source_sample_rate = 0,
                                .pan = 0,
                                .loop = true,
                                .interp = MIXER_INTERP_LINEAR };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t loop_samples[4] = { 100, 200, 300, 400 };
    mixer_write_channel(m, 0, loop_samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[24];
    mixer_render(m, out, 12);

    /* Frames 0..3 should be the loop content; 4..7 same again; 8..11 same. */
    for (int f = 0; f < 12; f++) {
        ASSERT_EQ_INT(loop_samples[f % 4], out[f*2]);
    }
    mixer_destroy(m);
}

/* ============================================================
 *  Stage 3: resampling (integer ratios)
 *
 *  These are the easy cases where expected output is exact.
 * ============================================================ */

static void test_2x_upsample_linear(void) {
    /* Source rate = 22050, output = 44100. Step = 0.5 (each source
     * sample played twice). Linear interpolation produces:
     *   frame 0: tap[0] (source sample 0)
     *   frame 1: midpoint between tap[0] and tap[1]
     *   frame 2: tap[0] (now source sample 1)
     *   frame 3: midpoint between source samples 1 and 2
     *   ...
     */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 16,
                                .volume = Q15_ONE,
                                .source_sample_rate = 22050,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t samples[4] = { 0, 1000, 2000, 3000 };
    mixer_write_channel(m, 0, samples, 4);
    mixer_channel_start(m, 0);

    int16_t out[16];
    mixer_render(m, out, 8);

    /* Frame 0: phase 0.0 → linear(s0, s1, 0) = s0 = 0
     * Frame 1: phase 0.5 → linear(s0, s1, 0.5) = (0 + 1000)/2 = 500
     * Frame 2: phase 1.0 (advance) → linear(s1, s2, 0) = s1 = 1000
     * Frame 3: phase 1.5 → (1000 + 2000)/2 = 1500
     * etc. */
    ASSERT_EQ_INT(0,    out[0]);
    ASSERT(out[2] >= 499 && out[2] <= 500);     /* allow rounding */
    ASSERT_EQ_INT(1000, out[4]);
    ASSERT(out[6] >= 1499 && out[6] <= 1500);
    mixer_destroy(m);
}

static void test_2x_downsample_linear(void) {
    /* Source rate = 88200, output = 44100. Step = 2.0 (every other
     * source sample). Linear: frame N reads sample 2N. */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 32,
                                .volume = Q15_ONE,
                                .source_sample_rate = 88200,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t samples[16];
    for (int i = 0; i < 16; i++) samples[i] = (int16_t)(100 * i);
    mixer_write_channel(m, 0, samples, 16);
    mixer_channel_start(m, 0);

    int16_t out[16];
    mixer_render(m, out, 8);

    /* Linear at integer step: frame N reads sample 2N exactly (frac=0). */
    for (int f = 0; f < 8; f++) {
        int16_t expected = (int16_t)(100 * 2 * f);
        ASSERT_EQ_INT(expected, out[f*2]);
    }
    mixer_destroy(m);
}

static void test_cubic_at_integer_phase(void) {
    /* Cubic interpolation at frac=0 should give p1 (no change). */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 32,
                                .volume = Q15_ONE,
                                .source_sample_rate = 88200,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_CUBIC };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t samples[16];
    for (int i = 0; i < 16; i++) samples[i] = (int16_t)(100 * i);
    mixer_write_channel(m, 0, samples, 16);
    mixer_channel_start(m, 0);

    int16_t out[16];
    mixer_render(m, out, 8);

    /* For cubic at frac=0: output is exactly p1 (the "current" tap).
     * For sample i, frac=0 case reads p1 = samples[i].
     * With 2x downsample at integer phase, frame N has frac=0 reading
     * sample 2N. */
    for (int f = 0; f < 8; f++) {
        int16_t expected = (int16_t)(100 * 2 * f);
        /* Allow tolerance — first frame uses zero-history which can
         * slightly affect cubic output, but at frac=0 the result is
         * still p1 regardless. */
        ASSERT(out[f*2] >= expected - 1 && out[f*2] <= expected + 1);
    }
    mixer_destroy(m);
}

/* ============================================================
 *  Stage 3: loop + resample
 * ============================================================ */

static void test_loop_with_2x_upsample(void) {
    /* Loop 4 samples at half the output rate. Each loop iteration
     * takes 8 output frames. */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 16,
                                .volume = Q15_ONE,
                                .source_sample_rate = 22050,
                                .pan = 0,
                                .loop = true,
                                .interp = MIXER_INTERP_LINEAR };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t loop[4] = { 1000, 2000, 3000, 4000 };
    mixer_write_channel(m, 0, loop, 4);
    mixer_channel_start(m, 0);

    /* Render enough frames to wrap the loop at least twice. */
    int16_t out[64];
    mixer_render(m, out, 32);

    /* The output is too complex to verify per-sample, but we can
     * verify that no underrun occurred (no long stretches of zero). */
    int nonzero_count = 0;
    for (int f = 0; f < 32; f++) {
        if (out[f*2] != 0) nonzero_count++;
    }
    ASSERT(nonzero_count > 25);  /* most samples should be nonzero */
    mixer_destroy(m);
}

/* ============================================================
 *  Stage 3: stronger interpolation correctness tests
 *
 *  These verify the interpolator at fractional positions, not just
 *  at integer-phase degenerate cases.
 * ============================================================ */

static void test_linear_constant_signal(void) {
    /* A constant signal should remain exactly constant under any
     * resampling ratio. Linear interpolation between equal values
     * is the value itself. */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE,
                                .source_sample_rate = 30000,  /* arbitrary, non-integer ratio */
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t samples[32];
    for (int i = 0; i < 32; i++) samples[i] = 5000;   /* constant */
    mixer_write_channel(m, 0, samples, 32);
    mixer_channel_start(m, 0);

    int16_t out[32];
    mixer_render(m, out, 16);

    /* Every sample should be exactly 5000 (or extremely close;
     * linear at frac on a constant signal returns the constant). */
    for (int f = 0; f < 16; f++) {
        ASSERT(out[f*2] >= 4999 && out[f*2] <= 5001);
    }
    mixer_destroy(m);
}

static void test_cubic_constant_signal(void) {
    /* Cubic interpolation of a constant signal should also remain
     * constant (cubic reproduces constants exactly). The exception
     * is the very first sample, where tap[0] is initialized to 0
     * (silence as the "before-the-start" history). After the tap
     * window fills, all subsequent samples should match the constant. */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE,
                                .source_sample_rate = 30000,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_CUBIC };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t samples[32];
    for (int i = 0; i < 32; i++) samples[i] = 5000;
    mixer_write_channel(m, 0, samples, 32);
    mixer_channel_start(m, 0);

    int16_t out[32];
    mixer_render(m, out, 16);

    /* Skip the first 2 frames (history settles in). Remaining frames
     * should be the constant within q15 rounding tolerance. */
    for (int f = 3; f < 16; f++) {
        ASSERT(out[f*2] >= 4990 && out[f*2] <= 5010);
    }
    mixer_destroy(m);
}

static void test_linear_ramp_at_half_phase(void) {
    /* At exactly 2x upsample (step = 0.5), every other output sample
     * has frac=0.5. For a linear ramp source (samples = [0, 100, 200,
     * 300, ...]), linear interpolation at frac=0.5 between two
     * adjacent ramp samples gives the midpoint. */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 32,
                                .volume = Q15_ONE,
                                .source_sample_rate = 22050,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t ramp[16];
    for (int i = 0; i < 16; i++) ramp[i] = (int16_t)(100 * i);
    mixer_write_channel(m, 0, ramp, 16);
    mixer_channel_start(m, 0);

    int16_t out[32];
    mixer_render(m, out, 16);

    /* Linear interpolation of a ramp gives the ramp values exactly
     * at every position. Output should be 0, 50, 100, 150, ... */
    for (int f = 0; f < 16; f++) {
        int16_t expected = (int16_t)(50 * f);
        ASSERT(out[f*2] >= expected - 1 && out[f*2] <= expected + 1);
    }
    mixer_destroy(m);
}

static void test_cubic_linear_ramp(void) {
    /* Cubic interpolation of a linear ramp should produce the ramp
     * exactly (cubic polynomials reproduce affine functions). */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 32,
                                .volume = Q15_ONE,
                                .source_sample_rate = 22050,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_CUBIC };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t ramp[16];
    for (int i = 0; i < 16; i++) ramp[i] = (int16_t)(100 * i);
    mixer_write_channel(m, 0, ramp, 16);
    mixer_channel_start(m, 0);

    int16_t out[32];
    mixer_render(m, out, 16);

    /* Skip first 2 frames (history settling). Then the ramp should
     * be exact within q15 rounding. */
    for (int f = 3; f < 16; f++) {
        int16_t expected = (int16_t)(50 * f);
        ASSERT(out[f*2] >= expected - 2 && out[f*2] <= expected + 2);
    }
    mixer_destroy(m);
}

static void test_linear_specific_fractional_values(void) {
    /* Test linear interpolation at specific known fractional phases.
     * Use source_rate = 11025, output = 44100 — step = 0.25. Each
     * output frame's phase advances by 0.25. The first 4 outputs
     * use samples 0 and 1 with fracs 0, 0.25, 0.5, 0.75.
     *
     * For samples [0]=1000, [1]=2000:
     *   frac=0.00 → 1000
     *   frac=0.25 → 1250
     *   frac=0.50 → 1500
     *   frac=0.75 → 1750
     */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 32,
                                .volume = Q15_ONE,
                                .source_sample_rate = 11025,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    AudioMixer *m = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    int16_t samples[8] = { 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000 };
    mixer_write_channel(m, 0, samples, 8);
    mixer_channel_start(m, 0);

    int16_t out[32];
    mixer_render(m, out, 16);

    /* Expected output: 0.25-step linear interpolation through the ramp.
     *   frame 0:  phase 0.00, samples[0]=1000, frac=0 → 1000
     *   frame 1:  phase 0.25, frac=0.25       → 1250
     *   frame 2:  phase 0.50                  → 1500
     *   frame 3:  phase 0.75                  → 1750
     *   frame 4:  phase 1.00, samples[1]=2000, frac=0 → 2000
     *   ...                                              → 2250, 2500, 2750
     */
    int expected[] = { 1000, 1250, 1500, 1750, 2000, 2250, 2500, 2750,
                       3000, 3250, 3500, 3750, 4000, 4250, 4500, 4750 };
    for (int f = 0; f < 16; f++) {
        ASSERT(out[f*2] >= expected[f] - 2 && out[f*2] <= expected[f] + 2);
    }
    mixer_destroy(m);
}

/* ============================================================
 *  v2: sync / drift correction
 * ============================================================ */

static MixerSyncConfig default_sync_config(void) {
    /* SNES master clock for the test setup: 21477272 Hz (nominal). */
    MixerSyncConfig s = {
        .external_ticks_per_second = 21477272,
        .correction_smoothing      = Q15_ONE / 4,   /* fast convergence */
        .max_correction            = Q15_ONE / 100, /* 1% max */
    };
    return s;
}

static void test_sync_observe_on_nonsync_mixer_is_noop(void) {
    /* Calling mixer_observe_sync on a non-sync mixer should return 0
     * and do nothing. */
    AudioMixer *m = make_simple_mixer(MIXER_SRC_PCM16_MONO, 64,
                                       fmt_q15_stereo(), 0);
    int32_t ppm = mixer_observe_sync(m, 1000, 12345);
    ASSERT_EQ_INT(0, (int)ppm);
    mixer_destroy(m);
}

static void test_sync_create_with_null_config_fails(void) {
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE,
                                .source_sample_rate = 0,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    AudioMixer *m = mixer_create_with_sync(&cfg, 1, 44100,
                                            fmt_q15_stereo(), 0,
                                            NULL, NULL, NULL);
    ASSERT_NULL(m);
}

static void test_sync_create_and_destroy(void) {
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE,
                                .source_sample_rate = 0,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    MixerSyncConfig sc = default_sync_config();
    AudioMixer *m = mixer_create_with_sync(&cfg, 1, 44100,
                                            fmt_q15_stereo(), 0,
                                            &sc, NULL, NULL);
    ASSERT_NOT_NULL(m);
    /* Before any observations, correction is 0. */
    ASSERT_EQ_INT(0, (int)mixer_observe_sync(m, 0, 0));
    mixer_destroy(m);
}

/* Helper: simulate an external counter advancing with a known drift
 * ratio. external_drift_ppm of +100 means external clock is 100 PPM
 * faster than nominal; +0 means nominal; -50 means 50 PPM slow. */
static uint64_t external_for(uint64_t internal_frames,
                              int output_rate, uint64_t ext_per_sec,
                              int drift_ppm) {
    /* nominal_external = internal * ext_per_sec / output_rate */
    uint64_t nominal = internal_frames * ext_per_sec / (uint64_t)output_rate;
    /* drift_ppm is in parts per million; convert to a multiplier */
    /* actual = nominal * (1 + drift_ppm / 1000000)
              = nominal + (nominal * drift_ppm / 1000000) */
    int64_t adjustment = (int64_t)nominal * (int64_t)drift_ppm / 1000000;
    return nominal + (uint64_t)adjustment;
}

static void test_sync_zero_drift_correction_stays_near_zero(void) {
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE,
                                .source_sample_rate = 0,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    MixerSyncConfig sc = default_sync_config();
    AudioMixer *m = mixer_create_with_sync(&cfg, 1, 44100,
                                            fmt_q15_stereo(), 0,
                                            &sc, NULL, NULL);

    /* Feed 20 observations with 0 drift. Correction should stay
     * near zero (some integer-division roundoff per call is OK). */
    int32_t last_ppm = 0;
    for (uint64_t i = 1; i <= 20; i++) {
        uint64_t internal = i * 1000;
        uint64_t external = external_for(internal, 44100, 21477272, 0);
        last_ppm = mixer_observe_sync(m, internal, external);
    }
    /* Correction should be small — within ±100 PPM of zero. */
    ASSERT(last_ppm > -100 && last_ppm < 100);
    mixer_destroy(m);
}

static void test_sync_positive_drift_converges_positive(void) {
    /* External clock is 500 PPM faster than nominal. After many
     * observations the correction should converge near +500 PPM. */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE,
                                .source_sample_rate = 0,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    MixerSyncConfig sc = default_sync_config();
    AudioMixer *m = mixer_create_with_sync(&cfg, 1, 44100,
                                            fmt_q15_stereo(), 0,
                                            &sc, NULL, NULL);

    int32_t last_ppm = 0;
    for (uint64_t i = 1; i <= 50; i++) {
        uint64_t internal = i * 1000;
        uint64_t external = external_for(internal, 44100, 21477272, 500);
        last_ppm = mixer_observe_sync(m, internal, external);
    }
    /* Should have converged toward +500 PPM. With smoothing=Q15/4 and
     * 50 observations, we should be well into the target range. */
    ASSERT(last_ppm > 300 && last_ppm < 700);
    mixer_destroy(m);
}

static void test_sync_negative_drift_converges_negative(void) {
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE,
                                .source_sample_rate = 0,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    MixerSyncConfig sc = default_sync_config();
    AudioMixer *m = mixer_create_with_sync(&cfg, 1, 44100,
                                            fmt_q15_stereo(), 0,
                                            &sc, NULL, NULL);

    int32_t last_ppm = 0;
    for (uint64_t i = 1; i <= 50; i++) {
        uint64_t internal = i * 1000;
        uint64_t external = external_for(internal, 44100, 21477272, -300);
        last_ppm = mixer_observe_sync(m, internal, external);
    }
    ASSERT(last_ppm < -150 && last_ppm > -500);
    mixer_destroy(m);
}

static void test_sync_clamps_to_max_correction(void) {
    /* Even with extreme drift, correction shouldn't exceed max_correction.
     * max = Q15_ONE/100 = ~10000 PPM. */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE,
                                .source_sample_rate = 0,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    MixerSyncConfig sc = default_sync_config();
    sc.max_correction = Q15_ONE / 100;   /* 1% = 10000 PPM */
    AudioMixer *m = mixer_create_with_sync(&cfg, 1, 44100,
                                            fmt_q15_stereo(), 0,
                                            &sc, NULL, NULL);

    int32_t last_ppm = 0;
    /* Simulate 5% drift (50000 PPM) — way more than max_correction. */
    for (uint64_t i = 1; i <= 100; i++) {
        uint64_t internal = i * 1000;
        uint64_t external = external_for(internal, 44100, 21477272, 50000);
        last_ppm = mixer_observe_sync(m, internal, external);
    }
    /* Should be clamped near +10000 PPM (max), not running off. */
    ASSERT(last_ppm > 9000 && last_ppm <= 11000);
    mixer_destroy(m);
}

static void test_sync_reset_clears_correction(void) {
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 64,
                                .volume = Q15_ONE,
                                .source_sample_rate = 0,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    MixerSyncConfig sc = default_sync_config();
    AudioMixer *m = mixer_create_with_sync(&cfg, 1, 44100,
                                            fmt_q15_stereo(), 0,
                                            &sc, NULL, NULL);

    /* Drive correction to a non-zero value. */
    for (uint64_t i = 1; i <= 30; i++) {
        uint64_t internal = i * 1000;
        uint64_t external = external_for(internal, 44100, 21477272, 1000);
        mixer_observe_sync(m, internal, external);
    }

    /* Reset. */
    mixer_reset_sync(m);

    /* After reset, the next single observation returns 0 (the
     * baseline-setting call) and correction is 0. */
    int32_t ppm = mixer_observe_sync(m, 100000, 48700000);
    ASSERT_EQ_INT(0, (int)ppm);
    mixer_destroy(m);
}

static void test_sync_audio_still_plays_correctly(void) {
    /* With sync enabled and zero drift, the mixer should produce
     * the same audio as a non-sync mixer (modulo tiny rounding from
     * the interpolation path being engaged). Drift correction = 0
     * means effective_step = base_step = 1.0 exactly. */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                                .buffer_samples = 32,
                                .volume = Q15_ONE,
                                .source_sample_rate = 0,
                                .pan = 0,
                                .loop = false,
                                .interp = MIXER_INTERP_LINEAR };
    MixerSyncConfig sc = default_sync_config();
    AudioMixer *m = mixer_create_with_sync(&cfg, 1, 44100,
                                            fmt_q15_stereo(), 0,
                                            &sc, NULL, NULL);

    int16_t samples[8] = { 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000 };
    mixer_write_channel(m, 0, samples, 8);
    mixer_channel_start(m, 0);

    int16_t out[16];
    mixer_render(m, out, 8);

    /* Linear interpolator with step=1.0 and frac=0 gives the exact
     * sample (no rounding). */
    for (int f = 0; f < 8; f++) {
        ASSERT_EQ_INT(samples[f], out[f*2]);
        ASSERT_EQ_INT(samples[f], out[f*2 + 1]);
    }
    mixer_destroy(m);
}

int main(void) {
    TEST_SUITE("audio_mixer (v2)");

    /* Lifecycle */
    RUN(test_create_and_destroy);
    RUN(test_destroy_null_safe);
    RUN(test_create_invalid_format_returns_null);
    RUN(test_custom_allocator_routes_through);
    RUN(test_null_allocator_uses_default);

    /* Source format conversions */
    RUN(test_16mono_to_q15_stereo);
    RUN(test_8s_mono_source);
    RUN(test_8u_mono_source);
    RUN(test_16stereo_source);
    RUN(test_8s_stereo_source);
    RUN(test_8u_stereo_source);

    /* Channel mapping */
    RUN(test_stereo_source_to_mono_output);
    RUN(test_mono_source_to_mono_output);

    /* Output format conversions */
    RUN(test_q15_to_8u_output);
    RUN(test_q15_to_8s_output);
    RUN(test_q15_to_12u_output);

    /* Mixing */
    RUN(test_two_channels_summed);
    RUN(test_saturation_no_headroom);
    RUN(test_headroom_prevents_clipping);

    /* Channel control */
    RUN(test_inactive_channel_silent);
    RUN(test_muted_channel_consumes_samples);
    RUN(test_buffer_underrun_silent);
    RUN(test_channel_reset_clears_buffer);
    RUN(test_out_of_range_indices_safe);

    /* Volume */
    RUN(test_volume_zero);
    RUN(test_volume_half);

    /* Stage 3: pan */
    RUN(test_pan_center_unchanged);
    RUN(test_pan_full_left);
    RUN(test_pan_full_right);

    /* Stage 3: looping */
    RUN(test_loop_no_resample_wraps);

    /* Stage 3: resampling */
    RUN(test_2x_upsample_linear);
    RUN(test_2x_downsample_linear);
    RUN(test_cubic_at_integer_phase);

    /* Stage 3: combination */
    RUN(test_loop_with_2x_upsample);

    /* Stage 3: interpolation correctness */
    RUN(test_linear_constant_signal);
    RUN(test_cubic_constant_signal);
    RUN(test_linear_ramp_at_half_phase);
    RUN(test_cubic_linear_ramp);
    RUN(test_linear_specific_fractional_values);

    /* v2: sync */
    RUN(test_sync_observe_on_nonsync_mixer_is_noop);
    RUN(test_sync_create_with_null_config_fails);
    RUN(test_sync_create_and_destroy);
    RUN(test_sync_zero_drift_correction_stays_near_zero);
    RUN(test_sync_positive_drift_converges_positive);
    RUN(test_sync_negative_drift_converges_negative);
    RUN(test_sync_clamps_to_max_correction);
    RUN(test_sync_reset_clears_correction);
    RUN(test_sync_audio_still_plays_correctly);

    return TEST_SUITE_RESULT();
}
