/* demo_ppu.c — MANUAL visual demo: PPU rasterizer -> present window.
 *
 * Builds a small Mode 1 scene (a checkerboard tilemap with scattered
 * framed tiles) and scrolls it diagonally, rendering each frame with
 * ppu_render() and showing it through the WGL present shim. This is the
 * harness for visually validating the background pipeline — and a
 * starting point: replace build_scene() with your real VRAM / CGRAM /
 * tilemap to preview actual game graphics.
 *
 * Not a unit test (needs a display). Build + run on Windows:
 *
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude \
 *      -o build/demo_ppu \
 *      src/video/ppu.c src/video/present_gl_win32.c src/video/tests/demo_ppu.c \
 *      -lopengl32 -lgdi32 -luser32
 *   ./build/demo_ppu              (add .exe on native mingw)
 *
 * Controls (from the present shim): F11 fullscreen, A aspect, F filter,
 * Esc / window-X to quit. You should see a scrolling checkerboard with
 * red-framed tiles drifting diagonally and wrapping.
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#include "video/present.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <windows.h>   /* QueryPerformanceCounter (Windows-only harness) */

/* High-resolution wall-clock seconds for the frame-rate probe. */
static double now_sec(void) {
    LARGE_INTEGER freq, ctr;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&ctr);
    return (double)ctr.QuadPart / (double)freq.QuadPart;
}

#define BLUE555   0x7C00u
#define GREEN555  0x03E0u
#define RED555    0x001Fu
#define NAVY555   0x2108u   /* dark backdrop */

#define SNES_NTSC_HZ 60.0988   /* SNES NTSC frame rate (PAL would be ~50.007) */

static PpuState  P;
static uint32_t  FB[PPU_SCREEN_W * PPU_SCREEN_H];

static void set_tile_4bpp(uint16_t *vram, unsigned cw, unsigned tile,
                          uint8_t p[8][8]) {
    unsigned base = (cw + tile * 16u) & 0x7FFFu;
    for (unsigned r = 0; r < 8; r++) {
        unsigned p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (unsigned x = 0; x < 8; x++) {
            unsigned v = p[r][x], b = 7u - x;
            p0 |= (v & 1u) << b;        p1 |= ((v >> 1) & 1u) << b;
            p2 |= ((v >> 2) & 1u) << b; p3 |= ((v >> 3) & 1u) << b;
        }
        vram[(base + r) & 0x7FFFu]      = (uint16_t)(p0 | (p1 << 8));
        vram[(base + 8u + r) & 0x7FFFu] = (uint16_t)(p2 | (p3 << 8));
    }
}

/* Place sprite 0 (16x16, tile 1, OBJ pal 0, priority 2 = above the BG). */
static void place_sprite0(int x, int y) {
    P.oam[0] = (uint8_t)((unsigned)x & 0xFFu);
    P.oam[1] = (uint8_t)y;
    P.oam[2] = 1;                                  /* tile 1 */
    P.oam[3] = (uint8_t)(2u << 4);                 /* pal 0, prio 2, no flip */
    unsigned xhi = ((unsigned)x >> 8) & 1u;
    P.oam[512] = (uint8_t)((P.oam[512] & ~3u) | (xhi | (1u << 1))); /* X hi + big */
}

