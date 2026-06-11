/* fmv_demux.c — split a muxed FMV2 file into FMV1 video + WAV audio.
 *
 * Reads an FMV2 (32-byte header + interleaved audio/video frames) and
 * writes two outputs the mgapi guest player consumes directly:
 *   - <base>.fmv : FMV1 (16-byte header + video-only frames)
 *   - <base>.wav : canonical PCM WAV (44-byte header + s16le stereo)
 *
 * Layout (per the encoder header in tools/fmv_encode.c):
 *   FMV2 header (32 B): "FMV2", u16 w, u16 h, u16 fps, u16 audio_channels,
 *                       u32 nframes, u32 audio_rate, u16 audio_bits, u16 _,
 *                       u32 audio_bytes_per_frame, u32 _
 *   FMV1 header (16 B): "FMV1", u16 w, u16 h, u16 fps, u16 _, u32 nframes
 *   per-frame in FMV2: [audio (audio_bytes_per_frame) | video (26776 B)]
 *
 *   gcc -Wall -O2 -o tools/fmv_demux tools/fmv_demux.c
 *   ./fmv_demux movie.fmv          # → movie.fmv (overwrites!) + movie.wav
 *   ./fmv_demux movie.fmv split    # → split.fmv + split.wav
 *
 * Public domain (CC0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define VIDEO_PER_FRAME 26776   /* 8*16*2 CGRAM + 780*2 tilemap + 780*32 CHR */

static uint16_t rd_u16le(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}
static void wr_u16le(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr_u32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: %s INPUT.fmv [OUTBASE]\n"
                        "  Demuxes FMV2 -> OUTBASE.fmv (FMV1) + OUTBASE.wav.\n"
                        "  Default OUTBASE = input basename (overwrites!).\n",
                argv[0]);
        return 1;
    }
    const char *in_path = argv[1];

    /* Derive output base from input or arg. */
    char base[512];
    if (argc == 3) {
        snprintf(base, sizeof(base), "%s", argv[2]);
    } else {
        snprintf(base, sizeof(base), "%s", in_path);
        char *dot = strrchr(base, '.');
        if (dot && dot > strrchr(base, '/') && dot > strrchr(base, '\\')) *dot = 0;
    }

    FILE *in = fopen(in_path, "rb");
    if (!in) { perror(in_path); return 1; }

    uint8_t hdr[32];
    if (fread(hdr, 1, 32, in) != 32 || memcmp(hdr, "FMV2", 4) != 0) {
        fprintf(stderr, "%s: not an FMV2 file\n", in_path);
        fclose(in); return 1;
    }
    uint16_t w   = rd_u16le(hdr + 4);
    uint16_t h   = rd_u16le(hdr + 6);
    uint16_t fps = rd_u16le(hdr + 8);
    uint16_t ach = rd_u16le(hdr + 10);
    uint32_t nfr = rd_u32le(hdr + 12);
    uint32_t arate = rd_u32le(hdr + 16);
    uint16_t abits = rd_u16le(hdr + 20);
    uint32_t abpf  = rd_u32le(hdr + 24);   /* audio bytes per video frame */

    fprintf(stderr, "input:  %s\n", in_path);
    fprintf(stderr, "        FMV2, %ux%u @ %u fps, %u frames (%.2f s)\n",
            w, h, fps, nfr, (double)nfr / (double)fps);
    fprintf(stderr, "        audio: %u Hz, %u ch, %u bit, %u B/frame\n",
            arate, ach, abits, abpf);

    /* Open outputs. */
    char fmv_path[600], wav_path[600];
    snprintf(fmv_path, sizeof(fmv_path), "%s.fmv", base);
    snprintf(wav_path, sizeof(wav_path), "%s.wav", base);
    FILE *outv = fopen(fmv_path, "wb");
    FILE *outa = fopen(wav_path, "wb");
    if (!outv || !outa) { perror("fopen output"); fclose(in); return 1; }

    /* FMV1 header (16 B): "FMV1", w, h, fps, _, nframes. */
    uint8_t fmv1_hdr[16] = {0};
    memcpy(fmv1_hdr, "FMV1", 4);
    wr_u16le(fmv1_hdr + 4, w);
    wr_u16le(fmv1_hdr + 6, h);
    wr_u16le(fmv1_hdr + 8, fps);
    /* fmv1_hdr[10..11] = 0 (pad) */
    wr_u32le(fmv1_hdr + 12, nfr);
    fwrite(fmv1_hdr, 1, 16, outv);

    /* Canonical PCM WAV header (44 B). data_size is patched on close. */
    uint8_t wav_hdr[44];
    uint16_t block_align = (uint16_t)(ach * (abits / 8));
    uint32_t byte_rate   = arate * block_align;
    memcpy(wav_hdr + 0,  "RIFF", 4);
    wr_u32le(wav_hdr + 4, 36 + 0);              /* RIFF chunk size (patched) */
    memcpy(wav_hdr + 8,  "WAVE", 4);
    memcpy(wav_hdr + 12, "fmt ", 4);
    wr_u32le(wav_hdr + 16, 16);                  /* fmt chunk size (PCM) */
    wr_u16le(wav_hdr + 20, 1);                   /* audio format = PCM */
    wr_u16le(wav_hdr + 22, ach);
    wr_u32le(wav_hdr + 24, arate);
    wr_u32le(wav_hdr + 28, byte_rate);
    wr_u16le(wav_hdr + 32, block_align);
    wr_u16le(wav_hdr + 34, abits);
    memcpy(wav_hdr + 36, "data", 4);
    wr_u32le(wav_hdr + 40, 0);                   /* data chunk size (patched) */
    fwrite(wav_hdr, 1, 44, outa);

    /* Demux loop: per frame, read audio bytes -> WAV, video bytes -> FMV1. */
    uint8_t *aud_buf = malloc(abpf);
    uint8_t *vid_buf = malloc(VIDEO_PER_FRAME);
    if (!aud_buf || !vid_buf) { fprintf(stderr, "OOM\n"); return 1; }

    uint32_t frames_done = 0;
    for (uint32_t f = 0; f < nfr; f++) {
        if (fread(aud_buf, 1, abpf, in) != abpf) {
            fprintf(stderr, "short read on audio at frame %u\n", f);
            break;
        }
        if (fread(vid_buf, 1, VIDEO_PER_FRAME, in) != VIDEO_PER_FRAME) {
            fprintf(stderr, "short read on video at frame %u\n", f);
            break;
        }
        fwrite(aud_buf, 1, abpf, outa);
        fwrite(vid_buf, 1, VIDEO_PER_FRAME, outv);
        frames_done++;
    }

    /* Patch the WAV size fields now that we know the total. */
    uint32_t data_bytes = frames_done * abpf;
    fseek(outa, 4, SEEK_SET);
    uint8_t buf4[4];
    wr_u32le(buf4, 36 + data_bytes); fwrite(buf4, 1, 4, outa);
    fseek(outa, 40, SEEK_SET);
    wr_u32le(buf4, data_bytes); fwrite(buf4, 1, 4, outa);

    fclose(in); fclose(outv); fclose(outa);
    free(aud_buf); free(vid_buf);

    fprintf(stderr, "wrote:  %s  (FMV1 video, %u frames)\n", fmv_path, frames_done);
    fprintf(stderr, "        %s  (PCM s16le %u Hz %u ch, %.2f s)\n",
            wav_path, arate, ach, (double)frames_done / (double)fps);
    return 0;
}
