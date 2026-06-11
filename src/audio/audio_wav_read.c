/* ============================================================
 *  audio_wav_read.c — WAV parser for drop-in assets
 *
 *  Parses a canonical PCM WAV from a memory buffer and converts it to
 *  the mono PCM16 the current SFX path expects. Zero-dep, the mirror
 *  of audio_sink_wav.c's writer. Walks the RIFF chunk list so files
 *  with extra chunks (LIST/INFO/fact, common in real-world .wav)
 *  still parse — we just need fmt + data.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_sink.h"

#include <string.h>

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

WavResult wav_parse(const uint8_t *buf, size_t len, WavInfo *out) {
    if (!buf || !out) return WAV_ERR_TOO_SMALL;
    if (len < 12) return WAV_ERR_TOO_SMALL;
    memset(out, 0, sizeof(*out));

    if (memcmp(buf + 0, "RIFF", 4) != 0) return WAV_ERR_BAD_MAGIC;
    if (memcmp(buf + 8, "WAVE", 4) != 0) return WAV_ERR_BAD_MAGIC;

    bool have_fmt = false, have_data = false;
    size_t pos = 12;                       /* first chunk after "WAVE" */
    while (pos + 8 <= len) {
        const uint8_t *ck = buf + pos;
        uint32_t ck_size = rd_u32le(ck + 4);
        const uint8_t *body = ck + 8;
        /* guard against a size that runs past the buffer */
        size_t avail = len - (pos + 8);
        uint32_t use = (ck_size <= avail) ? ck_size : (uint32_t)avail;

        if (memcmp(ck, "fmt ", 4) == 0 && use >= 16) {
            out->format      = rd_u16le(body + 0);
            out->channels    = rd_u16le(body + 2);
            out->sample_rate = rd_u32le(body + 4);
            out->bits        = rd_u16le(body + 14);
            have_fmt = true;
        } else if (memcmp(ck, "data", 4) == 0) {
            out->data       = body;
            out->data_bytes = use;
            have_data = true;
        }
        /* chunks are word-aligned: advance by size + pad to even */
        pos += 8 + ck_size + (ck_size & 1u);
    }

    if (!have_fmt)  return WAV_ERR_NO_DATA;     /* no format -> unusable */
    if (out->format != 1) return WAV_ERR_NOT_PCM;
    if (!have_data) return WAV_ERR_NO_DATA;
    if (out->channels < 1 || out->channels > 2) return WAV_ERR_UNSUPPORTED;
    if (out->bits != 8 && out->bits != 16) return WAV_ERR_UNSUPPORTED;
    return WAV_OK;
}

uint32_t wav_to_mono_pcm16(const WavInfo *info, int16_t *dst,
                           uint32_t max_frames) {
    if (!info || !dst || !info->data) return 0;
    uint32_t ch    = info->channels;
    uint32_t bps   = info->bits / 8u;          /* bytes per sample     */
    uint32_t fb    = ch * bps;                 /* bytes per frame      */
    if (fb == 0) return 0;
    uint32_t avail = info->data_bytes / fb;     /* source frames        */
    uint32_t n = (avail < max_frames) ? avail : max_frames;

    const uint8_t *d = info->data;
    for (uint32_t i = 0; i < n; i++) {
        int32_t acc = 0;
        for (uint32_t c = 0; c < ch; c++) {
            const uint8_t *s = d + (size_t)i * fb + (size_t)c * bps;
            int32_t v;
            if (info->bits == 16) {
                v = (int16_t)rd_u16le(s);              /* signed 16     */
            } else {
                v = ((int32_t)s[0] - 128) * 256;        /* u8 -> s16     */
            }
            acc += v;
        }
        dst[i] = (int16_t)(acc / (int32_t)ch);          /* downmix mono  */
    }
    return n;
}

/* Like wav_to_mono_pcm16 but resamples the result to dst_rate Hz with
 * linear interpolation. Useful when the loader knows the target rate
 * (e.g. the audio service's mixer rate) and wants every SFX in the
 * pool to be at that rate so the per-channel mixer step stays 1.0 —
 * sidesteps the need for per-sample source-rate tracking inside the
 * mixer. Returns frames written at dst_rate. */