static void build_scene(void) {
    ppu_state_clear(&P);
    P.mode = 1;
    P.bg[0].on_main = true;
    P.bg[0].tilemap_word = 0x1000;
    P.bg[0].char_word    = 0x2000;
    P.bg[0].size = PPU_SC_32x32;

    /* Palette (pal 0): backdrop + three solid colors. */
    P.cgram[0] = NAVY555;
    P.cgram[1] = BLUE555;
    P.cgram[2] = GREEN555;
    P.cgram[3] = RED555;

    /* Tiles: 1 = solid blue, 2 = solid green, 3 = green tile with a red
     * frame (so tile edges and motion are obvious). */
    uint8_t t[8][8];
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) t[r][c] = 1;
    set_tile_4bpp(P.vram, 0x2000, 1, t);
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) t[r][c] = 2;
    set_tile_4bpp(P.vram, 0x2000, 2, t);
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++)
        t[r][c] = (r == 0 || r == 7 || c == 0 || c == 7) ? 3 : 2;
    set_tile_4bpp(P.vram, 0x2000, 3, t);

    /* 32x32 checkerboard with a framed tile every 5th cell. */
    for (unsigned ty = 0; ty < 32; ty++) {
        for (unsigned tx = 0; tx < 32; tx++) {
            uint16_t tile = ((tx + ty) & 1u) ? 1u : 2u;
            if (((tx * 3u + ty) % 5u) == 0u) tile = 3u;
            P.vram[(0x1000 + ty * 32u + tx) & 0x7FFFu] = tile;  /* pal 0, prio 0 */
        }
    }

    /* A 16x16 solid-yellow sprite (OBJ) that bounces over the BG, so the
     * sprite path + sprite-over-BG priority are visible too. */
    P.obj_on_main   = true;
    P.obj_size_sel  = 0;             /* size pair 0: small 8x8 / large 16x16 */
    P.obj_char_word = 0x4000;
    P.obj_gap_word  = 0;
    P.cgram[128 + 1] = 0x03FFu;      /* OBJ pal0 index 1 = yellow (r+g) */
    uint8_t sp[8][8];
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) sp[r][c] = 1;
    set_tile_4bpp(P.vram, 0x4000, 1, sp);   /* 16x16 = tiles 1,2,17,18 */
    set_tile_4bpp(P.vram, 0x4000, 2, sp);
    set_tile_4bpp(P.vram, 0x4000, 17, sp);
    set_tile_4bpp(P.vram, 0x4000, 18, sp);
    place_sprite0(20, 20);
}

int main(void) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - PPU demo")) {
        return 1;
    }
    build_scene();

    /* vsync diagnostics: the renderer string proves HW vs software GL,
     * and the printed fps should pin near your monitor refresh (e.g.
     * ~60) if vsync is on, or run into the hundreds/thousands if not. */
    printf("GL renderer : %s\n", present_gl_renderer());
    printf("vsync       : %s\n",
           present_vsync_requested() ? "enabled (WGL_EXT_swap_control)"
                                     : "NOT enabled");
    printf("pacing emulated frames at %.4f Hz; presenting vsync'd...\n", SNES_NTSC_HZ);
    fflush(stdout);

    /* Fixed-timestep loop: advance the emulated frame at the SNES rate
     * on a wall clock, present every vsync (re-showing the latest frame
     * when no new emulated frame is due). The game runs at true SNES
     * speed independent of the monitor's refresh. */
    const double target_dt = 1.0 / SNES_NTSC_HZ;
    double last = now_sec(), report_t0 = last, acc = 0.0;
    unsigned emu = 0, presents = 0, emu_window = 0;
    int sx = 20, sy = 20, vx = 2, vy = 1;        /* bouncing sprite */

    ppu_render(&P, FB);                          /* first frame before the loop */

    while (!present_should_close()) {
        double t = now_sec();
        acc += t - last;
        last = t;
        if (acc > 0.25) acc = 0.25;              /* clamp catch-up after a hitch */

        bool stepped = false;
        while (acc >= target_dt) {               /* advance emulated frame(s) due */
            emu++; emu_window++;
            acc -= target_dt;
            stepped = true;
        }
        if (stepped) {                           /* re-render only on a new frame */
            P.bg[0].hofs = (uint16_t)(emu / 2u); /* diagonal scroll */
            P.bg[0].vofs = (uint16_t)(emu / 3u);
            sx += vx; sy += vy;                  /* bounce the sprite */
            if (sx < 0) { sx = 0; vx = -vx; }
            if (sx > PPU_SCREEN_W - 16) { sx = PPU_SCREEN_W - 16; vx = -vx; }
            if (sy < 0) { sy = 0; vy = -vy; }
            if (sy > PPU_SCREEN_H - 16) { sy = PPU_SCREEN_H - 16; vy = -vy; }
            place_sprite0(sx, sy);
            ppu_render(&P, FB);
        }
        present_frame(FB);                       /* vsync-throttled */
        presents++;

        double dt = t - report_t0;
        if (dt >= 1.0) {                         /* once per second */
            printf("  present %.1f fps  |  emulated %.2f fps  (target %.4f)\n",
                   presents / dt, emu_window / dt, SNES_NTSC_HZ);
            fflush(stdout);
            report_t0 = t; presents = 0; emu_window = 0;
        }
    }

    present_shutdown();
    return 0;
}
