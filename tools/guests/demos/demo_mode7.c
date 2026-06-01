/* ============================================================
 *  demo_mode7.c — fly the Mode 7 camera over a checkerboard plane.
 *
 *  Smallest end-to-end exercise of mg_mode7_camera + the matrix
 *  staging path that landed last week. The plane is a 128×128
 *  checkerboard built from a single 8bpp tile (every other pixel
 *  alternates color 1 / color 2); the tilemap stays at all-zero
 *  (every cell references tile 0), so the whole plane shows the
 *  one tile repeated. Mode 7 wraps by default, so the world feels
 *  infinite as you fly around.
 *
 *  Controls (D-pad faces +X at yaw=0):
 *      L / R       rotate yaw CCW / CW
 *      UP / DOWN   move forward / back along the yaw heading
 *      LEFT/RIGHT  strafe (perpendicular to yaw)
 *      A / B       zoom in / out (1.5x / 0.66x per press)
 *      SELECT      reset the camera to (512,512), zoom 1.0, yaw 0
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
 *  CHR — a single 8bpp tile encoding a 2×2-pixel checkerboard. The
 *  tile is 8×8 pixels; we alternate colors so each 2×2 block reads
 *  as a clean dark/light pair. Tilemap entries are all zero, so this
 *  one tile fills the entire 128×128 plane.
 * ---------------------------------------------------------------- */

/* Build the interleaved (low=tilemap_byte, high=CHR_byte) VRAM
 * upload for Mode 7. The first 64 words carry tile 0's 64 CHR
 * bytes (high bytes) plus tilemap entries 0..63 (low bytes, all 0).
 * Remaining tilemap entries (64..16383) are zero from the boot
 * VRAM clear the kernel does, so we don't need to touch them. */
static uint8_t s_chr_interleaved[128];   /* 64 words × 2 bytes */

static void build_chr(void) {
    /* 8×8 8bpp tile: pixel = ((y >> 1) ^ (x >> 1)) & 1 ? color 1 : color 2.
     * The shift gives 2×2 blocks; XOR makes the checkerboard pattern. */
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int idx = y * 8 + x;
            uint8_t pix = (((y >> 1) ^ (x >> 1)) & 1) ? 1 : 2;
            s_chr_interleaved[idx * 2 + 0] = 0;     /* tilemap low byte */
            s_chr_interleaved[idx * 2 + 1] = pix;   /* CHR high byte    */
        }
    }
}

/* Q16.16 helpers (sin/cos already give Q16.16 results). */
#define Q16(x)        ((q16_16_t)((int32_t)(x) * Q16_ONE))
#define Q16_FROM_DEG(d) ((q16_16_t)(((int64_t)(d) * Q16_TWO_PI) / 360))

void _start(void) {
    build_chr();

    /* Mode 7: BG1 is the plane; BG2..4 don't exist in this mode. */
    mg_bg_mode(MG_BG_MODE_7);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);
    mg_mode7_wrap(MG_MODE7_WRAP);

    /* Upload the interleaved CHR+tilemap chunk at VRAM word 0. */
    MG_OR_PANIC(mg_chr_upload(0x0000, s_chr_interleaved,
                              sizeof s_chr_interleaved));

    /* Palette: backdrop, then the two checkerboard colors. Color 0 of
     * a Mode 7 plane is treated as transparent on top of the backdrop;
     * we set it to a sky-ish tone in case the wrap mode ever clips. */
    mg_palette_set_rgb(0,  16,  16,  48);   /* backdrop / clipped     */
    mg_palette_set_rgb(1, 224, 224, 224);   /* light checker          */
    mg_palette_set_rgb(2,  32,  64, 128);   /* dark  checker (blue)   */

    /* Camera starts at the middle of the plane, zoom 1.0, facing +X.
     * Field-by-field rather than a struct-literal init so GCC stores
     * the values directly without going through a memcpy-from-rodata
     * helper (which would force us to bring in a libc memcpy stub). */
    MgMode7Camera cam;
    cam.x    = Q16(512);
    cam.y    = Q16(512);
    cam.zoom = Q16_ONE;
    cam.yaw  = 0;

    /* Per-press deltas. yaw_step = 5°/frame held; move_step = 2 px;
     * zoom multiplier = 1.5x in / 0.66x out per press. */
    const q16_16_t YAW_STEP   = Q16_FROM_DEG(5);
    const q16_16_t MOVE_STEP  = Q16(2);
    const q16_16_t ZOOM_IN_K  = (q16_16_t)((int64_t)Q16_ONE * 2 / 3);   /* 0.666… */
    const q16_16_t ZOOM_OUT_K = (q16_16_t)((int64_t)Q16_ONE * 3 / 2);   /* 1.5    */
    const q16_16_t ZOOM_MIN   = Q16_ONE / 16;
    const q16_16_t ZOOM_MAX   = Q16_ONE * 16;

    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) sys_exit(0);

        if (mg_pad_held(pads.p0, MG_BTN_L)) cam.yaw -= YAW_STEP;
        if (mg_pad_held(pads.p0, MG_BTN_R)) cam.yaw += YAW_STEP;

        /* Forward / strafe vectors derived from the current yaw. */
        q16_16_t s, c;
        q16_sincos(cam.yaw, &s, &c);

        if (mg_pad_held(pads.p0, MG_BTN_UP)) {
            cam.x += q16_mul(MOVE_STEP,  c);
            cam.y += q16_mul(MOVE_STEP,  s);
        }
        if (mg_pad_held(pads.p0, MG_BTN_DOWN)) {
            cam.x -= q16_mul(MOVE_STEP,  c);
            cam.y -= q16_mul(MOVE_STEP,  s);
        }
        if (mg_pad_held(pads.p0, MG_BTN_LEFT)) {
            cam.x += q16_mul(MOVE_STEP,  s);
            cam.y -= q16_mul(MOVE_STEP,  c);
        }
        if (mg_pad_held(pads.p0, MG_BTN_RIGHT)) {
            cam.x -= q16_mul(MOVE_STEP,  s);
            cam.y += q16_mul(MOVE_STEP,  c);
        }

        if (mg_pad_pressed(pads.p0, MG_BTN_A)) {
            cam.zoom = q16_mul(cam.zoom, ZOOM_IN_K);
            if (cam.zoom < ZOOM_MIN) cam.zoom = ZOOM_MIN;
        }
        if (mg_pad_pressed(pads.p0, MG_BTN_B)) {
            cam.zoom = q16_mul(cam.zoom, ZOOM_OUT_K);
            if (cam.zoom > ZOOM_MAX) cam.zoom = ZOOM_MAX;
        }

        if (mg_pad_pressed(pads.p0, MG_BTN_SELECT)) {
            cam.x = Q16(512); cam.y = Q16(512);
            cam.zoom = Q16_ONE; cam.yaw = 0;
        }

        MgMode7Params p;
        mg_mode7_camera(&cam, &p);
        mg_mode7_set(&p);

        mg_frame_commit();
        mg_wait_frame();
    }
}
