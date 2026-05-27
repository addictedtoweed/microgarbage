/* ============================================================
 *  test_audio_pool_stream.c — pool<->player bridge tests
 *
 *  Drives audio_pool_stream_read exactly as the music player would
 *  (frame-based requests with a stream_id), backed by real pool
 *  objects, and verifies the bridge's contract:
 *    - stream_id routes to the correct object handle
 *    - frame<->byte conversion is correct for each format
 *    - data read back matches what was loaded (across block chains)
 *    - offset (in frames) maps to the right byte offset
 *    - reads clamp at object end and return a frame count
 *    - unbound / unknown stream_id returns 0 (silence)
 *
 *  Small block size via -DAUDIO_POOL_BLOCK_SIZE so objects span
 *  multiple blocks, exercising the chain walk through the adapter.
 *
 *  Build:
 *    cc -std=c11 -DAUDIO_POOL_BLOCK_SIZE=64 -Iinclude -o t \
 *       src/audio/tests/test_audio_pool_stream.c src/audio/audio_pool_stream.c \
 *       src/audio/audio_pool.c
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_pool_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

#define REGION_BYTES (32 * AUDIO_POOL_BLOCK_SIZE)

/* Load `nbytes` of a known pattern into a fresh pool object. */
static AudioObjHandle make_object(AudioPool *p, uint32_t nbytes) {
    AudioObjHandle h;
    if (audio_pool_alloc(p, nbytes, 1, &h) != AUDIO_POOL_OK)
        return AUDIO_POOL_HANDLE_NONE;
    uint8_t *pat = malloc(nbytes);
    for (uint32_t i = 0; i < nbytes; i++) pat[i] = (uint8_t)(i * 13 + 5);
    uint32_t w = 0;
    audio_pool_write(p, h, 0, pat, nbytes, &w);
    free(pat);
    return h;
}

/* ---- frame<->byte conversion per format ---- */
static void test_frame_byte_conversion(void) {
    AudioPool pool;
    uint8_t *region = malloc(REGION_BYTES);
    audio_pool_init(&pool, region, REGION_BYTES);

    /* PCM16_STEREO: 4 bytes/frame. Make a 100-frame object (400 B). */
    uint32_t frames = 100, bpf = 4, nbytes = frames * bpf;
    AudioObjHandle h = make_object(&pool, nbytes);

    AudioPoolStreamCtx ctx;
    CHECK(audio_pool_stream_init(&ctx, &pool, MIXER_SRC_PCM16_STEREO),
          "init stereo16 ctx");
    CHECK(ctx.bytes_per_frame == 4, "stereo16 bytes_per_frame = 4");
    CHECK(audio_pool_stream_bind(&ctx, 7, h), "bind stream_id 7");

    /* Request 10 frames from offset 0 -> 40 bytes. */
    uint8_t dst[400];
    size_t got = audio_pool_stream_read(&ctx, 7, dst, 10, 0);
    CHECK(got == 10, "10 frames requested -> 10 returned");

    /* Verify those 40 bytes match the source pattern. */
    int ok = 1;
    for (uint32_t i = 0; i < 40; i++) if (dst[i] != (uint8_t)(i*13+5)) ok = 0;
    CHECK(ok, "stereo16 data correct for frames 0..9");

    /* Request 10 frames from frame offset 50 -> byte offset 200. */
    got = audio_pool_stream_read(&ctx, 7, dst, 10, 50);
    CHECK(got == 10, "10 frames from offset 50 returned");
    ok = 1;
    for (uint32_t i = 0; i < 40; i++)
        if (dst[i] != (uint8_t)((200 + i)*13 + 5)) ok = 0;
    CHECK(ok, "offset-50 frames map to byte offset 200");

    audio_pool_unref(&pool, h, NULL);
    audio_pool_destroy(&pool);
    free(region);
}

/* ---- mono8 (1 byte/frame) conversion ---- */
static void test_mono8_conversion(void) {
    AudioPool pool;
    uint8_t *region = malloc(REGION_BYTES);
    audio_pool_init(&pool, region, REGION_BYTES);

    uint32_t frames = 200;                 /* 1 byte/frame -> 200 B */
    AudioObjHandle h = make_object(&pool, frames);

    AudioPoolStreamCtx ctx;
    audio_pool_stream_init(&ctx, &pool, MIXER_SRC_PCM8U_MONO);
    CHECK(ctx.bytes_per_frame == 1, "mono8 bytes_per_frame = 1");
    audio_pool_stream_bind(&ctx, 3, h);

    uint8_t dst[200];
    size_t got = audio_pool_stream_read(&ctx, 3, dst, 50, 25);
    CHECK(got == 50, "mono8: 50 frames from offset 25");
    int ok = 1;
    for (uint32_t i = 0; i < 50; i++)
        if (dst[i] != (uint8_t)((25 + i)*13 + 5)) ok = 0;
    CHECK(ok, "mono8: frame offset == byte offset, data correct");

    audio_pool_unref(&pool, h, NULL);
    audio_pool_destroy(&pool);
    free(region);
}

/* ---- multi-block chain read through the adapter ---- */
static void test_chain_read_through_adapter(void) {
    AudioPool pool;
    uint8_t *region = malloc(REGION_BYTES);
    audio_pool_init(&pool, region, REGION_BYTES);

    /* Object spanning several blocks. mono16: 2 bytes/frame. */
    uint32_t nbytes = AUDIO_POOL_BLOCK_SIZE * 5 + 30;   /* 6 blocks */
    AudioObjHandle h = make_object(&pool, nbytes);
    uint32_t frames = nbytes / 2;

    AudioPoolStreamCtx ctx;
    audio_pool_stream_init(&ctx, &pool, MIXER_SRC_PCM16_MONO);
    audio_pool_stream_bind(&ctx, 0, h);

    /* Read the whole thing in one call; must walk the chain. */
    uint8_t *dst = malloc(nbytes);
    memset(dst, 0, nbytes);
    size_t got = audio_pool_stream_read(&ctx, 0, dst, frames, 0);
    CHECK(got == frames, "whole multi-block object read as frames");

    int ok = 1;
    for (uint32_t i = 0; i < frames * 2; i++)
        if (dst[i] != (uint8_t)(i*13 + 5)) ok = 0;
    CHECK(ok, "multi-block chain data intact through adapter");

    free(dst);
    audio_pool_unref(&pool, h, NULL);
    audio_pool_destroy(&pool);
    free(region);
}