uint32_t wav_to_mono_pcm16_resample(const WavInfo *info, int16_t *dst,
                                    uint32_t max_dst_frames,
                                    uint32_t dst_rate) {
    if (!info || !dst || !info->data || dst_rate == 0) return 0;
    uint32_t src_rate = info->sample_rate ? info->sample_rate : dst_rate;
    if (src_rate == dst_rate) {
        return wav_to_mono_pcm16(info, dst, max_dst_frames);
    }
    uint32_t ch  = info->channels;
    uint32_t bps = info->bits / 8u;
    uint32_t fb  = ch * bps;
    if (fb == 0) return 0;
    uint32_t src_frames = info->data_bytes / fb;
    if (src_frames < 2) return 0;

    /* Linear interpolation in q32.32. step = src_rate / dst_rate per
     * output frame; phase wraps the integer part into an input-frame
     * advance, the fractional part lerps between src[i] and src[i+1]. */
    uint64_t step = ((uint64_t)src_rate << 32) / (uint64_t)dst_rate;

    /* Cap output by both the dst-buffer and by what the source can
     * actually produce (one src frame yields ~dst_rate/src_rate dst
     * frames). The exact bound is (src_frames-1) * dst_rate / src_rate,
     * computed in 64-bit to avoid overflow on long SFX. */
    uint64_t producible = ((uint64_t)(src_frames - 1u) * (uint64_t)dst_rate)
                            / (uint64_t)src_rate;
    if (producible > (uint64_t)max_dst_frames) producible = max_dst_frames;
    uint32_t n_out = (uint32_t)producible;

    const uint8_t *d = info->data;
    const uint32_t bits = info->bits;

    /* Read + downmix one source frame to mono int16. Static-inline so
     * GCC's -Wpedantic doesn't complain about GNU statement-expressions
     * (a previous macro attempt tripped that warning). */
    #define READ_SRC_FRAME(idx_var, out_var) do {                           \
        int32_t _acc = 0;                                                   \
        for (uint32_t _c = 0; _c < ch; _c++) {                              \
            const uint8_t *_s = d + (size_t)(idx_var) * fb + (size_t)_c * bps; \
            int32_t _v = (bits == 16)                                       \
                ? (int32_t)(int16_t)rd_u16le(_s)                            \
                : ((int32_t)_s[0] - 128) * 256;                             \
            _acc += _v;                                                     \
        }                                                                   \
        (out_var) = (int32_t)(_acc / (int32_t)ch);                          \
    } while (0)

    uint64_t phase = 0;
    for (uint32_t i = 0; i < n_out; i++) {
        uint32_t idx  = (uint32_t)(phase >> 32);
        uint32_t frac = (uint32_t)phase;
        int32_t  a = 0, b = 0;
        if (idx + 1 >= src_frames) {
            /* Edge — clamp to last source sample. */
            READ_SRC_FRAME(src_frames - 1u, a);
            dst[i] = (int16_t)a;
        } else {
            READ_SRC_FRAME(idx, a);
            READ_SRC_FRAME(idx + 1u, b);
            int64_t lerp = (int64_t)a
                         + (((int64_t)(b - a) * (int64_t)frac) >> 32);
            dst[i] = (int16_t)lerp;
        }
        phase += step;
    }
    #undef READ_SRC_FRAME
    return n_out;
}

uint32_t wav_to_stereo_pcm16(const WavInfo *info, int16_t *dst,
                             uint32_t max_frames) {
    if (!info || !dst || !info->data) return 0;
    uint32_t ch    = info->channels;
    uint32_t bps   = info->bits / 8u;          /* bytes per sample     */
    uint32_t fb    = ch * bps;                 /* bytes per frame      */
    if (fb == 0) return 0;
    uint32_t avail = info->data_bytes / fb;     /* source frames        */
    uint32_t n = (avail < max_frames) ? avail : max_frames;

    const uint8_t *d = info->data;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *s = d + (size_t)i * fb;
        int16_t l, r;
        if (info->bits == 16) {
            l = (int16_t)rd_u16le(s);
            r = (ch == 2) ? (int16_t)rd_u16le(s + 2) : l;   /* mono -> L=R */
        } else {                                            /* 8-bit -> 16 */
            l = (int16_t)(((int32_t)s[0] - 128) * 256);
            r = (ch == 2) ? (int16_t)(((int32_t)s[1] - 128) * 256) : l;
        }
        dst[i * 2]     = l;
        dst[i * 2 + 1] = r;
    }
    return n;
}
