/* ============================================================
 *  test_audio_wav_read.c — WAV parsing + mono16 conversion
 *
 *  Builds WAV buffers in memory (stereo16, mono16, mono8, and one
 *  with an extra LIST chunk to prove chunk-walking), parses them, and
 *  asserts the fmt fields + the mono PCM16 conversion (downmix +
 *  bit-promotion) are correct. Also checks the round-trip with the
 *  writer's output isn't needed — we hand-build headers so all
 *  formats are covered without the writer being stereo-only.
 *
 *  Build:
 *    cc -std=c11 -Iinclude -o t \
 *       src/audio/tests/test_audio_wav_read.c src/audio/audio_wav_read.c
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

static void put_u32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static void put_u16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

/* Build a minimal WAV (fmt + data) into buf, return total length.
 * `extra_list` inserts a junk LIST chunk between fmt and data. */
static size_t build_wav(uint8_t *buf, uint16_t channels, uint16_t bits,
                        uint32_t rate, const uint8_t *data, uint32_t data_bytes,
                        int extra_list) {
    uint16_t align = (uint16_t)(channels * (bits/8));
    uint8_t *p = buf;
    memcpy(p, "RIFF", 4); p += 4;
    uint8_t *riff_size = p; put_u32(p, 0); p += 4;   /* patch later */
    memcpy(p, "WAVE", 4); p += 4;
    memcpy(p, "fmt ", 4); p += 4;
    put_u32(p, 16); p += 4;
    put_u16(p, 1); p += 2;                 /* PCM */
    put_u16(p, channels); p += 2;
    put_u32(p, rate); p += 4;
    put_u32(p, rate * align); p += 4;      /* byte rate */
    put_u16(p, align); p += 2;
    put_u16(p, bits); p += 2;
    if (extra_list) {
        memcpy(p, "LIST", 4); p += 4;
        put_u32(p, 4); p += 4;
        memcpy(p, "INFO", 4); p += 4;      /* 4-byte junk body */
    }
    memcpy(p, "data", 4); p += 4;
    put_u32(p, data_bytes); p += 4;
    memcpy(p, data, data_bytes); p += data_bytes;
    size_t total = (size_t)(p - buf);
    put_u32(riff_size, (uint32_t)(total - 8));
    return total;
}

/* ---- stereo 16-bit parse + downmix ---- */
static void test_stereo16(void) {
    int16_t pcm[8] = { 100,200, 300,400, -100,-300, 1000,-1000 }; /* 4 frames LR */
    uint8_t wav[256];
    size_t len = build_wav(wav, 2, 16, 44100, (uint8_t*)pcm, sizeof(pcm), 0);

    WavInfo info;
    CHECK(wav_parse(wav, len, &info) == WAV_OK, "parse stereo16 OK");
    CHECK(info.channels == 2, "channels 2");
    CHECK(info.bits == 16, "bits 16");
    CHECK(info.sample_rate == 44100, "rate 44100");
    CHECK(info.data_bytes == sizeof(pcm), "data bytes");

    int16_t mono[4];
    uint32_t n = wav_to_mono_pcm16(&info, mono, 4);
    CHECK(n == 4, "4 frames downmixed");
    CHECK(mono[0] == 150, "frame0 (100+200)/2 = 150");
    CHECK(mono[1] == 350, "frame1 (300+400)/2 = 350");
    CHECK(mono[2] == -200, "frame2 (-100-300)/2 = -200");
    CHECK(mono[3] == 0, "frame3 (1000-1000)/2 = 0");
}

/* ---- mono 16-bit parse (no downmix) ---- */
static void test_mono16(void) {
    int16_t pcm[4] = { 11, -22, 333, -444 };
    uint8_t wav[256];
    size_t len = build_wav(wav, 1, 16, 22050, (uint8_t*)pcm, sizeof(pcm), 0);

    WavInfo info;
    CHECK(wav_parse(wav, len, &info) == WAV_OK, "parse mono16 OK");
    CHECK(info.channels == 1 && info.bits == 16, "mono16 fmt");
    CHECK(info.sample_rate == 22050, "rate 22050");

    int16_t mono[4];
    uint32_t n = wav_to_mono_pcm16(&info, mono, 4);
    CHECK(n == 4, "4 mono frames");
    CHECK(mono[0]==11 && mono[1]==-22 && mono[2]==333 && mono[3]==-444,
          "mono16 passes through unchanged");
}

/* ---- mono 8-bit parse (u8 -> s16 promotion) ---- */
static void test_mono8(void) {
    uint8_t pcm[4] = { 128, 255, 0, 192 };   /* center, max, min, 0.5 */
    uint8_t wav[256];
    size_t len = build_wav(wav, 1, 8, 8000, pcm, sizeof(pcm), 0);

    WavInfo info;
    CHECK(wav_parse(wav, len, &info) == WAV_OK, "parse mono8 OK");
    CHECK(info.bits == 8, "bits 8");

    int16_t mono[4];
    uint32_t n = wav_to_mono_pcm16(&info, mono, 4);
    CHECK(n == 4, "4 frames from mono8");
    CHECK(mono[0] == 0, "u8 128 -> 0 (center)");
    CHECK(mono[1] == ((255-128)<<8), "u8 255 -> max-ish");
    CHECK(mono[2] == (-128*256), "u8 0 -> min-ish");
}

/* ---- chunk-walking past an extra LIST chunk ---- */
static void test_extra_chunk(void) {
    int16_t pcm[4] = { 5,5, 9,9 };           /* 2 stereo frames */
    uint8_t wav[256];
    size_t len = build_wav(wav, 2, 16, 44100, (uint8_t*)pcm, sizeof(pcm), 1);

    WavInfo info;
    CHECK(wav_parse(wav, len, &info) == WAV_OK, "parse with extra LIST chunk OK");
    CHECK(info.channels == 2 && info.data_bytes == sizeof(pcm),
          "found data after skipping LIST");
    int16_t mono[2];
    uint32_t n = wav_to_mono_pcm16(&info, mono, 2);
    CHECK(n == 2 && mono[0] == 5 && mono[1] == 9, "downmix after chunk-walk");
}

/* ---- rejections ---- */
static void test_rejections(void) {
    WavInfo info;
    uint8_t junk[64];
    memset(junk, 0, sizeof(junk));
    CHECK(wav_parse(junk, sizeof(junk), &info) == WAV_ERR_BAD_MAGIC,
          "non-RIFF rejected");
    CHECK(wav_parse(junk, 4, &info) == WAV_ERR_TOO_SMALL, "tiny buffer rejected");

    /* non-PCM format (e.g. 3 = IEEE float) */
    int16_t pcm[2] = {1,1};
    uint8_t wav[128];
    size_t len = build_wav(wav, 1, 16, 44100, (uint8_t*)pcm, sizeof(pcm), 0);
    wav[20] = 3;  /* patch format field to 3 */
    CHECK(wav_parse(wav, len, &info) == WAV_ERR_NOT_PCM, "non-PCM rejected");

    /* clamp: ask for more frames than present */
    len = build_wav(wav, 1, 16, 44100, (uint8_t*)pcm, sizeof(pcm), 0);
    wav_parse(wav, len, &info);
    int16_t out[100];
    uint32_t n = wav_to_mono_pcm16(&info, out, 100);
    CHECK(n == 2, "conversion clamps to available frames");
}

int main(void) {
    test_stereo16();
    test_mono16();
    test_mono8();
    test_extra_chunk();
    test_rejections();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
