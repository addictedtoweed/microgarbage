/* ============================================================
 *  test_audio_sink_wav.c — verify the WAV-dump backend
 *
 *  Writes known PCM through the sink to a temp .wav, reads the file
 *  back by hand, and asserts the header fields and the sample data
 *  are exactly right. This proves the output path produces correct
 *  bytes with no sound hardware involved.
 *
 *  Build:
 *    cc -std=c11 -Iinclude -o t \
 *       src/audio/test_audio_sink_wav.c src/audio/audio_sink_wav.c
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

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

int main(void) {
    const char *path = "/tmp/test_sink_out.wav";
    const uint32_t rate = 44100;
    const uint32_t frames = 1000;

    /* Build a known stereo ramp: L = i, R = -i. */
    int16_t *pcm = malloc(frames * 2 * sizeof(int16_t));
    for (uint32_t i = 0; i < frames; i++) {
        pcm[2*i+0] = (int16_t)(i & 0x7FFF);
        pcm[2*i+1] = (int16_t)(-(int32_t)(i & 0x7FFF));
    }

    /* Write it through the sink in two chunks (exercise streaming). */
    AudioSink sink;
    CHECK(audio_sink_open(&sink, "wav", path, rate), "open wav sink");
    int w1 = audio_sink_write(&sink, pcm, 400);
    int w2 = audio_sink_write(&sink, pcm + 400*2, frames - 400);
    CHECK(w1 == 400 && w2 == (int)(frames - 400), "wrote all frames in two chunks");
    audio_sink_close(&sink);

    /* Read the file back raw. */
    FILE *fp = fopen(path, "rb");
    CHECK(fp != NULL, "reopen written wav");
    if (!fp) { printf("%d passed, %d failed\n", g_pass, g_fail); return 1; }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)sz);
    fread(buf, 1, (size_t)sz, fp);
    fclose(fp);

    uint32_t expect_data = frames * 2 * sizeof(int16_t);
    CHECK((uint32_t)sz == 44 + expect_data, "file size = 44 header + data");

    /* header checks */
    CHECK(memcmp(buf + 0, "RIFF", 4) == 0, "RIFF magic");
    CHECK(rd_u32le(buf + 4) == 36 + expect_data, "RIFF chunk size patched");
    CHECK(memcmp(buf + 8, "WAVE", 4) == 0, "WAVE magic");
    CHECK(memcmp(buf + 12, "fmt ", 4) == 0, "fmt chunk");
    CHECK(rd_u32le(buf + 16) == 16, "fmt size 16 (PCM)");
    CHECK(rd_u16le(buf + 20) == 1, "format = PCM");
    CHECK(rd_u16le(buf + 22) == 2, "channels = 2 (stereo)");
    CHECK(rd_u32le(buf + 24) == rate, "sample rate");
    CHECK(rd_u32le(buf + 28) == rate * 2 * 2, "byte rate = rate*channels*2");
    CHECK(rd_u16le(buf + 32) == 4, "block align = 4");
    CHECK(rd_u16le(buf + 34) == 16, "bits = 16");
    CHECK(memcmp(buf + 36, "data", 4) == 0, "data chunk");
    CHECK(rd_u32le(buf + 40) == expect_data, "data size patched");

    /* sample data integrity */
    const uint8_t *d = buf + 44;
    int ok = 1;
    for (uint32_t i = 0; i < frames; i++) {
        int16_t l = (int16_t)rd_u16le(d + (2*i+0)*2);
        int16_t r = (int16_t)rd_u16le(d + (2*i+1)*2);
        if (l != (int16_t)(i & 0x7FFF)) ok = 0;
        if (r != (int16_t)(-(int32_t)(i & 0x7FFF))) ok = 0;
    }
    CHECK(ok, "all stereo samples round-trip exactly");

    free(buf); free(pcm);
    remove(path);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
