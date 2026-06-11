/* ============================================================
 *  demo_dynamic_letterbox.c — auto-sweeping letterbox + 16-color
 *                              banded backdrop, Phase 3a showcase.
 *
 *  Background: BG1 Mode 1 with a per-scanline-band tilemap. 16 solid-
 *  color tiles cycle vertically so each tilemap row is a distinct
 *  rainbow band. As the letterbox grows from top or bottom, the
 *  outer bands disappear visibly — a clean demonstration that the
 *  kernel's unified default_hirq_handler is driving INIDISP at the
 *  correct scanlines.
 *
 *  Controls:
 *    START   : toggle auto-sweep on/off
 *    LEFT/R  : when sweep is off, manually adjust top letterbox
 *    UP/DOWN : when sweep is off, manually adjust bottom letterbox
 *    SELECT  : exit
 *
 *  Auto-sweep: top and bottom letterbox values are each cycled
 *  independently between 0 and 32 lines with different periods so
 *  the boundaries clearly aren't moving in lockstep.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_game.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* v2.30.7: mg_wait_frame now properly blocks until the SNES kernel
 * has acked the last committed frame, so the demo runs at SNES frame
 * rate without manual sleep_ticks. */

/* 16 tiles × 32 bytes/tile (4bpp): tile N is solid color N.
 * 4bpp planar layout per tile:
 *   bytes 0..15  = rows 0..7 plane 0,1 interleaved (low 2 bits of pixel)
 *   bytes 16..31 = rows 0..7 plane 2,3 interleaved (high 2 bits of pixel)
 * For a solid-color tile with palette index N, each plane is either
 * $00 (bit clear) or $FF (bit set) for all 8 pixels of that row. */
static uint8_t SOLID_TILES[16 * 32];

static void init_solid_tiles(void) {
    for (int n = 0; n < 16; ++n) {
        uint8_t *p = SOLID_TILES + n * 32;
        uint8_t  b0 = (n & 1) ? 0xFFu : 0x00u;
        uint8_t  b1 = (n & 2) ? 0xFFu : 0x00u;
        uint8_t  b2 = (n & 4) ? 0xFFu : 0x00u;
        uint8_t  b3 = (n & 8) ? 0xFFu : 0x00u;
        for (int row = 0; row < 8; ++row) {
            p[row * 2 + 0]      = b0;
            p[row * 2 + 1]      = b1;
            p[16 + row * 2 + 0] = b2;
            p[16 + row * 2 + 1] = b3;
        }
    }
}

/* 16-color rainbow palette. Index 0 stays black (acts as backdrop
 * for the force-blanked letterbox bands, since force-blank means
 * the PPU outputs CGRAM[0] regardless of brightness). */
static const uint8_t RAINBOW[16][3] = {
    {  0,   0,   0},  /*  0 black backdrop                            */
    {255,   0,   0},  /*  1 red                                       */
    {255, 128,   0},  /*  2 orange                                    */
    {255, 255,   0},  /*  3 yellow                                    */
    {128, 255,   0},  /*  4 lime                                      */
    {  0, 255,   0},  /*  5 green                                     */
    {  0, 255, 128},  /*  6 teal                                      */
    {  0, 255, 255},  /*  7 cyan                                      */
    {  0, 128, 255},  /*  8 sky                                       */
    {  0,   0, 255},  /*  9 blue                                      */
    {128,   0, 255},  /* 10 violet                                    */
    {255,   0, 255},  /* 11 magenta                                   */
    {255,   0, 128},  /* 12 hot pink                                  */
    {192, 192, 192},  /* 13 silver                                    */
    {128, 128, 128},  /* 14 grey                                      */
    {255, 255, 255},  /* 15 white                                     */
};

void _start(void) {
    mg_ppu_clean_slate();

    /* BG1 Mode 1, tilemap at word $0000, CHR at word $1000. */
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, 0x0000, MG_BG_SIZE_32x32, 0x1000);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);

    init_solid_tiles();
    MG_OR_PANIC(mg_chr_upload(0x1000, SOLID_TILES, sizeof SOLID_TILES));

    /* Tilemap: row R uses tile ((R + 1) % 16), so row 0 = color 1
     * (red), row 1 = color 2 (orange), ..., wrapping at row 15. We
     * skip color 0 (black backdrop) for tile content since rows
     * filled with tile 0 would visually merge with the letterbox. */
    static MgBgTile tmap[32 * 32];
    for (int row = 0; row < 28; ++row) {
        uint16_t tile = (uint16_t)((row + 1) % 16);
        for (int col = 0; col < 32; ++col) {
            tmap[row * 32 + col].word = tile;
        }
    }
    /* Rows 28..31 stay tile 0 (off-screen / behind bottom letterbox). */
    mg_bg_blit(MG_BG_LAYER_1, 0, 0, tmap, 32 * 32);

    /* Upload palette. */
    for (int i = 0; i < 16; ++i) {
        mg_palette_set_rgb((uint8_t)i,
                           RAINBOW[i][0], RAINBOW[i][1], RAINBOW[i][2]);
    }

    /* Sweep state. */
    bool    sweep_on    = true;
    uint8_t top_lb      = 0;
    uint8_t bot_lb      = 0;
    uint8_t last_top    = 255;  /* force initial write */
    uint8_t last_bot    = 255;
    int8_t  top_dir     = +1;
    int8_t  bot_dir     = +1;
    /* Different periods so top and bottom don't lockstep. With the
     * sleep_one_frame() yield each iteration, the loop runs at ~60 Hz.
     * Top advances every 30 frames (~0.5 s per line, ~32 s for full
     * 0→32→0 cycle), bottom every 24 frames (~0.4 s per line, ~26 s
     * full cycle). Tune these if a different pace is preferred. */
    uint8_t  top_div   = 0;
    uint8_t  bot_div   = 0;

    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_SELECT)) sys_exit(0);
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) {
            sweep_on = !sweep_on;
        }

        if (sweep_on) {
            if (++top_div >= 15) {
                top_div = 0;
                int v = (int)top_lb + top_dir;
                if (v < 0)  { v = 0;  top_dir = +1; }
                if (v > 32) { v = 32; top_dir = -1; }
                top_lb = (uint8_t)v;
            }
            if (++bot_div >= 12) {
                bot_div = 0;
                int v = (int)bot_lb + bot_dir;
                if (v < 0)  { v = 0;  bot_dir = +1; }
                if (v > 32) { v = 32; bot_dir = -1; }
                bot_lb = (uint8_t)v;
            }
        } else {
            /* Manual D-pad mode. */
            if (mg_pad_held(pads.p0, MG_BTN_LEFT)  && top_lb > 0)  top_lb--;
            if (mg_pad_held(pads.p0, MG_BTN_RIGHT) && top_lb < 64) top_lb++;
            if (mg_pad_held(pads.p0, MG_BTN_UP)    && bot_lb > 0)  bot_lb--;
            if (mg_pad_held(pads.p0, MG_BTN_DOWN)  && bot_lb < 64) bot_lb++;
        }

        /* Only push a kernel_layout update when the values actually
         * change. Avoids racing the SNES's NMI-handler read of the
         * cart_window layout bytes against the host's per-iter
         * write — which was producing the visible flicker. */
        if (top_lb != last_top || bot_lb != last_bot) {
            mg_kernel_layout(top_lb, bot_lb);
            last_top = top_lb;
            last_bot = bot_lb;
        }

        mg_frame_commit();
        mg_wait_frame();
    }
}
