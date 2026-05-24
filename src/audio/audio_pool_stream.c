/* ============================================================
 *  audio_pool_stream.c — pool <-> music player bridge
 *
 *  See audio_pool_stream.h. The only real work is unit conversion:
 *  the player speaks frames, the pool speaks bytes.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_pool_stream.h"

static size_t fmt_bpf(MixerSourceFormat fmt) {
    switch (fmt) {
        case MIXER_SRC_PCM16_MONO:    return 2;
        case MIXER_SRC_PCM8S_MONO:    return 1;
        case MIXER_SRC_PCM8U_MONO:    return 1;
        case MIXER_SRC_PCM16_STEREO:  return 4;
        case MIXER_SRC_PCM8S_STEREO:  return 2;
        case MIXER_SRC_PCM8U_STEREO:  return 2;
    }
    return 0;
}

bool audio_pool_stream_init(AudioPoolStreamCtx *ctx, AudioPool *pool,
                            MixerSourceFormat format) {
    if (!ctx || !pool) return false;
    size_t bpf = fmt_bpf(format);
    if (bpf == 0) return false;

    ctx->pool            = pool;
    ctx->bytes_per_frame = bpf;
    ctx->stream_id[0]    = MUSIC_STREAM_NONE;
    ctx->stream_id[1]    = MUSIC_STREAM_NONE;
    ctx->handle[0]       = AUDIO_POOL_HANDLE_NONE;
    ctx->handle[1]       = AUDIO_POOL_HANDLE_NONE;
    ctx->promote_stereo  = false;
    return true;
}

void audio_pool_stream_set_promote_stereo(AudioPoolStreamCtx *ctx, bool on) {
    if (ctx) ctx->promote_stereo = on;
}

bool audio_pool_stream_bind(AudioPoolStreamCtx *ctx,
                            int stream_id, AudioObjHandle handle) {
    if (!ctx) return false;

    /* If this id is already bound, update it (or clear it). */
    for (int i = 0; i < 2; i++) {
        if (ctx->stream_id[i] == stream_id &&
            ctx->stream_id[i] != MUSIC_STREAM_NONE) {
            if (handle == AUDIO_POOL_HANDLE_NONE) {
                ctx->stream_id[i] = MUSIC_STREAM_NONE;
                ctx->handle[i]    = AUDIO_POOL_HANDLE_NONE;
            } else {
                ctx->handle[i] = handle;
            }
            return true;
        }
    }

    if (handle == AUDIO_POOL_HANDLE_NONE) return true;  /* nothing to clear */

    /* Take a free slot. */
    for (int i = 0; i < 2; i++) {
        if (ctx->stream_id[i] == MUSIC_STREAM_NONE) {
            ctx->stream_id[i] = stream_id;
            ctx->handle[i]    = handle;
            return true;
        }
    }
    return false;   /* both slots taken by other ids */
}

static AudioObjHandle lookup(const AudioPoolStreamCtx *ctx, int stream_id) {
    for (int i = 0; i < 2; i++) {
        if (ctx->stream_id[i] == stream_id &&
            ctx->stream_id[i] != MUSIC_STREAM_NONE) {
            return ctx->handle[i];
        }
    }
    return AUDIO_POOL_HANDLE_NONE;
}

size_t audio_pool_stream_read(void *user_data, int stream_id,
                              void *destination,
                              size_t requested_samples,
                              size_t offset_in_stream) {
    AudioPoolStreamCtx *ctx = (AudioPoolStreamCtx *)user_data;
    if (!ctx || !destination || ctx->bytes_per_frame == 0) return 0;

    AudioObjHandle h = lookup(ctx, stream_id);
    if (h == AUDIO_POOL_HANDLE_NONE) return 0;

    /* frames -> bytes */
    uint32_t byte_off = (uint32_t)(offset_in_stream * ctx->bytes_per_frame);
    uint32_t byte_n   = (uint32_t)(requested_samples * ctx->bytes_per_frame);

    uint32_t copied = 0;
    AudioPoolResult r = audio_pool_read(ctx->pool, h, byte_off,
                                        destination, byte_n, &copied);
    if (r != AUDIO_POOL_OK) return 0;

    /* bytes -> frames. Pool reads clamp to object size; if the object
     * size isn't a whole number of frames (it should be), any partial
     * trailing frame is dropped from the count. */
    uint32_t frames = (uint32_t)(copied / ctx->bytes_per_frame);

    /* mono16 source -> stereo16 output: duplicate each sample to L==R
     * (no downmix). We read the mono frames into the front of the
     * destination above; expand in place, back to front, so the source
     * sample at [i] is consumed before stereo writes at [2i],[2i+1]
     * (2i >= i, and lower indices aren't yet overwritten). */
    if (ctx->promote_stereo) {
        int16_t *d = (int16_t *)destination;
        for (int32_t i = (int32_t)frames - 1; i >= 0; i--) {
            int16_t m = d[i];
            d[i * 2]     = m;
            d[i * 2 + 1] = m;
        }
    }
    return frames;
}
