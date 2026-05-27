/* ============================================================
 *  test_audio_pool_stream_integration.c
 *
 *  End-to-end proof that the pool adapter plugs into the REAL music
 *  player + mixer: load a known ramp into a pool object, configure a
 *  player whose stream_fn is audio_pool_stream_read, prime + play,
 *  render through the mixer, and confirm the output is non-silent
 *  and tracks the source (i.e. the player really pulled the pool's
 *  data through the adapter into the mixer).
 *
 *  This is deliberately light — the music player's own behaviour is
 *  covered by test_music_player.c; here we only prove the SEAM
 *  (pool -> adapter -> player -> mixer) is wired correctly.
 *
 *  Build:
 *    cc -std=c11 -Iinclude -o t \
 *       src/audio/tests/test_audio_pool_stream_integration.c \
 *       src/audio/audio_pool_stream.c src/audio/audio_pool.c \
 *       src/audio/music_player.c src/audio/audio_mixer.c \
 *       src/containers/ring_buffer.c
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_pool_stream.h"
#include "audio/music_player.h"
#include "audio/audio_mixer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

#define MIXER_BUF_SAMPLES   64
#define STREAM_BUF_SAMPLES  32
#define HEAD_SAMPLES        8
#define POOL_REGION_BYTES   (64 * 1024)

static MixerOutputFormat fmt_q15_stereo(void) {
    MixerOutputFormat f = { .bits = 16, .is_signed = true,
                            .storage_bits = 16, .channels = 2 };
    return f;
}

int main(void) {
    /* ---- pool with one mono16 object holding a ramp ---- */
    AudioPool pool;
    uint8_t *region = malloc(POOL_REGION_BYTES);
    if (audio_pool_init(&pool, region, POOL_REGION_BYTES) != AUDIO_POOL_OK) {
        printf("  pool init failed\n"); return 2;
    }

    /* 256 mono16 frames = 512 bytes, a rising ramp so output is
     * clearly non-zero and ordered. */
    enum { NFRAMES = 256 };
    int16_t ramp[NFRAMES];
    for (int i = 0; i < NFRAMES; i++) ramp[i] = (int16_t)(i * 64); /* up to ~16k */
    AudioObjHandle obj;
    if (audio_pool_alloc(&pool, sizeof(ramp), 1, &obj) != AUDIO_POOL_OK) {
        printf("  obj alloc failed\n"); return 2;
    }
    uint32_t wrote = 0;
    audio_pool_write(&pool, obj, 0, ramp, sizeof(ramp), &wrote);
    CHECK(wrote == sizeof(ramp), "ramp written into pool object");

    /* ---- adapter context: stream_id 0 -> our object ---- */
    AudioPoolStreamCtx ctx;
    CHECK(audio_pool_stream_init(&ctx, &pool, MIXER_SRC_PCM16_MONO),
          "adapter ctx init");
    CHECK(audio_pool_stream_bind(&ctx, 0, obj), "bind stream 0 -> object");

    /* ---- real mixer + player, stream_fn = the adapter ---- */
    MixerChannelConfig cfg = { .format = MIXER_SRC_PCM16_MONO,
                               .buffer_samples = MIXER_BUF_SAMPLES,
                               .volume = Q15_ONE };
    AudioMixer *mixer = mixer_create(&cfg, 1, 44100, fmt_q15_stereo(), 0);
    CHECK(mixer != NULL, "mixer created");

    int16_t streaming_buf[STREAM_BUF_SAMPLES];
    int16_t intro_head[HEAD_SAMPLES];

    MusicPlayerConfig mpc = {
        .mixer            = mixer,
        .mixer_channel    = 0,
        .format           = MIXER_SRC_PCM16_MONO,
        .stream_fn        = audio_pool_stream_read,   /* THE BRIDGE */
        .stream_user_data = &ctx,
        .intro_stream_id  = 0,                         /* -> our object */
        .loop_stream_id   = MUSIC_STREAM_NONE,
        .streaming_buffer = streaming_buf,
        .streaming_buffer_samples = STREAM_BUF_SAMPLES,
        .intro_head_buffer = intro_head,
        .intro_head_samples = HEAD_SAMPLES,
        .loop_head_buffer = NULL,
        .loop_head_samples = 0,
    };
    MusicPlayer *player = music_create(&mpc);
    CHECK(player != NULL, "player created with pool-backed stream_fn");

    /* Prime the intro head — this calls the adapter, which reads the
     * first HEAD_SAMPLES frames from the pool object. A success here
     * means pool -> adapter -> player priming works. */
    int pr = music_prime_intro(player);
    CHECK(pr == 0, "prime intro pulled head samples from the pool");

    /* Verify the pinned head actually contains the pool's ramp data
     * (the player copied it from the pool via the adapter). */
    int head_ok = 1;
    for (int i = 0; i < HEAD_SAMPLES; i++)
        if (intro_head[i] != (int16_t)(i * 64)) head_ok = 0;
    CHECK(head_ok, "pinned head holds the pool object's ramp data");

    /* Play + pump + render; output should be non-silent. */
    music_play(player);
    music_update(player);

    int16_t out[MIXER_BUF_SAMPLES * 2];   /* stereo */
    memset(out, 0, sizeof(out));
    mixer_render(mixer, out, MIXER_BUF_SAMPLES);

    int nonzero = 0;
    for (int i = 0; i < MIXER_BUF_SAMPLES * 2; i++) if (out[i] != 0) nonzero++;
    CHECK(nonzero > 0, "mixer output is non-silent (pool audio reached output)");

    music_destroy(player);
    mixer_destroy(mixer);
    audio_pool_unref(&pool, obj, NULL);
    audio_pool_destroy(&pool);
    free(region);

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
