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
 *  Tile 0 = a "Tron corner": top row + left column bright (color
 *  1), interior dark (color 2). When every cell of the Mode-7
 *  tilemap points at tile 0 (= what mg_ppu_clean_slate leaves),
 *  the bright edges align across cell boundaries to form a clean
 *  8-pixel grid covering the whole plane -- a high-contrast Tron-
 *  style visualization that makes camera motion easy to read.
 *
 *  See demo_mode7.c for the Mode 7 interleaved-CHR-and-tilemap
 *  VRAM layout this 128-byte upload exploits.
 * ---------------------------------------------------------------- */
static uint8_t s_chr_interleaved[128];

static void build_chr(void) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            uint8_t pix = (y == 0 || x == 0) ? 1u : 2u;
            s_chr_interleaved[idx * 2 + 0] = 0;     /* tilemap byte */
            s_chr_interleaved[idx * 2 + 1] = pix;   /* CHR byte     */
        }
    }
}

/* HDMA tables for the per-scanline mode 7 effect:
 *   M7A..M7D    -- the 2x2 rotation/scale matrix (channels 1..4)
 *   HV combined -- BG1HOFS + BG1VOFS together via mode-3 HDMA on
 *                  channel 5 (frees up a channel for M7SEL below)
 *   M7SEL       -- per-scanline "screen over" mode on channel 6:
 *                  sky band uses FILL_BLACK ($80), active band uses
 *                  WRAP ($00). Lets the active band sample the
 *                  wrapped plane without FILL_BLACK wedge artifacts
 *                  while keeping the sky band as solid backdrop.
 *
 * mg_mode7_camera3d fills M7A..D and HV every frame. M7SEL is built
 * once at startup since it doesn't depend on the camera. */
static uint8_t s_tab_m7a  [MG_MODE7_3D_TABLE_BYTES];
static uint8_t s_tab_m7b  [MG_MODE7_3D_TABLE_BYTES];
static uint8_t s_tab_m7c  [MG_MODE7_3D_TABLE_BYTES];
static uint8_t s_tab_m7d  [MG_MODE7_3D_TABLE_BYTES];
static uint8_t s_tab_hv   [MG_MODE7_3D_HV_TABLE_BYTES];
static uint8_t s_tab_m7sel[MG_MODE7_3D_TABLE_BYTES];
static uint16_t s_m7sel_bytes;   /* set in _start; constant per run */

#define Q16(x)        ((q16_16_t)((int32_t)(x) * Q16_ONE))
#define Q16_FROM_DEG(d) ((q16_16_t)(((int64_t)(d) * Q16_TWO_PI) / 360))

