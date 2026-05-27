/* ============================================================
 *  test_audio_file_stream.c — file-backed stream source tests
 *
 *  Drives audio_file_stream with a mock AudioFileReader over an
 *  in-memory synthetic WAV. Verifies stereo output: stereo preserved
 *  (NOT downmixed), mono promoted to L==R, plus chunked reads larger
 *  than the scratch, seek/offset, and end-of-file.
 *
 *  Build:
 *    cc -std=c11 -Iinclude -o t \
 *       src/audio/tests/test_audio_file_stream.c src/audio/audio_file_stream.c \
 *       src/audio/audio_wav_read.c
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_file_stream.h"
#include "audio/audio_sink.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

/* ---- in-memory file + mock reader ---- */
typedef struct { const uint8_t *buf; uint32_t len; uint32_t pos; } MemFile;

static void *m_open(void *ctx, const char *path) {
    (void)path;
    MemFile *mf = (MemFile *)ctx;
    mf->pos = 0;
    return mf;
}
static uint32_t m_read(void *ctx, void *fh, void *dst, uint32_t bytes) {
    (void)ctx;
    MemFile *mf = (MemFile *)fh;
    uint32_t avail = mf->len - mf->pos;
    uint32_t n = (bytes < avail) ? bytes : avail;
    memcpy(dst, mf->buf + mf->pos, n);
    mf->pos += n;
    return n;
}
static bool m_seek(void *ctx, void *fh, uint32_t off) {
    (void)ctx;
    MemFile *mf = (MemFile *)fh;
    if (off > mf->len) return false;
    mf->pos = off;
    return true;
}
static void m_close(void *ctx, void *fh) { (void)ctx; (void)fh; }

/* ---- synthetic WAV builder (16-bit PCM) ---- */
static uint32_t put_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
    return 4;
}
static uint32_t put_u16(uint8_t *p, uint16_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); return 2;
}
static uint32_t build_wav16(uint8_t *b, int ch, uint32_t rate,
                            const int16_t *inter, uint32_t nframes) {
    uint32_t data = nframes * (uint32_t)ch * 2u, p = 0;
    memcpy(b+p,"RIFF",4); p+=4; p+=put_u32(b+p, 36+data); memcpy(b+p,"WAVE",4); p+=4;
    memcpy(b+p,"fmt ",4); p+=4; p+=put_u32(b+p,16); p+=put_u16(b+p,1);
    p+=put_u16(b+p,(uint16_t)ch); p+=put_u32(b+p,rate);
    p+=put_u32(b+p, rate*(uint32_t)ch*2u); p+=put_u16(b+p,(uint16_t)(ch*2)); p+=put_u16(b+p,16);
    memcpy(b+p,"data",4); p+=4; p+=put_u32(b+p,data);
    for (uint32_t i = 0; i < nframes*(uint32_t)ch; i++) p += put_u16(b+p, (uint16_t)inter[i]);
    return p;
}

static uint8_t  g_wav[128*1024];
static int16_t  g_src[10000];
static int16_t  g_dst[12000];   /* stereo16: 2 int16 per frame */

#define N_MONO 5000

static void test_mono_promoted_to_stereo(void) {
    for (int i = 0; i < N_MONO; i++) g_src[i] = (int16_t)i;
    uint32_t len = build_wav16(g_wav, 1, 44100, g_src, N_MONO);

    MemFile mf = { g_wav, len, 0 };
    AudioFileReader rd = { m_open, m_read, m_seek, m_close, &mf };
    AudioFileStreamCtx ctx;
    CHECK(audio_file_stream_open(&ctx, &rd, "x.wav"), "open mono wav");
    CHECK(ctx.channels == 1 && ctx.bits == 16, "parsed mono/16");
    CHECK(ctx.data_bytes == N_MONO * 2u, "true data size from chunk header");

    /* 10 frames at offset 0: mono i -> L==R==i */
    size_t got = audio_file_stream_read(&ctx, 0, g_dst, 10, 0);
    CHECK(got == 10, "mono read 10 frames");
    int ok = 1;
    for (int i = 0; i < 10; i++)
        if (g_dst[i*2] != i || g_dst[i*2+1] != i) ok = 0;
    CHECK(ok, "mono promoted to L==R");

    /* offset seek */
    got = audio_file_stream_read(&ctx, 0, g_dst, 10, 100);
    ok = 1;
    for (int i = 0; i < 10; i++)
        if (g_dst[i*2] != 100+i || g_dst[i*2+1] != 100+i) ok = 0;
    CHECK(got == 10 && ok, "seek to offset 100");

    /* full read, chunked beyond the scratch */
    got = audio_file_stream_read(&ctx, 0, g_dst, N_MONO, 0);
    CHECK(got == N_MONO, "chunked full read returns all frames");
    CHECK(g_dst[0] == 0 && g_dst[(N_MONO-1)*2] == N_MONO-1
          && g_dst[(N_MONO-1)*2+1] == N_MONO-1, "chunked content correct end-to-end");

    /* EOF */
    CHECK(audio_file_stream_read(&ctx, 0, g_dst, 10, N_MONO) == 0, "read past end -> 0 (loop)");

    /* partial tail then EOF */
    got = audio_file_stream_read(&ctx, 0, g_dst, 10, N_MONO - 5);
    CHECK(got == 5, "tail returns the final 5 frames");

    audio_file_stream_close(&ctx);
}

static void test_stereo_preserved(void) {
    const uint32_t nf = 2000;
    for (uint32_t i = 0; i < nf; i++) {
        g_src[i*2]   = (int16_t)i;          /* L */
        g_src[i*2+1] = (int16_t)(1000 + i); /* R */
    }
    uint32_t len = build_wav16(g_wav, 2, 44100, g_src, nf);

    MemFile mf = { g_wav, len, 0 };
    AudioFileReader rd = { m_open, m_read, m_seek, m_close, &mf };
    AudioFileStreamCtx ctx;
    CHECK(audio_file_stream_open(&ctx, &rd, "x.wav"), "open stereo wav");
    CHECK(ctx.channels == 2, "parsed 2 channels");
    CHECK(ctx.data_bytes == nf * 4u, "stereo data size");

    /* stereo must be PRESERVED, not downmixed */
    size_t got = audio_file_stream_read(&ctx, 0, g_dst, 10, 0);
    int ok = 1;
    for (int i = 0; i < 10; i++)
        if (g_dst[i*2] != i || g_dst[i*2+1] != 1000+i) ok = 0;
    CHECK(got == 10 && ok, "stereo L/R preserved (no downmix)");

    /* full chunked read */
    got = audio_file_stream_read(&ctx, 0, g_dst, nf, 0);
    CHECK(got == nf, "stereo chunked full read");
    CHECK(g_dst[(nf-1)*2] == (int16_t)(nf-1) && g_dst[(nf-1)*2+1] == (int16_t)(1000+nf-1),
          "stereo content correct at end");

    audio_file_stream_close(&ctx);
}

int main(void) {
    test_mono_promoted_to_stereo();
    test_stereo_preserved();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
