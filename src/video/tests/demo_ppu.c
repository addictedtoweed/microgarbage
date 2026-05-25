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

#define BLUE555   0x7C00u
#define GREEN555  0x03E0u
#define RED555    0x001Fu
#define NAVY555   0x2108u   /* dark backdrop */

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
}

int main(void) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - PPU demo")) {
        return 1;
    }
    build_scene();

    unsigned frame = 0;
    while (!present_should_close()) {
        P.bg[0].hofs = (uint16_t)(frame / 2u);   /* diagonal scroll */
        P.bg[0].vofs = (uint16_t)(frame / 3u);
        ppu_render(&P, FB);
        present_frame(FB);
        frame++;
    }

    present_shutdown();
    return 0;
}
