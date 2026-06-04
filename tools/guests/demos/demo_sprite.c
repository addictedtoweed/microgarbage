/* ============================================================
 *  demo_sprite.c — single 16×16 sprite walked by the D-pad.
 *
 *  Demonstrates: mg_sprite_chr_base + mg_sprite_sizes, the OAM
 *  shadow path via mg_sprite_set / mg_sprite_move, and the per-VM
 *  edge detection in mg_pad_pressed. Sprite wraps at the screen
 *  edges (256 wide, 224 tall). START exits.
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

/* 4 tiles (= 16×16 sprite at OBSEL small=8, large=16). Each tile is
 * solid color 1; the four together fill the 16×16 sprite quad. The
 * SNES OBJ CHR layout indexes 8×8 tiles in a 16-tiles-per-row grid,
 * so the four tiles of a 16×16 sprite live at tile, tile+1, tile+16,
 * tile+17 — we put ours at 0, 1, 16, 17 by writing tiles 0..17 of
 * the OBJ CHR area, with 2..15 zeroed (invisible). */
static uint8_t SPR_CHR[18 * 32];

static void init_spr_chr(void) {
    for (int i = 0; i < (int)sizeof SPR_CHR; i++) SPR_CHR[i] = 0;
    /* Tile 0 + 1 (top row), 16 + 17 (bottom row) all "all color 1". */
    static const int LIT[4] = { 0, 1, 16, 17 };
    for (int k = 0; k < 4; k++) {
        uint8_t *p = SPR_CHR + LIT[k] * 32;
        for (int row = 0; row < 8; row++) {
            p[row * 2 + 0] = 0xFF;   /* plane 0: all pixels = 1 */
            p[row * 2 + 1] = 0;      /* plane 1 */
        }
        for (int i = 16; i < 32; i++) p[i] = 0;   /* planes 2,3 */
    }
}

void _start(void) {
    init_spr_chr();

    /* Wipe leftover shadow state + BG tilemap data from any prior demo.
     * v1.28's partial mg_ppu_clean_slate dirties all BG shadows so the
     * next commit zeroes the active layer's tilemap area, clearing the
     * stale tile references that previously caused 8x8 colored blocks
     * at the top of the screen after running mode7.elf or mode7_3d.elf.
     * Full VRAM clear (including CHR data) is still a known limitation
     * (see session notes task #6). */
    mg_ppu_clean_slate();

    /* Background: black so the sprite is the only thing on screen.
     * We still enable BG1 with an empty tilemap so the PPU's main
     * screen isn't completely dead (some emulators get unhappy). */
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, 0x0000, MG_BG_SIZE_32x32, 0x1000);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);

    /* OBSEL: pick 8×8 / 16×16 size pair; CHR bases at VRAM word
     * $4000 (typical SNES default). */
    mg_sprite_sizes(MG_SPR_SIZES_8_16);
    mg_sprite_chr_base(0x4000, 0x5000);

    MG_OR_PANIC(mg_chr_upload(0x4000, SPR_CHR, sizeof SPR_CHR));

    /* BG palette: 0 = black backdrop (only color visible on BG1
     * since the tilemap stays at tile 0 and we never upload its CHR).
     * Sprite palette starts at CGRAM entry 128 — entries 128..135 are
     * sprite palette 0, etc. We paint color 129 = yellow. */
    mg_palette_set_rgb(0,   0,   0,   0);
    mg_palette_set_rgb(129, 255, 232, 64);

    MgSprite spr = {
        .x = 120, .y = 104,   /* near screen center */
        .tile = 0,
        .palette = 0, .priority = 2,
        .hflip = 0, .vflip = 0,
        .size_large = 1,      /* picks the 16×16 size for this slot */
    };
    mg_sprite_set(0, &spr);

    int16_t x = spr.x;
    int16_t y = spr.y;

    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) sys_exit(0);

        if (mg_pad_held(pads.p0, MG_BTN_LEFT))  x--;
        if (mg_pad_held(pads.p0, MG_BTN_RIGHT)) x++;
        if (mg_pad_held(pads.p0, MG_BTN_UP))    y--;
        if (mg_pad_held(pads.p0, MG_BTN_DOWN))  y++;

        /* Wrap. 256-wide field; treat 224 as visible height. */
        if (x < -16) x = 255;
        if (x > 255) x = -16;
        if (y < 0)   y = 223;
        if (y > 223) y = 0;

        mg_sprite_move(0, x, (uint8_t)y);

        mg_frame_commit();
        mg_wait_frame();
    }
}