/* ---- clamp at object end ---- */
static void test_clamp_at_end(void) {
    AudioPool pool;
    uint8_t *region = malloc(REGION_BYTES);
    audio_pool_init(&pool, region, REGION_BYTES);

    uint32_t frames = 100;                 /* mono16 -> 200 B */
    AudioObjHandle h = make_object(&pool, frames * 2);

    AudioPoolStreamCtx ctx;
    audio_pool_stream_init(&ctx, &pool, MIXER_SRC_PCM16_MONO);
    audio_pool_stream_bind(&ctx, 1, h);

    uint8_t dst[400];
    /* Ask for 50 frames starting 10 before the end -> only 10 left. */
    size_t got = audio_pool_stream_read(&ctx, 1, dst, 50, frames - 10);
    CHECK(got == 10, "read clamps to frames remaining at object end");

    /* Read entirely past the end -> 0 frames. */
    got = audio_pool_stream_read(&ctx, 1, dst, 10, frames + 5);
    CHECK(got == 0, "read past object end returns 0 frames");

    audio_pool_unref(&pool, h, NULL);
    audio_pool_destroy(&pool);
    free(region);
}

/* ---- two streams (intro + loop) routed by id ---- */
static void test_two_streams_routing(void) {
    AudioPool pool;
    uint8_t *region = malloc(REGION_BYTES);
    audio_pool_init(&pool, region, REGION_BYTES);

    /* Two distinct objects, distinct patterns by being different
     * sizes & offsets won't differ in pattern fn, so tag via size. */
    AudioObjHandle intro = make_object(&pool, 64);   /* mono16: 32 frames */
    AudioObjHandle loop  = make_object(&pool, 128);  /* 64 frames */

    AudioPoolStreamCtx ctx;
    audio_pool_stream_init(&ctx, &pool, MIXER_SRC_PCM16_MONO);
    CHECK(audio_pool_stream_bind(&ctx, 10, intro), "bind intro id 10");
    CHECK(audio_pool_stream_bind(&ctx, 20, loop),  "bind loop id 20");

    uint8_t dst[8];
    /* Both patterns start identically (same fn from offset 0), so to
     * prove routing we read past intro's length on id 10 (clamps to
     * 32 frames) vs id 20 (has 64 frames available). */
    size_t gi = audio_pool_stream_read(&ctx, 10, dst, 4, 30); /* intro: 2 left */
    size_t gl = audio_pool_stream_read(&ctx, 20, dst, 4, 30); /* loop: 4 left */
    CHECK(gi == 2, "id 10 routed to intro (clamps at 32 frames)");
    CHECK(gl == 4, "id 20 routed to loop (has 64 frames)");

    /* Unknown / unbound id -> 0 (silence). */
    size_t gu = audio_pool_stream_read(&ctx, 99, dst, 4, 0);
    CHECK(gu == 0, "unbound stream_id returns 0 (silence)");

    /* Third distinct id can't bind (only two slots). */
    AudioObjHandle extra = make_object(&pool, 64);
    CHECK(!audio_pool_stream_bind(&ctx, 30, extra), "third stream_id rejected");

    audio_pool_unref(&pool, intro, NULL);
    audio_pool_unref(&pool, loop, NULL);
    audio_pool_unref(&pool, extra, NULL);
    audio_pool_destroy(&pool);
    free(region);
}

/* ---- rebind / clear ---- */
static void test_rebind_and_clear(void) {
    AudioPool pool;
    uint8_t *region = malloc(REGION_BYTES);
    audio_pool_init(&pool, region, REGION_BYTES);

    AudioObjHandle a = make_object(&pool, 64);
    AudioObjHandle b = make_object(&pool, 64);

    AudioPoolStreamCtx ctx;
    audio_pool_stream_init(&ctx, &pool, MIXER_SRC_PCM16_MONO);
    audio_pool_stream_bind(&ctx, 5, a);

    uint8_t dst[8];
    CHECK(audio_pool_stream_read(&ctx, 5, dst, 1, 0) == 1, "id 5 reads from a");

    /* Rebind id 5 to b — should not consume the second slot. */
    audio_pool_stream_bind(&ctx, 5, b);
    CHECK(audio_pool_stream_read(&ctx, 5, dst, 1, 0) == 1, "id 5 now reads from b");

    /* Now a free slot remains for another id. */
    CHECK(audio_pool_stream_bind(&ctx, 6, a), "second id binds in freed slot");

    /* Clear id 5. */
    audio_pool_stream_bind(&ctx, 5, AUDIO_POOL_HANDLE_NONE);
    CHECK(audio_pool_stream_read(&ctx, 5, dst, 1, 0) == 0, "cleared id reads 0");

    audio_pool_unref(&pool, a, NULL);
    audio_pool_unref(&pool, b, NULL);
    audio_pool_destroy(&pool);
    free(region);
}

int main(void) {
    test_frame_byte_conversion();
    test_mono8_conversion();
    test_chain_read_through_adapter();
    test_clamp_at_end();
    test_two_streams_routing();
    test_rebind_and_clear();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
