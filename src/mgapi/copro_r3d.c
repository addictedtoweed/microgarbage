/* ============================================================
 *  copro_r3d.c — native coprocessor-firmware 3D renderer service.
 *  See copro_r3d.h and docs/3d-renderer.md.
 *
 *  M1: transform + rasterize (r3d) a small scene into a 240x208 8bpp
 *  framebuffer, swizzle to SNES 4bpp tiles, and stage one frame through
 *  the existing mg_state cart-window transport (same path the guest's
 *  mg_frame_commit uses). Single BG1 4bpp layer, ~16 colours. The 2bpp
 *  BG3 "high colour" sub layer (the 60-colour LUT split) is M2.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "copro_r3d.h"

#include "copro_mg_state.h"
#include "cart_window.h"

#include "video/r3d.h"
#include "video/hicolor.h"
#include "math/mat_q16.h"
#include "math/fixed_point.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- 240x200 dual-layer tiled target ---- */
#define VW 240
#define VH 200
#define TW (VW/8)            /* 30 tiles wide  */
#define TH (VH/8)            /* 25 tiles tall  */
#define NTILES (TW*TH)       /* 750            */
#define CHR_BYTES (NTILES*32)/* 24000          */

/* Rolling delivery: the full frame is fixed-layout (750 tiles) and updated in
 * place, one contiguous tile-range BAND per cart-window frame. 4 bands, byte-
 * balanced (~188 tiles = ~9 KB each = one sub-frame), so a full frame is 4
 * cart-window frames = 4 vblanks = the 15 fps floor. Bands are tile-index
 * ranges (row-major), not screen-row-aligned — the fixed tilemap shows each
 * tile once its CHR lands. */
#define NBANDS 4
static const int s_band_t0[NBANDS + 1] = { 0, 188, 375, 563, 750 };
/* delivery order: bottom-3 first, TOP band LAST (roll_test.s convention). */
static const int s_band_order[NBANDS] = { 1, 2, 3, 0 };

/* cart-window payload offsets for DIRECT band staging (bypasses mg_state's
 * subframe/commit-ahead/reupload machinery — see [[3d-renderer-design]]). */
#define R3D_OFF_BG1   0x0000u   /* band BG1 4bpp CHR (<= ~6 KB)   */
#define R3D_OFF_BG3   0x1800u   /* band BG3 2bpp CHR (<= ~3 KB)   */
#define R3D_OFF_TMAP1 0x3000u   /* BG1 tilemap  (2 KB, init only) */
#define R3D_OFF_TMAP3 0x3800u   /* BG3 tilemap  (2 KB, init only) */
#define R3D_OFF_PAL   0x4000u   /* palette (40 B, init only)      */
/* VRAM word layout (dual-layer). Tilemap bases are 0x400-word units (BGxSC),
 * CHR bases are 0x1000-word units (BGxxNBA). BG1 4bpp CHR (780*16w=12480w) fits
 * 0x2000..0x50C0; BG3 2bpp CHR (780*8w=6240w) at 0x6000..0x7860. */
#define TMAP_W  0x0000       /* BG1 tilemap  (SC base 0)  */
#define TMAP3_W 0x0400       /* BG3 tilemap  (SC base 1)  */
#define CHR_W   0x2000       /* BG1 4bpp CHR (NBA unit 2) */
#define CHR3_W  0x6000       /* BG3 2bpp CHR (NBA unit 6) */
#define BLANK_TILE NTILES    /* tile 780 = zeroed VRAM = transparent, for margins */
#define BG3_PAL_HI 0x10      /* tilemap word palette 4 (<<10) -> high byte bit 4 */

/* CHR chunking: one DMA slot must stay under MG_SUBFRAME_BYTE_BUDGET (9180).
 * 260 4bpp tiles * 32 B = 8320 B/chunk; 2bpp is 16 B/tile so chunks are half. */
#define CHUNK_TILES 260

/* ---- 60-colour dual-layer palette (see video/hicolor.h): fb value packs
 * (hue<<2 | brightness); BG1 base = hue (CGRAM 0-15), BG3 sub = brightness
 * (CGRAM 16-19), composited by half-add. Cube faces use 6 hues. ---- */
