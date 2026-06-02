/* ============================================================
 *  demo_letterbox.c — toggle force-blank letterboxing on START.
 *
 *  Three letterbox heights are cycled by pressing START:
 *      (0, 0)   no letterbox — full 224 visible
 *      (8, 8)   "demo TV" — 208 visible
 *      (16, 16) "movie" — 192 visible
 *
 *  Demonstrates: mg_force_blank + the runtime's INIDISP HDMA-channel-7
 *  table rebuild. The visible region snaps to the new height on the
 *  next frame. BG content is a solid blue plane so the contrast
 *  between visible-vs-blanked bands is unmistakable.
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

/* Same all-color-1 4bpp tile as demo_palette.c. */
static const uint8_t SOLID_CHR[32] = {
    0xFF, 0,  0xFF, 0,  0xFF, 0,  0xFF, 0,
    0xFF, 0,  0xFF, 0,  0xFF, 0,  0xFF, 0,
    0,    0,  0,    0,  0,    0,  0,    0,
    0,    0,  0,    0,  0,    0,  0,    0,
};

static const uint8_t PRESETS[3][2] = {
    { 0,  0  },   /* full screen */
    { 8,  8  },   /* demo TV */
    { 16, 16 },   /* movie */
};

void _start(void) {
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, 0x0000, MG_BG_SIZE_32x32, 0x1000);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);

    MG_OR_PANIC(mg_chr_upload(0x1000, SOLID_CHR, sizeof SOLID_CHR));

    static MgBgTile tmap[32 * 32];
    for (int i = 0; i < 32 * 32; i++) tmap[i].word = 0;
    mg_bg_blit(MG_BG_LAYER_1, 0, 0, tmap, 32 * 32);

    mg_palette_set_rgb(0, 0,    0,    0  );   /* backdrop = black  */
    mg_palette_set_rgb(1, 64,   128,  255);   /* visible plane = sky blue */

    uint8_t preset = 0;
    mg_force_blank(PRESETS[0][0], PRESETS[0][1]);

    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_SELECT)) sys_exit(0);
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) {
            preset = (uint8_t)((preset + 1u) % 3u);
            mg_force_blank(PRESETS[preset][0], PRESETS[preset][1]);
        }

        mg_frame_commit();
        mg_wait_frame();
    }
}
