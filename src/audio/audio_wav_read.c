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
