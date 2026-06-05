/* ============================================================
 *  demo_mode7_3d.c -- Mode 7 with a horizon. F-Zero / Mario Kart
 *                     "looking-at-the-horizon" perspective.
 *
 *  Same checkerboard plane as demo_mode7, but the matrix changes
 *  per-scanline via HDMA on M7A..M7D so the ground recedes to a
 *  horizon at row 96. Lines above the horizon stay (0,0,0,0) and
 *  draw as backdrop (MG_MODE7_FILL_BLACK) -- that's the sky band.
 *
 *  Controls:
 *      L / R       yaw CCW / CW
 *      UP / DN     forward / back along yaw
 *      LEFT/RIGHT  strafe perpendicular to yaw
 *      A / B       camera height up / down (steeper / flatter view)
 *      SELECT      reset
 *      START       exit
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_game.h"

#include "math/trig_q16.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* ----------------------------------------------------------------
 *  Same checkerboard tile as demo_mode7. See that file for the
 *  Mode 7 VRAM-interleave rationale.
 * ---------------------------------------------------------------- */
static uint8_t s_chr_interleaved[128];

static void build_chr(void) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            uint8_t pix = (((y >> 1) ^ (x >> 1)) & 1) ? 1 : 2;
            s_chr_interleaved[idx * 2 + 0] = 0;     /* tilemap byte */
            s_chr_interleaved[idx * 2 + 1] = pix;   /* CHR byte     */
        }
    }
}

/* HDMA tables for the four M7 matrix registers. mg_mode7_camera3d
 * fills these every frame -- the camera's yaw + height changing
 * means the table contents change too. Static so they live in BSS,
 * not stack. */
static uint8_t s_tab_m7a[MG_MODE7_3D_TABLE_BYTES];
static uint8_t s_tab_m7b[MG_MODE7_3D_TABLE_BYTES];
static uint8_t s_tab_m7c[MG_MODE7_3D_TABLE_BYTES];
static uint8_t s_tab_m7d[MG_MODE7_3D_TABLE_BYTES];

#define Q16(x)        ((q16_16_t)((int32_t)(x) * Q16_ONE))
#define Q16_FROM_DEG(d) ((q16_16_t)(((int64_t)(d) * Q16_TWO_PI) / 360))

