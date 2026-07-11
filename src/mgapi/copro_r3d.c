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
static int         s_band;          /* rolling delivery: 0..NBANDS-1 */
static bool        s_tilemap_done;  /* fixed tilemap staged once (static thereafter) */

/* render scratch */
static uint8_t  s_fbuf[VW * VH];
static uint8_t  s_chr [NTILES][32];   /* BG1 4bpp base CHR */
static uint8_t  s_chr2[NTILES][16];   /* BG3 2bpp sub  CHR */
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

/* 60-colour palette: CGRAM 0-15 = 4bpp base hues, 16-19 = 2bpp sub levels. */
static void build_palette(uint16_t *cg) {
    for (int i = 0; i < 16; i++) cg[i]      = hc_base[i];
    for (int i = 0; i < 4;  i++) cg[16 + i] = hc_sub[i];
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
    s_band = 0;
    s_tilemap_done = false;

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
    uint8_t *map1 = mg_state()->bg[0].shadow;
    uint8_t *map3 = mg_state()->bg[2].shadow;
    for (int i = 0; i < 1024; i++) {
        map1[i*2 + 0] = (uint8_t)(BLANK_TILE & 0xFF);
        map1[i*2 + 1] = (uint8_t)(BLANK_TILE >> 8);
        map3[i*2 + 0] = (uint8_t)(BLANK_TILE & 0xFF);
        map3[i*2 + 1] = (uint8_t)(BLANK_TILE >> 8);
    }
    for (int t = 0; t < NTILES; t++) {
        int r = t / TW, c = t % TW;
        int cell = (r + 1) * 32 + (c + 1);
        map1[cell*2 + 0] = (uint8_t)(t & 0xFF);
        map1[cell*2 + 1] = (uint8_t)((t >> 8) & 0xFF);
        map3[cell*2 + 0] = (uint8_t)(t & 0xFF);
        map3[cell*2 + 1] = (uint8_t)(((t >> 8) & 0xFF) | BG3_PAL_HI);
    }
    mg_state_dirty_bg(0, 0, MG_BG_TILEMAP_BYTES);
    mg_state_dirty_bg(2, 0, MG_BG_TILEMAP_BYTES);
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

int copro_r3d_render(void) {
    if (!s_inited) copro_r3d_init();

    static int s_trace = -1;
    if (s_trace < 0) {
        const char *e = getenv("MG_DMA_TRACE");
        s_trace = (e && *e && *e != '0') ? 1 : 0;
    }

    /* --- band 0: start a new displayed frame (render, encode) ---
     * No commit-ahead gate: the guest paces one band per mg_wait_frame, so the
     * pipeline is naturally serialized (depth 1). A gate that returns -1 here
     * would DEADLOCK — demo_cube3d calls mg_wait_frame() unconditionally, so a
     * dropped (unstaged) render leaves it waiting on a frame that never comes. */
    if (s_band == 0) {
        build_scene();
        r3d_render_dither(&s_scene, s_fbuf, VW, VH);
        if (s_need_clear) {
            mg_state_arm_clean_slate_vram_clear();
            s_need_clear = false;
        }
        encode_dual();                              /* whole frame -> s_chr/s_chr2 */
    }

    MgState *s = mg_state();

    /* --- this band's CHR slice: tiles [t0, t1) --- */
    int t0 = s_band_t0[s_band], t1 = s_band_t0[s_band + 1];
    for (int c = t0; c < t1; c += CHUNK_TILES) {
        int n = (t1 - c < CHUNK_TILES) ? (t1 - c) : CHUNK_TILES;
        mg_state_queue_dma_transient(&s_chr[c][0], (uint32_t)n * 32u,
                                     0x18, 0x01, (uint16_t)(CHR_W + c * 16));
    }
    for (int c = t0; c < t1; c += CHUNK_TILES) {
        int n = (t1 - c < CHUNK_TILES) ? (t1 - c) : CHUNK_TILES;
        mg_state_queue_dma_transient(&s_chr2[c][0], (uint32_t)n * 16u,
                                     0x18, 0x01, (uint16_t)(CHR3_W + c * 8));
    }

    /* --- band 0 also carries palette + layer/colour-math setup, and the
     * fixed tilemap the first time (static thereafter). --- */
    if (s_band == 0) {
        build_palette(s->cgram_shadow);
        mg_state_dirty_cgram(0, 40);                /* 20 entries * 2 B */
        s->bgmode = 1;
        s->bg[0].tilemap_word = TMAP_W;
        s->bg[0].chr_word     = CHR_W;
        s->bg[0].size_code    = 0;
        s->bg[0].enabled_main = true;
        s->bg[2].tilemap_word = TMAP3_W;
        s->bg[2].chr_word     = CHR3_W;
        s->bg[2].size_code    = 0;
        s->bg[2].enabled_sub  = true;
        mg_state_set_color_math(true);              /* CGWSEL=$02 / CGADSUB=$41 */
        if (!s_tilemap_done) {
            fill_tilemaps();                        /* fixed grid -> bg shadow */
            s_tilemap_done = true;
        }
    }

    if (s_trace)
        fprintf(stderr, "[r3d] band %d: tiles [%d,%d) chr~%uB%s\n",
                s_band, t0, t1, (unsigned)((t1 - t0) * (32u + 16u)),
                (s_band == 0 && !s_tilemap_done) ? " +tilemap" : "");

    mg_state_build_frame();
    cart_window_set_frame_ready(1);
    s_band = (s_band + 1) % NBANDS;
    return 0;
}
