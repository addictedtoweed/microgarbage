/* ============================================================
 *  game.c — canonical microgarbage game.elf skeleton.
 *
 *  What this does, line for line:
 *    boot() — picks BG mode, lays out CHR + tilemap, starts music
 *    main() — per-frame: read pads, move sprite, commit
 *
 *  Copy this directory, replace assets/, rewrite the loop body,
 *  ship. The mg_* API surface is documented at docs/game-api.md;
 *  for the foundational concepts (frame pacing, DMA model, panic
 *  flow) see that doc.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "mg_game.h"

/* Asset blobs — these are produced by `build.sh` running png_to_chr
 * over assets PNG inputs and bin2c over the resulting CHR / palette
 * binaries. Until the asset pipeline lands, the includes are
 * commented out and boot() will use the no-asset code path that
 * paints solid colors.
 *
 *   #include "generated/player_chr.h"
 *   #include "generated/player_pal.h"
 *   #include "generated/hud_font_chr.h"
 *   #include "generated/hud_font_pal.h"
 */

/* VRAM layout for this demo. Pick yours per game; the comments are a
 * reminder of the standard convention: BG CHR low, BG tilemap middle,
 * sprite CHR high. The runtime owns no permanent VRAM reservation
 * during normal play (the panic path resets the SNES and reboots
 * into error.elf, which then gets the full VRAM to itself). */
#define BG2_CHR_BASE       0x0000
#define BG2_MAP_BASE       0x1000
#define SPRITE_CHR_BASE    0x4000

#define HERO_SLOT          0

static int  hero_x = 128;
static int  hero_y = 112;

static void boot(void) {
    /* Mode 1: BG1+BG2 4bpp, BG3 2bpp. We use BG2 only here. */
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_2,
                BG2_MAP_BASE / 2, MG_BG_SIZE_32x32,
                BG2_CHR_BASE / 2);
    mg_bg_enable(MG_BG_LAYER_2, /*main=*/true, /*sub=*/false);

    mg_sprite_sizes(MG_SPR_SIZES_8_16);
    mg_sprite_chr_base(SPRITE_CHR_BASE / 2, 0);

    /* Once the asset pipeline is in place, uncomment these. They
     * trigger 2 DMA slots + ~2 KB against the byte budget at boot.
     *
     * MG_OR_PANIC(mg_chr_upload(SPRITE_CHR_BASE,
     *                           player_chr, player_chr_len));
     * MG_OR_PANIC(mg_chr_upload(BG2_CHR_BASE,
     *                           hud_font_chr, hud_font_chr_len));
     *
     * mg_palette_load(128, (const uint16_t *)player_pal,
     *                 player_pal_len / 2);
     * mg_palette_load(0,   (const uint16_t *)hud_font_pal,
     *                 hud_font_pal_len / 2);
     *
     * static const char hud[] = "HELLO WORLD";
     * for (int i = 0; hud[i]; i++) {
     *     MgBgTile t;
     *     t.word = 0;
     *     t.tile = (uint16_t)hud[i];
     *     t.palette = 0;
     *     mg_bg_set_tile(MG_BG_LAYER_2, (uint8_t)(1 + i), 1, t);
     * }
     *
     * (void)mg_stream_play("/cart/music.wav", MG_AUDIO_LOOP);
     */

    /* No-asset fallback: paint a single solid color into sprite
     * palette slot 0 so the hero shows as a colored block. */
    mg_palette_set_rgb(128 + 1, 0xFF, 0xC0, 0x40);   /* amber */
}

/* Guest entry point. The RV32IMC freestanding ELFs that this runtime
 * loads enter at _start, not main. Returning from _start is
 * undefined here — for(;;) is the contract. A future runtime version
 * with a "return to launcher" path will define mg_exit_to_launcher(). */
void _start(void) {
    boot();

    for (;;) {
        mg_wait_frame();
        MgPads p = mg_pads();

        if (mg_pad_held(p.p0, MG_BTN_LEFT))  hero_x--;
        if (mg_pad_held(p.p0, MG_BTN_RIGHT)) hero_x++;
        if (mg_pad_held(p.p0, MG_BTN_UP))    hero_y--;
        if (mg_pad_held(p.p0, MG_BTN_DOWN))  hero_y++;

        if (mg_pad_pressed(p.p0, MG_BTN_START))
            mg_panic("debug stop");

        MgSprite s = {
            .x          = (int16_t)hero_x,
            .y          = (uint8_t)hero_y,
            .tile       = 0,
            .palette    = 0,
            .priority   = 2,
            .size_large = 1,
        };
        mg_sprite_set(HERO_SLOT, &s);

        mg_frame_commit();
    }
}
