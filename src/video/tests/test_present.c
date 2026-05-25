/* test_present.c — MANUAL visual harness for the present shim.
 *
 * Opens a window and animates a test pattern. This is NOT a unit test
 * (it needs a display and runs until you close it), so it's not part of
 * run_tests.sh. Build + run it by hand on Windows:
 *
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude \
 *      -o build/present_test \
 *      src/video/present_gl_win32.c src/video/tests/test_present.c \
 *      -lopengl32 -lgdi32 -luser32
 *   ./build/present_test          (add .exe on native mingw)
 *
 * What to look for:
 *   - 8 vertical color bars; a grayscale gradient across the bottom.
 *   - A solid RED 16x16 block in the TOP-LEFT corner (orientation check
 *     — if it's anywhere else, the image is flipped/mirrored).
 *   - A white vertical line sweeping left->right (motion / vsync — it
 *     should glide smoothly with no tearing if vsync is on).
 *   - F11 toggles fullscreen, A toggles aspect (4:3 <-> square pixels),
 *     F toggles filtering (nearest <-> linear), Esc / window-X quits.
 *
 * Public domain (CC0). No warranty.
 */
#include "video/present.h"

#include <stdio.h>
#include <stdint.h>

#define W 256
#define H 224

int main(void) {
    static uint32_t fb[W * H];

    if (!present_init(W, H, "microgarbage - present shim test")) {
        fprintf(stderr, "present_init failed "
                        "(no windowing on this platform, or GL context refused)\n");
        return 1;
    }
    printf("present test: F11 fullscreen | A aspect | F filter | Esc quit\n");

    /* 8 SMPTE-ish vertical color bars (0..235 to stay in 'broadcast' range). */
    static const uint8_t bars[8][3] = {
        {235, 235, 235}, {235, 235,  16}, { 16, 235, 235}, { 16, 235,  16},
        {235,  16, 235}, {235,  16,  16}, { 16,  16, 235}, { 16,  16,  16},
    };

    unsigned frame = 0;
    while (!present_should_close()) {
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                uint8_t r, gg, b;
                if (y >= H - 32) {                       /* bottom: gradient */
                    uint8_t v = (uint8_t)(x * 255 / (W - 1));
                    r = gg = b = v;
                } else {                                 /* color bars */
                    int bar = x * 8 / W;
                    r  = bars[bar][0];
                    gg = bars[bar][1];
                    b  = bars[bar][2];
                }
                fb[y * W + x] = PRESENT_RGBA(r, gg, b);
            }
        }

        /* moving vertical white line (motion / vsync check) */
        int lx = (int)(frame % (unsigned)W);
        for (int y = 0; y < H; y++) fb[y * W + lx] = PRESENT_RGBA(255, 255, 255);

        /* top-left 16x16 red orientation marker */
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                fb[y * W + x] = PRESENT_RGBA(255, 0, 0);

        present_frame(fb);
        frame++;
    }

    present_shutdown();
    return 0;
}