void _start(void) {
    build_chr();

    /* Mode 7 + FILL_BLACK so sky lines (which get a degenerate
     * (0,0,0,0) matrix from mg_mode7_camera3d) draw as the
     * backdrop color rather than wrapping garbage from the plane.
     *
     * NOTE: don't call mg_ppu_clean_slate here. BG1's tilemap_word
     * defaults to 0 and clean_slate dirties the BG-shadow, which
     * would stage a 2KB-of-zeros DMA at VRAM 0 -- right on top of
     * our just-uploaded Mode-7 interleaved CHR+tilemap. Mode 7's
     * shared CHR/tilemap layout is incompatible with the per-BG-
     * layer tilemap-clear pattern that the indexed-tile BG modes
     * use. We rely on the kernel's boot-time VRAM clear instead. */
    mg_bg_mode(MG_BG_MODE_7);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);
    mg_mode7_wrap(MG_MODE7_FILL_BLACK);

    MG_OR_PANIC(mg_chr_upload(0x0000, s_chr_interleaved,
                              sizeof s_chr_interleaved));

    /* Palette: sky / checker-light / checker-dark. The backdrop
     * (CGRAM color 0) is what shows above the horizon. */
    mg_palette_set_rgb(0,  40,  80, 200);   /* sky blue (backdrop)     */
    mg_palette_set_rgb(1, 224, 224, 224);   /* light checker            */
    mg_palette_set_rgb(2,  64,  64,  64);   /* dark  checker            */

    /* Reserve HDMA channels 1..4 for the M7 matrix writes. The
     * kernel uses channel 0 for the DMA list dispatch and channel 7
     * for the INIDISP letterbox, so 1..6 are free for games. */
    MgHdmaCfg cfg_a; cfg_a.channel = 1; cfg_a.dest = MG_HDMA_DEST_M7A;
                     cfg_a.xfer = MG_HDMA_XFER_2B_1R; cfg_a.indirect = false;
    MgHdmaCfg cfg_b; cfg_b.channel = 2; cfg_b.dest = MG_HDMA_DEST_M7B;
                     cfg_b.xfer = MG_HDMA_XFER_2B_1R; cfg_b.indirect = false;
    MgHdmaCfg cfg_c; cfg_c.channel = 3; cfg_c.dest = MG_HDMA_DEST_M7C;
                     cfg_c.xfer = MG_HDMA_XFER_2B_1R; cfg_c.indirect = false;
    MgHdmaCfg cfg_d; cfg_d.channel = 4; cfg_d.dest = MG_HDMA_DEST_M7D;
                     cfg_d.xfer = MG_HDMA_XFER_2B_1R; cfg_d.indirect = false;
    mg_hdma_setup(&cfg_a);
    mg_hdma_setup(&cfg_b);
    mg_hdma_setup(&cfg_c);
    mg_hdma_setup(&cfg_d);
    mg_hdma_enable(1, true);
    mg_hdma_enable(2, true);
    mg_hdma_enable(3, true);
    mg_hdma_enable(4, true);

    /* Camera starts mid-plane, facing +X, eye-height tuned so the
     * checker tiles a few pixels deep look reasonable. */
    MgMode7Camera3D cam;
    cam.base.x    = Q16(512);
    cam.base.y    = Q16(512);
    cam.base.zoom = Q16_ONE;
    cam.base.yaw  = 0;
    cam.height    = Q16(64);
    cam.horizon_row = 96;

    const q16_16_t YAW_STEP    = Q16_FROM_DEG(5);
    const q16_16_t MOVE_STEP   = Q16(2);
    const q16_16_t HEIGHT_STEP = Q16(4);
    const q16_16_t HEIGHT_MIN  = Q16(8);
    const q16_16_t HEIGHT_MAX  = Q16(512);

    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) sys_exit(0);

        if (mg_pad_held(pads.p0, MG_BTN_L)) cam.base.yaw -= YAW_STEP;
        if (mg_pad_held(pads.p0, MG_BTN_R)) cam.base.yaw += YAW_STEP;

        q16_16_t s, c;
        q16_sincos(cam.base.yaw, &s, &c);

        /* Forward / strafe basis aligned with mg_mode7_camera3d's
         * perspective convention: at yaw=0 the camera looks down the
         * -y plane axis (top-of-screen = -y direction = "far away
         * forward"), with +x as the camera's right-hand strafe axis.
         *
         *   forward = ( sin(yaw), -cos(yaw))
         *   right   = ( cos(yaw),  sin(yaw))
         *
         * Earlier the demo used forward = (cos, sin), which is
         * 90° off from where the matrix actually points -- pressing
         * UP scrolled the world sideways instead of into the horizon.
         */
        if (mg_pad_held(pads.p0, MG_BTN_UP)) {
            cam.base.x += q16_mul(MOVE_STEP,  s);
            cam.base.y -= q16_mul(MOVE_STEP,  c);
        }
        if (mg_pad_held(pads.p0, MG_BTN_DOWN)) {
            cam.base.x -= q16_mul(MOVE_STEP,  s);
            cam.base.y += q16_mul(MOVE_STEP,  c);
        }
        if (mg_pad_held(pads.p0, MG_BTN_LEFT)) {
            cam.base.x -= q16_mul(MOVE_STEP,  c);
            cam.base.y -= q16_mul(MOVE_STEP,  s);
        }
        if (mg_pad_held(pads.p0, MG_BTN_RIGHT)) {
            cam.base.x += q16_mul(MOVE_STEP,  c);
            cam.base.y += q16_mul(MOVE_STEP,  s);
        }

        if (mg_pad_held(pads.p0, MG_BTN_A)) {
            cam.height += HEIGHT_STEP;
            if (cam.height > HEIGHT_MAX) cam.height = HEIGHT_MAX;
        }
        if (mg_pad_held(pads.p0, MG_BTN_B)) {
            cam.height -= HEIGHT_STEP;
            if (cam.height < HEIGHT_MIN) cam.height = HEIGHT_MIN;
        }

        if (mg_pad_pressed(pads.p0, MG_BTN_SELECT)) {
            cam.base.x = Q16(512); cam.base.y = Q16(512);
            cam.base.zoom = Q16_ONE; cam.base.yaw = 0;
            cam.height = Q16(64); cam.horizon_row = 96;
        }

        /* Build the four HDMA tables + the static matrix part. */
        MgMode7Params p;
        uint16_t bytes = mg_mode7_camera3d(&cam,
                                           s_tab_m7a, s_tab_m7b,
                                           s_tab_m7c, s_tab_m7d, &p);

        /* Upload them. Each call queues one DMA slot + `bytes` of
         * cart-window payload; four of those use 4/8 slots and
         * ~1 KB of the ~6.5 KB per-frame byte budget. */
        MG_OR_PANIC(mg_hdma_upload_table(1, s_tab_m7a, bytes));
        MG_OR_PANIC(mg_hdma_upload_table(2, s_tab_m7b, bytes));
        MG_OR_PANIC(mg_hdma_upload_table(3, s_tab_m7c, bytes));
        MG_OR_PANIC(mg_hdma_upload_table(4, s_tab_m7d, bytes));

        mg_mode7_set(&p);

        mg_frame_commit();
        mg_wait_frame();
    }
}
