/* demo_fmv.c — MANUAL PoC: play a .fmv clip through the emulated SNES PPU.
 *
 * Reads the encoder's .fmv (4bpp, 8 palettes + shared backdrop), loads each
 * frame's CGRAM / tilemap / CHR into VRAM, and renders it as a Mode-1 BG,
 * paced by the file's fps off the wall clock (each video frame shown for the
 * right number of 60Hz presents). The cart double-buffers two VRAM banks and
 * DMAs over 2-4 vblanks; on the host a decode is atomic so one bank suffices.
 *
 * Build + run (Windows):
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_fmv \
 *      src/video/ppu.c src/video/present_gl_win32.c \
 *      src/video/tests/demo_fmv.c -lopengl32 -lgdi32 -luser32
 *   ./build/demo_fmv synth.fmv
 * Headless ASCII (frame 0): add -DFMV_HEADLESS, link only ppu.c.
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#ifndef FMV_HEADLESS
#include "video/present.h"
#include <windows.h>
#include <mmsystem.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VW 240
#define VH 208
#define TW (VW/8)          /* 30 */
#define TH (VH/8)          /* 26 */
#define NTILES (TW*TH)     /* 780 */
#define BLOCK (8*16*2 + NTILES*2 + NTILES*32)   /* 26776 */
#define TMAP_W 0x0000      /* nametable base (word)  */
#define CHR_W  0x2000      /* CHR base (word, 8KB-aligned) */
#define BLANK_TILE NTILES  /* tile 780 = zeroed = backdrop, for the margins */

/* SNES NTSC DMA budget, for the HUD */
#define NTSC_LINES  262
#define LINE_CYC    1364
#define STD_ACTIVE  224
#define LETTERBOX   (STD_ACTIVE - VH)         /* 16 forced-blank lines    */
#define VBLANK_STD  (NTSC_LINES - STD_ACTIVE) /* 38 normal vblank lines   */
#define BLANK_LINES (LETTERBOX + VBLANK_STD)  /* 54 lines of DMA / 60Hz   */
#define DMA_WIN     (BLANK_LINES*LINE_CYC/8)  /* bytes/60Hz (no joypad read) */

static PpuState P;
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
static uint8_t *clip;          /* whole .fmv in memory */
static int nframes, fps = 20;

static void load_frame(int f) {
    const uint8_t *blk = clip + 16 + (long)f * BLOCK;
    const uint8_t *cg = blk;
    const uint8_t *tm = blk + 8*16*2;
    const uint8_t *ch = tm + NTILES*2;
    memcpy(P.cgram, cg, 8*16*2);                          /* 128 palette entries */
    for (int i = 0; i < 1024; i++) P.vram[TMAP_W + i] = BLANK_TILE;   /* blank -> black backdrop */
    for (int r = 0; r < TH; r++)                                      /* centered: 8px margin all round */
        for (int c = 0; c < TW; c++) {
            int s = (r*TW + c) * 2;
            P.vram[TMAP_W + (r+1)*32 + (c+1)] = (uint16_t)(tm[s] | (tm[s+1] << 8));
        }
    memcpy(&P.vram[CHR_W], ch, NTILES*32);                /* 12480 words of CHR */
}

static void setup(void) {
    ppu_state_clear(&P);                                  /* zeroes VRAM -> tile BLANK_TILE stays blank */
    P.mode = 1;
    P.brightness = 15;
    P.bg[0].tilemap_word = TMAP_W;
    P.bg[0].char_word    = CHR_W;
    P.bg[0].size         = PPU_SC_32x32;
    P.bg[0].hofs = 0; P.bg[0].vofs = 0;
    P.bg[0].on_main = true;
}

static int load_clip(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    clip = malloc(sz);
    if (fread(clip, 1, sz, f) != (size_t)sz) { fclose(f); return 0; }
    fclose(f);
    if (memcmp(clip, "FMV1", 4) != 0) { fprintf(stderr, "not a .fmv\n"); return 0; }
    fps     = clip[8] | (clip[9] << 8);
    nframes = clip[12] | (clip[13]<<8) | (clip[14]<<16) | (clip[15]<<24);
    if (fps < 1) fps = 20;
    printf("%s: %d frames @ %d fps (%.1fs), %d B/frame\n", path, nframes, fps, (double)nframes/fps, BLOCK);
    return nframes > 0;
}

