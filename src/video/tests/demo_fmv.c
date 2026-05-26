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
    for (int i = 0; i < 1024; i++) P.vram[TMAP_W + i] = BLANK_TILE;   /* blank the nametable */
    for (int r = 0; r < TH; r++)
        for (int c = 0; c < TW; c++) {
            int s = (r*TW + c) * 2;
            P.vram[TMAP_W + r*32 + c] = (uint16_t)(tm[s] | (tm[s+1] << 8));
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
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: demo_fmv <clip.fmv>\n"); return 1; }
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - FMV")) return 1;
    if (!load_clip(argv[1])) return 1;
    setup();
    fflush(stdout);

    double t0 = now_sec();
    int last = -1;
    while (!present_should_close()) {
        double t = now_sec() - t0;
        int vf = (int)(t * fps) % nframes;
        if (vf != last) { load_frame(vf); ppu_render(&P, FB); last = vf; }
        present_frame(FB);
    }
    present_shutdown();
    free(clip);
    return 0;
}
#endif
