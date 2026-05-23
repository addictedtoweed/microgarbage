/* ============================================================
 *  audio_sink_wav.c — WAV-file output backend + sink dispatcher
 *
 *  Hand-rolled RIFF/WAVE writer, no libsndfile, no deps. Writes a
 *  44-byte canonical PCM WAV header, streams interleaved s16 stereo,
 *  and patches the two size fields on close (we don't know the total
 *  length until the stream ends, so we write placeholders and seek
 *  back). This is the fully-verifiable backend: render -> WAV ->
 *  read back -> compare.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_sink.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- little-endian helpers (WAV is always little-endian) ---- */
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

typedef struct {
    FILE    *fp;
    uint32_t sample_rate;
    uint32_t frames_written;   /* for patching sizes on close */
    char     path[512];
} WavCtx;

/* Write the 44-byte canonical PCM WAV header. data_bytes may be a
 * placeholder (0); it's patched on close. */
static void write_header(FILE *fp, uint32_t sample_rate, uint32_t data_bytes) {
    uint8_t h[44];
    uint16_t channels   = AUDIO_SINK_CHANNELS;
    uint16_t bits       = AUDIO_SINK_BITS;
    uint16_t block_align = (uint16_t)(channels * (bits / 8));
    uint32_t byte_rate   = sample_rate * block_align;

    memcpy(h + 0, "RIFF", 4);
    put_u32le(h + 4, 36 + data_bytes);      /* RIFF chunk size */
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    put_u32le(h + 16, 16);                   /* fmt chunk size (PCM) */
    put_u16le(h + 20, 1);                    /* audio format = PCM */
    put_u16le(h + 22, channels);
    put_u32le(h + 24, sample_rate);
    put_u32le(h + 28, byte_rate);
    put_u16le(h + 32, block_align);
    put_u16le(h + 34, bits);
    memcpy(h + 36, "data", 4);
    put_u32le(h + 40, data_bytes);           /* data chunk size */
    fwrite(h, 1, sizeof(h), fp);
}

static void *wav_open(uint32_t sample_rate) {
    /* path is delivered separately via audio_sink_open through a
     * thread-local-ish handoff; see g_wav_path below. */
    extern const char *audio_sink_wav_path(void);
    const char *path = audio_sink_wav_path();
    if (!path) return NULL;

    WavCtx *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->fp = fopen(path, "wb");
    if (!c->fp) { free(c); return NULL; }
    c->sample_rate = sample_rate;
    c->frames_written = 0;
    snprintf(c->path, sizeof(c->path), "%s", path);

    write_header(c->fp, sample_rate, 0);     /* placeholder sizes */
    return c;
}

static int wav_write(void *ctx, const int16_t *interleaved, uint32_t frames) {
    WavCtx *c = (WavCtx *)ctx;
    if (!c || !c->fp) return -1;
    size_t n = fwrite(interleaved, sizeof(int16_t),
                      (size_t)frames * AUDIO_SINK_CHANNELS, c->fp);
    uint32_t got = (uint32_t)(n / AUDIO_SINK_CHANNELS);
    c->frames_written += got;
    return (int)got;
}

static void wav_close(void *ctx) {
    WavCtx *c = (WavCtx *)ctx;
    if (!c) return;
    if (c->fp) {
        /* patch the size fields now that we know the length */
        uint32_t data_bytes = c->frames_written *
                              AUDIO_SINK_CHANNELS * (AUDIO_SINK_BITS / 8);
        uint8_t buf[4];
        /* RIFF size at offset 4 */
        if (fseek(c->fp, 4, SEEK_SET) == 0) {
            put_u32le(buf, 36 + data_bytes); fwrite(buf, 1, 4, c->fp);
        }
        /* data size at offset 40 */
        if (fseek(c->fp, 40, SEEK_SET) == 0) {
            put_u32le(buf, data_bytes); fwrite(buf, 1, 4, c->fp);
        }
        fclose(c->fp);
    }
    free(c);
}

const AudioSinkBackend audio_sink_wav = {
    "wav", wav_open, wav_write, wav_close
};

/* ---- path handoff ----
 * The vtable's open() takes only a sample rate, but the WAV backend
 * needs a file path. We stash it here between audio_sink_open() and
 * the backend's open(). Single-context desktop tool — no concurrency
 * on this. */
static const char *g_wav_path;
const char *audio_sink_wav_path(void) { return g_wav_path; }

/* ---- dispatcher ---- */
bool audio_sink_open(AudioSink *sink, const char *backend_name,
                     const char *path, uint32_t sample_rate) {
    if (!sink || !backend_name) return false;
    memset(sink, 0, sizeof(*sink));

    const AudioSinkBackend *b = NULL;
    if (strcmp(backend_name, "wav") == 0) {
        b = &audio_sink_wav;
        g_wav_path = path;
    }
#if defined(_WIN32) || defined(__CYGWIN__)
    else if (strcmp(backend_name, "wave") == 0 ||
             strcmp(backend_name, "waveout") == 0) {
        b = &audio_sink_waveout;
    }
#endif
    if (!b) return false;

    void *ctx = b->open(sample_rate);
    if (!ctx) return false;

    sink->backend     = b;
    sink->ctx         = ctx;
    sink->sample_rate = sample_rate;
    return true;
}

int audio_sink_write(AudioSink *sink, const int16_t *interleaved,
                     uint32_t frames) {
    if (!sink || !sink->backend) return -1;
    return sink->backend->write(sink->ctx, interleaved, frames);
}

void audio_sink_close(AudioSink *sink) {
    if (!sink || !sink->backend) return;
    sink->backend->close(sink->ctx);
    sink->ctx = NULL;
}
