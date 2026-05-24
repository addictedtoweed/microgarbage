/* ============================================================
 *  audio_file_stream.c — file-backed music_player stream source
 *
 *  Reads a WAV incrementally through an AudioFileReader and serves the
 *  music_player's stream callback as interleaved stereo16 on the fly
 *  (stereo preserved, mono promoted to L==R — no downmix).
 *  See audio_file_stream.h. Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_file_stream.h"
#include "audio/audio_sink.h"      /* WavInfo, wav_parse, wav_to_mono_pcm16 */

#include <string.h>

/* Bytes read up front to locate the fmt + data chunk headers. The audio
 * data body usually starts within the first few dozen bytes; 4 KB is
 * generous headroom for files with extra metadata chunks before data. */
#define HEADER_PROBE  4096u

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool audio_file_stream_open(AudioFileStreamCtx *ctx,
                            const AudioFileReader *reader, const char *path) {
    if (!ctx || !reader || !path) return false;
    if (!reader->open || !reader->read || !reader->seek || !reader->close)
        return false;

    memset(ctx, 0, sizeof *ctx);
    ctx->reader = *reader;
    ctx->fh = reader->open(reader->ctx, path);
    if (!ctx->fh) return false;

    uint8_t hdr[HEADER_PROBE];
    if (!ctx->reader.seek(ctx->reader.ctx, ctx->fh, 0)) goto fail;
    uint32_t n = ctx->reader.read(ctx->reader.ctx, ctx->fh, hdr, sizeof hdr);
    if (n < 12) goto fail;

    WavInfo info;
    if (wav_parse(hdr, n, &info) != WAV_OK) goto fail;

    /* info.data aliases into hdr at the data body; its file offset is
     * that distance from the start. */
    uint32_t data_off = (uint32_t)(info.data - hdr);
    if (data_off < 8 || data_off > n) goto fail;   /* need the size field */

    ctx->data_offset     = data_off;
    /* wav_parse clamps data_bytes to the bytes we read; take the TRUE
     * size from the data chunk's size field (the 4 bytes before body). */
    ctx->data_bytes      = rd_u32le(hdr + data_off - 4);
    ctx->channels        = info.channels;
    ctx->bits            = info.bits;
    ctx->sample_rate     = info.sample_rate;
    ctx->src_frame_bytes = (uint32_t)info.channels * (info.bits / 8u);
    if (ctx->src_frame_bytes == 0) goto fail;
    return true;

fail:
    ctx->reader.close(ctx->reader.ctx, ctx->fh);
    ctx->fh = NULL;
    return false;
}

size_t audio_file_stream_read(void *user_data, int stream_id,
                              void *destination,
                              size_t requested_samples,
                              size_t offset_in_stream) {
    (void)stream_id;
    AudioFileStreamCtx *ctx = (AudioFileStreamCtx *)user_data;
    if (!ctx || !ctx->fh || !destination || ctx->src_frame_bytes == 0) return 0;

    const uint32_t fb = ctx->src_frame_bytes;
    const uint32_t total_frames = ctx->data_bytes / fb;
    if (offset_in_stream >= total_frames) return 0;     /* EOF -> player loops */

    int16_t *dst = (int16_t *)destination;

    /* Seek once to the requested source position, then read forward. */
    uint32_t cur_frame = (uint32_t)offset_in_stream;
    if (!ctx->reader.seek(ctx->reader.ctx, ctx->fh,
                          ctx->data_offset + cur_frame * fb))
        return 0;

    const uint32_t scratch_frames = AUDIO_FILE_STREAM_SCRATCH / fb;
    uint32_t produced = 0;
    while (produced < requested_samples && cur_frame < total_frames) {
        uint32_t want = (uint32_t)requested_samples - produced;
        if (want > scratch_frames)            want = scratch_frames;
        if (want > total_frames - cur_frame)  want = total_frames - cur_frame;

        uint32_t got = ctx->reader.read(ctx->reader.ctx, ctx->fh,
                                        ctx->scratch, want * fb);
        uint32_t got_frames = got / fb;
        if (got_frames == 0) break;            /* short read / truncated file */

        WavInfo chunk = {
            .format      = 1,
            .channels    = ctx->channels,
            .sample_rate = ctx->sample_rate,
            .bits        = ctx->bits,
            .data        = ctx->scratch,
            .data_bytes  = got_frames * fb,
        };
        /* Emit interleaved stereo16 (2 int16 per frame): stereo preserved,
         * mono promoted to L==R. */
        uint32_t conv = wav_to_stereo_pcm16(&chunk, dst + produced * 2, got_frames);
        produced  += conv;
        cur_frame += got_frames;
        if (got_frames < want) break;          /* hit end of file */
    }
    return produced;
}

void audio_file_stream_close(AudioFileStreamCtx *ctx) {
    if (ctx && ctx->fh) {
        ctx->reader.close(ctx->reader.ctx, ctx->fh);
        ctx->fh = NULL;
    }
}