#define HUE(h) ((uint8_t)((h) << 2))

#define R3D_MAX_OBJECTS 8
#define CAM_Z_DEFAULT q16_from_double(6.0)

typedef struct {
    bool    used, visible;
    uint8_t mesh_id;
    int32_t px, py, pz;        /* Q16.16 world position */
    int32_t rx, ry, rz;        /* Q16.16 Euler radians  */
} R3dInstance;

typedef struct {
    int32_t ex, ey, ez;
    int32_t yaw, pitch, roll;
    int32_t focal;
} R3dCam;

static R3dInstance s_objs[R3D_MAX_OBJECTS];
static R3dCam      s_cam;
static bool        s_need_clear;
static bool        s_inited;
static int         s_deliver;       /* rolling delivery index 0..NBANDS-1 */
static bool        s_init_done;     /* tilemap + palette staged into VRAM once */

/* render scratch */
static uint8_t  s_fbuf[VW * VH];
static uint8_t  s_chr [NTILES][32];   /* BG1 4bpp base CHR (whole frame) */
static uint8_t  s_chr2[NTILES][16];   /* BG3 2bpp sub  CHR */
static uint8_t  s_tmap1[2048];        /* BG1 tilemap (host, fixed) */
static uint8_t  s_tmap3[2048];        /* BG3 tilemap */
static uint16_t s_pal[20];            /* CGRAM 0-19 (16 base + 4 sub) */
static R3dScene s_scene;
static R3dObject s_robjs[R3D_MAX_OBJECTS];