#ifdef FMV_HEADLESS
int main(int argc, char **argv) {
    if (argc < 2 || !load_clip(argv[1])) return 1;
    setup();
    load_frame(0);
    ppu_render(&P, FB);
    for (int y = 0; y < PPU_SCREEN_H; y += 7) {
        for (int x = 0; x < PPU_SCREEN_W; x += 4) {
            uint32_t c = FB[y*PPU_SCREEN_W + x];
            unsigned r = c & 0xFF, g = (c>>8)&0xFF, b = (c>>16)&0xFF, lum = (r+g+b)/3;
            putchar(lum < 50 ? ' ' : lum < 110 ? '.' : lum < 180 ? ':' : '#');
        }
        putchar('\n');
    }
    return 0;
}
#else
static double now_sec(void) {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

/* ---- audio = master clock (sidecar raw PCM: s16le, stereo, AUDIO_RATE) ---- */
#define AUDIO_RATE 44100        /* CD quality, matching the mixer's 16-bit signed output */
static HWAVEOUT g_hwo;
static WAVEHDR  g_hdr;
static char    *g_pcm;
static DWORD    g_total;        /* sample-frames in the clip */
static int      g_audio;

static int load_audio(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { perror(path); return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    g_pcm = malloc(sz);
    if (fread(g_pcm, 1, sz, f) != (size_t)sz) { fclose(f); return 0; }
    fclose(f);
    g_total = (DWORD)(sz / 4);                       /* stereo s16 = 4 bytes/frame */
    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 2; wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 4; wf.nAvgBytesPerSec = AUDIO_RATE * 4;
    if (waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        fprintf(stderr, "waveOutOpen failed\n"); return 0;
    }
    memset(&g_hdr, 0, sizeof g_hdr); g_hdr.lpData = g_pcm; g_hdr.dwBufferLength = (DWORD)sz;
    waveOutPrepareHeader(g_hwo, &g_hdr, sizeof g_hdr);
    waveOutWrite(g_hwo, &g_hdr, sizeof g_hdr);
    g_audio = 1;
    printf("audio: %s (%.1fs @ %d Hz stereo) = master clock\n", path, (double)g_total/AUDIO_RATE, AUDIO_RATE);
    return 1;
}
static int audio_video_frame(void) {                 /* current video frame from the play cursor */
    MMTIME mt; mt.wType = TIME_SAMPLES;
    waveOutGetPosition(g_hwo, &mt, sizeof mt);
    DWORD pos = (mt.wType == TIME_SAMPLES) ? mt.u.sample
              : (mt.wType == TIME_BYTES)   ? mt.u.cb / 4 : 0;
    if (pos >= g_total) {                             /* clip ended -> loop audio + video together */
        waveOutReset(g_hwo); g_hdr.dwFlags &= ~WHDR_DONE;
        waveOutWrite(g_hwo, &g_hdr, sizeof g_hdr); pos = 0;
    }
    int vf = (int)((long long)pos * fps / AUDIO_RATE);
    return vf < 0 ? 0 : vf >= nframes ? nframes - 1 : vf;
}
static void audio_shutdown(void) {
    if (!g_audio) return;
    waveOutReset(g_hwo); waveOutUnprepareHeader(g_hwo, &g_hdr, sizeof g_hdr);
    waveOutClose(g_hwo); free(g_pcm);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: demo_fmv <clip.fmv> [audio.pcm]\n"); return 1; }
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - FMV")) return 1;
    if (!load_clip(argv[1])) return 1;
    if (argc >= 3) load_audio(argv[2]);
    setup();
    fflush(stdout);

    double t0 = now_sec(), report = t0;
    int last = -1; unsigned frames = 0; double hostfps = 0;
    int vbpf = (60 + fps - 1) / fps;                  /* 60Hz windows per video frame (3 @20fps) */
    while (!present_should_close()) {
        double now = now_sec();
        int vf;
        if (g_audio) vf = audio_video_frame();        /* audio drives video */
        else vf = (int)((now - t0) * fps) % nframes;
        if (vf != last) { load_frame(vf); ppu_render(&P, FB); last = vf; }

        int need = BLOCK, avail = vbpf * DMA_WIN;
        char ov[512];
        snprintf(ov, sizeof ov,
            "SNES PPU emulated (Mode 1, 4bpp) - FMV streamed from coprocessor\n"
            "video res     : %d x %d   (4bpp, 8 palettes/frame, uncompressed)\n"
            "lines rendered: %d active / %d total\n"
            "blank window  : %d lines (%d forced-blank + %d vblank, no joypad read)\n"
            "DMA bandwidth : %d B / 60Hz frame   (%d*%d/8)\n"
            "DMA @%d fps    : %d B need | %d avail (%d windows) -> %s\n"
            "framerate     : %d fps video | %.1f host present | audio %s\n"
            "keys          : I info | V vsync | F filter | F11 fullscreen | Esc quit",
            VW, VH, VH, NTSC_LINES,
            BLANK_LINES, LETTERBOX, VBLANK_STD,
            DMA_WIN, BLANK_LINES, LINE_CYC,
            fps, need, avail, vbpf, (need <= avail ? "FITS" : "OVER"),
            fps, hostfps, g_audio ? "ON (44.1kHz)" : "off (pass a .pcm)");
        present_set_overlay(ov);
        present_frame(FB);
        frames++;
        if (now - report >= 1.0) { hostfps = (double)frames / (now - report); report = now; frames = 0; }
    }
    audio_shutdown();
    present_shutdown();
    free(clip);
    return 0;
}
#endif
