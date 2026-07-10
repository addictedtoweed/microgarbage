/* ============================================================
 *  demo_nmibody.c — prove the v2.44 custom transfer-body seam.
 *
 *  Render path is a near-copy of demo_dynamic_letterbox (16 solid
 *  tiles + one static tilemap + one blit + full palette — a proven
 *  render), drawing a fine per-tile rainbow field. On top of that it
 *  lets you switch the SNES kernel's per-frame transfer body live:
 *
 *      A     -> CUSTOM body: jsr the proven kernel chainer (via the
 *               K_ABI_FRAME_DMA jump-table) AND write MOSAIC=$F1.
 *               The fine field pixelates into coarse blocks — an
 *               effect the staged frame never asked for, so it can
 *               only come from the guest body.
 *      B     -> mg_nmi_build_default(): jsr the chainer, rts.
 *               Byte-behaviorally identical to the built-in backup,
 *               so the field snaps back to fine.
 *      START -> exit.
 *
 *  At startup nothing is installed (pure kernel backup).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_game.h"
#include "mg_nmi.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* 16 solid-color tiles (tile N = solid palette index N), 4bpp planar
 * — identical construction to demo_dynamic_letterbox. */
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

/* Palette: 0 = BLUE backdrop (diagnostic: distinguishes "BG1 checker
 * failed" (screen stays blue) from "nothing renders" (black)), 1 red,
 * 15 white (the checker colors). */
static const uint8_t PAL[16][3] = {
    {0,0,255},{255,0,0},{255,128,0},{255,255,0},{128,255,0},{0,255,0},
    {0,255,128},{0,255,255},{0,128,255},{0,0,255},{128,0,255},{255,0,255},
    {255,0,128},{192,192,192},{128,128,128},{255,255,255},
};

/* 1 KB builder buffer — keep it off the guest stack. */
static MgNmi   g_body;
static MgBgTile g_tmap[32 * 32];

static void install_default_body(void) {
    mg_nmi_build_default(&g_body);              /* jsr ABI:frame_dma; rts */
    if (mg_nmi_finish(&g_body, 16) < 0) mg_panic("nmibody: default overrun");
    mg_nmi_install(&g_body);
}

static void install_mosaic_body(void) {
    mg_nmi_begin(&g_body);
    mg_nmi_emit_call_default(&g_body);              /* run the staged transfer   */
    mg_nmi_emit_store_imm8(&g_body, 0x2106, 0xF1);  /* then MOSAIC size 15, BG1  */
    mg_nmi_emit_body_end(&g_body);                  /* rts                       */
    if (mg_nmi_finish(&g_body, 16) < 0) mg_panic("nmibody: mosaic overrun");
    mg_nmi_install(&g_body);
}

void _start(void) {
    mg_ppu_clean_slate();

    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, /*tilemap_word=*/0x0000, MG_BG_SIZE_32x32,
                /*chr_word=*/0x1000);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);

    init_solid_tiles();
    MG_OR_PANIC(mg_chr_upload(0x1000, SOLID_TILES, sizeof SOLID_TILES));

    /* Per-tile rainbow field: every 8×8 tile a different solid color,
     * high spatial frequency, so the custom body's MOSAIC visibly
     * pixelates it into coarse 2×2-tile blocks. (A checker would
     * collapse under mosaic — every block samples the same parity and
     * goes uniform; a varied field does not.) tile index == color index,
     * and SOLID_TILES[N] is solid palette color N. */
    for (int row = 0; row < 32; ++row)
        for (int col = 0; col < 32; ++col)
            g_tmap[row * 32 + col].word =
                (uint16_t)(((row * 5 + col * 3) % 15) + 1);
    mg_bg_blit(MG_BG_LAYER_1, 0, 0, g_tmap, 32 * 32);

    for (int i = 0; i < 16; ++i)
        mg_palette_set_rgb((uint8_t)i, PAL[i][0], PAL[i][1], PAL[i][2]);

    for (;;) {
        MgPads p = mg_pads();
        if (mg_pad_pressed(p.p0, MG_BTN_A))     install_mosaic_body();
        if (mg_pad_pressed(p.p0, MG_BTN_B))     install_default_body();
        if (mg_pad_pressed(p.p0, MG_BTN_START)) break;

        /* Re-touch backdrop each frame so a tiny CGRAM slot always stages
         * -> frame_ready set -> the transfer body runs every frame. */
        mg_palette_set_rgb(0, 0, 0, 255);
        mg_frame_commit();
        mg_wait_frame();
    }
    sys_exit(0);
}