/* ---- built-in cube mesh (unit cube, CCW-from-outside winding) ---- */
static vec3_q16 s_cube_v[8];
static const int s_cube_vd[8][3] = {
    {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
    {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
};
static const uint16_t s_cube_t[36] = {
    4,5,6,  4,6,7,    /* +Z */
    0,3,2,  0,2,1,    /* -Z */
    1,6,5,  1,2,6,    /* +X */
    0,4,7,  0,7,3,    /* -X */
    3,7,6,  3,6,2,    /* +Y */
    0,1,5,  0,5,4,    /* -Y */
};
static const uint8_t s_cube_base[12] = {   /* 6 hues, one per face (hue<<2) */
    HUE(1),  HUE(1),   /* +Z red    */
    HUE(3),  HUE(3),   /* -Z yellow */
    HUE(5),  HUE(5),   /* +X green  */
    HUE(7),  HUE(7),   /* -X cyan   */
    HUE(9),  HUE(9),   /* +Y blue   */
    HUE(11), HUE(11),  /* -Y magenta*/
};
static R3dMesh s_mesh_cube;

/* 60-colour palette into the host buffer: CGRAM 0-15 = base hues, 16-19 = sub. */
static void build_palette(void) {
    for (int i = 0; i < 16; i++) s_pal[i]      = hc_base[i];
    for (int i = 0; i < 4;  i++) s_pal[16 + i] = hc_sub[i];
}

void copro_r3d_init(void) {
    if (s_inited) return;
    for (int i = 0; i < 8; i++)
        s_cube_v[i] = vec3_q16_make(q16_from_int(s_cube_vd[i][0]),
                                    q16_from_int(s_cube_vd[i][1]),
                                    q16_from_int(s_cube_vd[i][2]));
    s_mesh_cube.verts = s_cube_v; s_mesh_cube.nverts = 8;
    s_mesh_cube.tris  = s_cube_t; s_mesh_cube.ntris  = 12;
    s_mesh_cube.tri_base = s_cube_base;
    s_inited = true;
    copro_r3d_reset();
}

void copro_r3d_shutdown(void) { /* scratch is static; nothing to free */ }

void copro_r3d_reset(void) {
    memset(s_objs, 0, sizeof s_objs);
    s_cam.ex = 0; s_cam.ey = 0; s_cam.ez = 0;
    s_cam.yaw = 0; s_cam.pitch = 0; s_cam.roll = 0;
    s_cam.focal = q16_from_int(160);
    s_need_clear = true;
    s_deliver = 0;
    s_init_done = false;

    /* scene constants (mirrors the demo_cube preview) */
    s_scene.near_z  = q16_from_double(0.5);
    s_scene.light   = vec3_q16_normalize(vec3_q16_make(q16_from_double(-0.4),
                                                       q16_from_double(0.55),
                                                       q16_from_double(-0.73)));
    s_scene.ambient = q16_from_double(0.30);
    s_scene.diffuse = q16_from_double(0.70);
    s_scene.base    = 0;            /* per-face tri_base (hue<<2) overrides this */
    s_scene.ramp    = HC_RAMP;      /* 4 brightness levels -> fb = hue<<2 | bright */
}

void copro_r3d_set_camera(int32_t ex, int32_t ey, int32_t ez,
                          int32_t yaw, int32_t pitch, int32_t roll, int32_t focal) {
    s_cam.ex = ex; s_cam.ey = ey; s_cam.ez = ez;
    s_cam.yaw = yaw; s_cam.pitch = pitch; s_cam.roll = roll;
    if (focal != 0) s_cam.focal = focal;
}

int copro_r3d_add(uint8_t mesh_id) {
    for (int i = 0; i < R3D_MAX_OBJECTS; i++) {
        if (!s_objs[i].used) {
            s_objs[i].used = true;
            s_objs[i].visible = true;
            s_objs[i].mesh_id = mesh_id;
            s_objs[i].px = 0; s_objs[i].py = 0; s_objs[i].pz = CAM_Z_DEFAULT;
            s_objs[i].rx = 0; s_objs[i].ry = 0; s_objs[i].rz = 0;
            return i;
        }
    }
    return -1;
}

static bool obj_ok(int h) { return h >= 0 && h < R3D_MAX_OBJECTS && s_objs[h].used; }

void copro_r3d_object_move(int h, int32_t x, int32_t y, int32_t z) {
    if (obj_ok(h)) { s_objs[h].px = x; s_objs[h].py = y; s_objs[h].pz = z; }
}
void copro_r3d_object_rotate(int h, int32_t rx, int32_t ry, int32_t rz) {
    if (obj_ok(h)) { s_objs[h].rx = rx; s_objs[h].ry = ry; s_objs[h].rz = rz; }
}
void copro_r3d_object_show(int h, bool visible) {
    if (obj_ok(h)) s_objs[h].visible = visible;
}

/* build the R3dScene (view + object xforms) for the current state */
static void build_scene(void) {
    mat3_q16 crot = mat3_q16_mul(mat3_q16_rotation_z(s_cam.roll),
                    mat3_q16_mul(mat3_q16_rotation_y(s_cam.yaw),
                                 mat3_q16_rotation_x(s_cam.pitch)));
    vec3_q16 right = mat3_q16_mul_vec3(crot, vec3_q16_make(Q16_ONE, 0, 0));
    vec3_q16 up    = mat3_q16_mul_vec3(crot, vec3_q16_make(0, Q16_ONE, 0));
    vec3_q16 fwd   = mat3_q16_mul_vec3(crot, vec3_q16_make(0, 0, Q16_ONE));
    s_scene.view  = affine3_q16_view(vec3_q16_make(s_cam.ex, s_cam.ey, s_cam.ez),
                                     right, up, fwd);
    s_scene.focal = s_cam.focal;

    int n = 0;
    for (int i = 0; i < R3D_MAX_OBJECTS; i++) {
        if (!s_objs[i].used || !s_objs[i].visible) continue;
        mat3_q16 rot = mat3_q16_mul(mat3_q16_rotation_z(s_objs[i].rz),
                       mat3_q16_mul(mat3_q16_rotation_y(s_objs[i].ry),
                                    mat3_q16_rotation_x(s_objs[i].rx)));
        s_robjs[n].mesh  = &s_mesh_cube;          /* only the cube for now */
        s_robjs[n].xform = affine3_q16_compose(
            affine3_q16_from_translation(vec3_q16_make(s_objs[i].px, s_objs[i].py, s_objs[i].pz)),
            affine3_q16_from_rotation(rot));
        n++;
    }
    s_scene.objs  = s_robjs;
    s_scene.nobjs = n;
}

/* Fill both shadow tilemaps for the FIXED 750-tile grid: cell (r,c) -> tile
 * r*TW+c, centred with a 1-tile margin. This never changes frame to frame
 * (only the CHR content does), so it's staged once per displayed frame with
 * band 0. BG3 cells carry palette 4 (sub colours read CGRAM 16-19). */
static void fill_tilemaps(void) {
    for (int i = 0; i < 1024; i++) {
        s_tmap1[i*2 + 0] = (uint8_t)(BLANK_TILE & 0xFF);
        s_tmap1[i*2 + 1] = (uint8_t)(BLANK_TILE >> 8);
        s_tmap3[i*2 + 0] = (uint8_t)(BLANK_TILE & 0xFF);
        s_tmap3[i*2 + 1] = (uint8_t)(BLANK_TILE >> 8);
    }
    for (int t = 0; t < NTILES; t++) {
        int r = t / TW, c = t % TW;
        int cell = (r + 1) * 32 + (c + 1);
        s_tmap1[cell*2 + 0] = (uint8_t)(t & 0xFF);
        s_tmap1[cell*2 + 1] = (uint8_t)((t >> 8) & 0xFF);
        s_tmap3[cell*2 + 0] = (uint8_t)(t & 0xFF);
        s_tmap3[cell*2 + 1] = (uint8_t)(((t >> 8) & 0xFF) | BG3_PAL_HI);
    }
}

/* Dual-layer encode of the WHOLE frame: split s_fbuf (value = hue<<2 |
 * brightness) via the hicolor LUT into BG1 4bpp base CHR (s_chr) + BG3 2bpp
 * sub CHR (s_chr2), fixed tile order t = r*TW+c. Swizzle round-trip verified
 * in src/video/tests/hc_preview.c. The tilemap is fixed (fill_tilemaps). */
static void encode_dual(void) {
    memset(s_chr,  0, sizeof s_chr);
    memset(s_chr2, 0, sizeof s_chr2);
    for (int t = 0; t < NTILES; t++) {
        int tx = (t % TW) * 8, ty = (t / TW) * 8;
        for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) {
            uint8_t v = s_fbuf[(ty + yy) * VW + (tx + xx)];
            int b   = hc_base_of(v);
            int sub = hc_sub_of(v);
            int bit = 7 - xx;
            s_chr[t][yy*2 + 0]      |= (uint8_t)(((b   >> 0) & 1) << bit);
            s_chr[t][yy*2 + 1]      |= (uint8_t)(((b   >> 1) & 1) << bit);
            s_chr[t][16 + yy*2 + 0] |= (uint8_t)(((b   >> 2) & 1) << bit);
            s_chr[t][16 + yy*2 + 1] |= (uint8_t)(((b   >> 3) & 1) << bit);
            s_chr2[t][yy*2 + 0]     |= (uint8_t)(((sub >> 0) & 1) << bit);
            s_chr2[t][yy*2 + 1]     |= (uint8_t)(((sub >> 1) & 1) << bit);
        }
    }
}

static void set_slot(unsigned i, uint8_t bbus, uint8_t dmap,
                     uint16_t src, uint16_t size, uint16_t prep) {
    CartDmaSlot sl;
    sl.bbus = bbus; sl.dmap = dmap; sl.src = src; sl.size = size; sl.prep = prep;
    cart_window_set_dma_slot(i, &sl);
}

/* DIRECT staging: write one band straight into the cart window (band CHR +
 * DMA-list slots + frame_ready), bypassing mg_state_build_frame's subframe
 * packing / commit-ahead / bg-reupload — the kernel's frame_dma delivers it as
 * one clean frame. Static setup (batch, tilemap, palette, letterbox) staged
 * once. Bands delivered bottom-3-first, TOP-LAST; render at delivery 0. */
int copro_r3d_render(void) {
    if (!s_inited) copro_r3d_init();

    static int s_trace = -1;
    if (s_trace < 0) {
        const char *e = getenv("MG_DMA_TRACE");
        s_trace = (e && *e && *e != '0') ? 1 : 0;
    }

    int d    = s_deliver;
    int band = s_band_order[d];

    if (d == 0) {                                   /* new displayed frame */
        build_scene();
        r3d_render_dither(&s_scene, s_fbuf, VW, VH);
        encode_dual();                              /* whole frame -> s_chr/s_chr2 */
    }

    unsigned slot = 0;

    if (!s_init_done) {
        /* Letterbox: top 8 + bottom 16 force-blanked -> ~62-line DMA window
         * (a ~9 KB band fits one force-blank burst) AND hides the tilemap
         * margins. Visible region = lines 8..208 = our 200px frame. */
        cart_window_store_u16_le(CW_OFF_KERNEL_LAYOUT, (uint16_t)(8u | (16u << 8)));
        /* No HDMA in framebuffer mode: zero frame_dma's ch1-6 config. */
        static const uint8_t zeros[7 * 8] = {0};
        cart_window_load_blob(0x7968u /* COPRO_HDMA_CONFIG */, zeros, sizeof zeros);
        /* PPU batch: Mode 1, BG1 4bpp main + BG3 2bpp sub, half-add colour math. */
        PpuBatch b; memset(&b, 0, sizeof b);
        b.bgmode  = 1;
        b.bg1sc   = (uint8_t)((TMAP_W  / 0x400u) << 2);
        b.bg3sc   = (uint8_t)((TMAP3_W / 0x400u) << 2);
        b.bg12nba = (uint8_t)(CHR_W  / 0x1000u);
        b.bg34nba = (uint8_t)(CHR3_W / 0x1000u);
        b.tm = 0x01; b.ts = 0x04;                   /* BG1 main, BG3 sub */
        b.cgwsel = 0x02; b.cgadsub = 0x41;          /* colour math: BG1, half, add */
        cart_window_set_ppu_batch(&b);
        /* Static palette + tilemaps into the cart window + their DMA slots. */
        build_palette();
        fill_tilemaps();
        cart_window_load_blob(R3D_OFF_PAL,   s_pal,   sizeof s_pal);
        cart_window_load_blob(R3D_OFF_TMAP1, s_tmap1, sizeof s_tmap1);
        cart_window_load_blob(R3D_OFF_TMAP3, s_tmap3, sizeof s_tmap3);
        set_slot(slot++, 0x22, 0x00, R3D_OFF_PAL,   (uint16_t)sizeof s_pal, 0);    /* CGRAM */
        set_slot(slot++, 0x18, 0x01, R3D_OFF_TMAP1, 2048, TMAP_W);
        set_slot(slot++, 0x18, 0x01, R3D_OFF_TMAP3, 2048, TMAP3_W);
        s_init_done = true;
    }

    /* This band's CHR into the cart window + its two DMA slots. */
    int t0 = s_band_t0[band], t1 = s_band_t0[band + 1];
    uint16_t bg1_len = (uint16_t)((t1 - t0) * 32);
    uint16_t bg3_len = (uint16_t)((t1 - t0) * 16);
    cart_window_load_blob(R3D_OFF_BG1, &s_chr[t0][0],  bg1_len);
    cart_window_load_blob(R3D_OFF_BG3, &s_chr2[t0][0], bg3_len);
    set_slot(slot++, 0x18, 0x01, R3D_OFF_BG1, bg1_len, (uint16_t)(CHR_W  + t0 * 16));
    set_slot(slot++, 0x18, 0x01, R3D_OFF_BG3, bg3_len, (uint16_t)(CHR3_W + t0 * 8));
    set_slot(slot, 0, 0, 0, 0, 0);                  /* terminator */

    cart_window_set_frame_ready(1);

    if (s_trace)
        fprintf(stderr, "[r3d] deliver %d -> band %d tiles [%d,%d) bg1=%uB bg3=%uB\n",
                d, band, t0, t1, bg1_len, bg3_len);

    s_deliver = (d + 1) % NBANDS;
    return 0;
}