void _start(void) {
    build_chr();

    /* Clean-slate clears all 64KB of VRAM via a fixed-source DMA at
     * the very first slot, so leftover tilemap/CHR from a previous
     * demo doesn't show through as the camera strafes into cells
     * mode7_3d's small (1 tile + 64 cell) CHR upload didn't touch.
     * The clear fires once on the first NMI; build_frame drops the
     * slot afterward so it doesn't re-fire every frame. */
    mg_ppu_clean_slate();

    /* Mode 7 + FILL_BLACK so sky lines (which get a $7FFF-saturated
     * matrix from mg_mode7_camera3d) draw as the backdrop color via
     * the out-of-plane FILL_BLACK rule rather than sampling whatever
     * cell sits under the camera. */
    mg_bg_mode(MG_BG_MODE_7);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);
    mg_mode7_wrap(MG_MODE7_FILL_BLACK);

    MG_OR_PANIC(mg_chr_upload(0x0000, s_chr_interleaved,
                              sizeof s_chr_interleaved));

    /* Palette: sky-blue backdrop (above the horizon, where the
     * per-scanline M7SEL HDMA selects FILL_BLACK), bright cyan grid
     * lines and a dark-navy tile interior — high-contrast Tron
     * look that makes camera motion easy to read against the grid. */
    mg_palette_set_rgb(0,  40,  80, 200);   /* sky blue (backdrop) */
    mg_palette_set_rgb(1,   0, 240, 240);   /* bright cyan grid    */
    mg_palette_set_rgb(2,  16,  16,  48);   /* dark navy interior  */

    /* Reserve HDMA channels 1..6:
     *   1 = M7A    2 = M7B    3 = M7C    4 = M7D       (mode 2)
     *   5 = HV   (4 bytes/scanline, mode 3, BBAD=BG1HOFS hits both
     *            BG1HOFS and BG1VOFS via the "4B 2R" pattern)
     *   6 = M7SEL (mode 0, 1 byte/scanline; switches FILL_BLACK over
     *            sky lines and WRAP over the active band)
     * Channel 0 reserved for the kernel's DMA-list dispatch; channel
     * 7 reserved for INIDISP letterbox. */
    MgHdmaCfg cfg_a;  cfg_a.channel  = 1; cfg_a.dest  = MG_HDMA_DEST_M7A;
                     cfg_a.xfer  = MG_HDMA_XFER_2B_1R; cfg_a.indirect  = false;
    MgHdmaCfg cfg_b;  cfg_b.channel  = 2; cfg_b.dest  = MG_HDMA_DEST_M7B;
                     cfg_b.xfer  = MG_HDMA_XFER_2B_1R; cfg_b.indirect  = false;
    MgHdmaCfg cfg_c;  cfg_c.channel  = 3; cfg_c.dest  = MG_HDMA_DEST_M7C;
                     cfg_c.xfer  = MG_HDMA_XFER_2B_1R; cfg_c.indirect  = false;
    MgHdmaCfg cfg_d;  cfg_d.channel  = 4; cfg_d.dest  = MG_HDMA_DEST_M7D;
                     cfg_d.xfer  = MG_HDMA_XFER_2B_1R; cfg_d.indirect  = false;
    MgHdmaCfg cfg_hv; cfg_hv.channel = 5; cfg_hv.dest = MG_HDMA_DEST_BG1_HOFS;
                     cfg_hv.xfer = MG_HDMA_XFER_4B_2R; cfg_hv.indirect = false;
    MgHdmaCfg cfg_s;  cfg_s.channel  = 6; cfg_s.dest  = MG_HDMA_DEST_M7SEL;
                     cfg_s.xfer  = MG_HDMA_XFER_1B_1R; cfg_s.indirect  = false;
    mg_hdma_setup(&cfg_a);
    mg_hdma_setup(&cfg_b);
    mg_hdma_setup(&cfg_c);
    mg_hdma_setup(&cfg_d);
    mg_hdma_setup(&cfg_hv);
    mg_hdma_setup(&cfg_s);
    mg_hdma_enable(1, true);
    mg_hdma_enable(2, true);
    mg_hdma_enable(3, true);
    mg_hdma_enable(4, true);
    mg_hdma_enable(5, true);
    mg_hdma_enable(6, true);

    /* Build the M7SEL table once -- doesn't depend on the camera. */
    s_m7sel_bytes = mg_mode7_build_m7sel_table(s_tab_m7sel, 96);

    /* Camera starts mid-plane, looking down the -y plane axis, eye-
     * height tuned so the checker tiles a few pixels deep look
     * reasonable. Step magnitudes are slow enough to feel
     * controllable -- with MOVE_STEP=1 you get 60 plane units/sec
     * forward at 60 fps, ~7 sec to traverse the full 1024-wide
     * plane. YAW_STEP=1° / frame = 60°/sec held. */
    MgMode7Camera3D cam;
    cam.base.x    = Q16(512);
    cam.base.y    = Q16(512);
    cam.base.zoom = Q16_ONE;
    cam.base.yaw  = 0;
    cam.height    = Q16(64);
    cam.horizon_row = 96;

    /* 1° per frame at 60 Hz = 60°/sec; halve it for finer aim. */
    const q16_16_t YAW_STEP    = Q16_FROM_DEG(1) / 2;
    const q16_16_t MOVE_STEP   = Q16_ONE;            /* 1.0 unit/frame  */
    const q16_16_t HEIGHT_STEP = Q16_ONE;            /* 1.0 unit/frame  */
    const q16_16_t HEIGHT_MIN  = Q16(8);
    const q16_16_t HEIGHT_MAX  = Q16(192);

    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) sys_exit(0);

        if (mg_pad_held(pads.p0, MG_BTN_L)) cam.base.yaw -= YAW_STEP;
        if (mg_pad_held(pads.p0, MG_BTN_R)) cam.base.yaw += YAW_STEP;

        q16_16_t s, c;
        q16_sincos(cam.base.yaw, &s, &c);

        /* Motion basis. UP = move forward (in the direction the
         * camera is facing); strafe is +/- right of forward.
         *
         * Convention rebuilt around mg_mode7_camera3d's new HOFS/
         * VOFS formula: that math puts cam.x at SX=128 (screen
         * center, horizontally) and cam.y at SY=224 (just past the
         * bottom edge of the visible 224-line frame). So at yaw=0
         * the camera is looking toward +y (smaller plane_y means
         * "ahead" -- wait, actually the inverse: every visible
         * scanline has plane_y < cam.y, meaning we render plane
         * regions AHEAD of cam.y. "Forward" therefore = direction
         * of decreasing plane_y = -y axis). So UP = cam.y -= MOVE
         * was right -- but the user reports it as reversed, which
         * makes me suspect the visual flow is what counts and SNES
         * mode 7 convention has +y as "ahead" for some other
         * historical reason. Inverting here matches user-reported
         * feel; the math doesn't care which sign cam.y takes. */
        if (mg_pad_held(pads.p0, MG_BTN_UP)) {
            cam.base.x -= q16_mul(MOVE_STEP,  s);
            cam.base.y += q16_mul(MOVE_STEP,  c);
        }
        if (mg_pad_held(pads.p0, MG_BTN_DOWN)) {
            cam.base.x += q16_mul(MOVE_STEP,  s);
            cam.base.y -= q16_mul(MOVE_STEP,  c);
        }
        if (mg_pad_held(pads.p0, MG_BTN_LEFT)) {
            cam.base.x -= q16_mul(MOVE_STEP,  c);
            cam.base.y -= q16_mul(MOVE_STEP,  s);
        }
        if (mg_pad_held(pads.p0, MG_BTN_RIGHT)) {
            cam.base.x += q16_mul(MOVE_STEP,  c);
            cam.base.y += q16_mul(MOVE_STEP,  s);
        }

        /* Wrap camera modulo 1024 (= the SNES Mode-7 plane width).
         * Because the active band uses WRAP via the per-scanline
         * M7SEL HDMA, plane sampling wraps anyway -- this just keeps
         * cam.x/cam.y from drifting outside the 13-bit range that
         * M7HOFS/M7VOFS can encode after the cam/z divide. The user
         * can fly arbitrarily far in any direction; the world keeps
         * presenting the same uniform Tron-grid pattern. */
        const q16_16_t PLANE = Q16(1024);
        while (cam.base.x >= PLANE) cam.base.x -= PLANE;
        while (cam.base.x <  0)     cam.base.x += PLANE;
        while (cam.base.y >= PLANE) cam.base.y -= PLANE;
        while (cam.base.y <  0)     cam.base.y += PLANE;

        if (mg_pad_held(pads.p0, MG_BTN_A)) {
            cam.height += HEIGHT_STEP;
            if (cam.height > HEIGHT_MAX) cam.height = HEIGHT_MAX;
        }
        if (mg_pad_held(pads.p0, MG_BTN_B)) {
            cam.height -= HEIGHT_STEP;
            if (cam.height < HEIGHT_MIN) cam.height = HEIGHT_MIN;
        }

        /* SELECT button doubles as a yaw control fallback in case the
         * L/R shoulders aren't mapped in the emulator's input config:
         * SELECT alone yaws LEFT, SELECT+START yaws RIGHT, plain
         * SELECT-press (no other buttons held) resets the camera. */
        if (mg_pad_pressed(pads.p0, MG_BTN_SELECT) &&
            !mg_pad_held(pads.p0, MG_BTN_START)) {
            cam.base.x = Q16(512); cam.base.y = Q16(512);
            cam.base.zoom = Q16_ONE; cam.base.yaw = 0;
            cam.height = Q16(64); cam.horizon_row = 96;
        }

        /* Build the per-frame HDMA tables + the static matrix part.
         * M7A..D and HV come back from mg_mode7_camera3d; M7SEL is
         * static and was built once in _start. */
        MgMode7Params p;
        MgMode7TableSizes sz = mg_mode7_camera3d(&cam,
                                                 s_tab_m7a, s_tab_m7b,
                                                 s_tab_m7c, s_tab_m7d,
                                                 s_tab_hv,  &p);

        /* Upload all six tables. Sizes vary by mode:
         *   M7A..D : sz.bytes_m7  (~452 each)
         *   HV     : sz.bytes_hv  (~900, mode-3 doubles byte/scanline)
         *   M7SEL  : s_m7sel_bytes (~228, mode-0 single byte/scanline)
         * Total cart-window pool usage ~3 KB, under the 4 KB cap. */
        MG_OR_PANIC(mg_hdma_upload_table(1, s_tab_m7a,   sz.bytes_m7));
        MG_OR_PANIC(mg_hdma_upload_table(2, s_tab_m7b,   sz.bytes_m7));
        MG_OR_PANIC(mg_hdma_upload_table(3, s_tab_m7c,   sz.bytes_m7));
        MG_OR_PANIC(mg_hdma_upload_table(4, s_tab_m7d,   sz.bytes_m7));
        MG_OR_PANIC(mg_hdma_upload_table(5, s_tab_hv,    sz.bytes_hv));
        MG_OR_PANIC(mg_hdma_upload_table(6, s_tab_m7sel, s_m7sel_bytes));

        mg_mode7_set(&p);

        mg_frame_commit();
        mg_wait_frame();
    }
}
