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
#include "math/mat_q16.h"
#include "math/fixed_point.h"

#include <string.h>

/* ---- 4bpp tiled target (matches the FMV / canyon4 resolution) ---- */
#define VW 240
#define VH 208
#define TW (VW/8)            /* 30 tiles wide  */
#define TH (VH/8)            /* 26 tiles tall  */
#define NTILES (TW*TH)       /* 780            */
#define CHR_BYTES (NTILES*32)/* 24960          */
#define TMAP_W 0x0000        /* BG1 tilemap VRAM word base */
#define CHR_W  0x2000        /* BG1 CHR VRAM word base (char base unit 2) */
#define BLANK_TILE NTILES    /* tile 780 = zeroed VRAM = backdrop, for margins */

/* CHR chunking: one DMA slot must stay under MG_SUBFRAME_BYTE_BUDGET (9180).
 * 260 tiles * 32 B = 8320 B/chunk -> 3 chunks for the 780-tile frame. */
#define CHUNK_TILES 260

/* ---- palette: 3 hue ramps (red/green/blue), 5 shades each, idx 0 = backdrop. ---- */
#define RAMP4    5
#define RED_BASE   1
#define GREEN_BASE 6
#define BLUE_BASE  11

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

/* render scratch */
static uint8_t  s_fbuf[VW * VH];
static uint8_t  s_chr[NTILES][32];
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
static const uint8_t s_cube_base[12] = {
    RED_BASE,   RED_BASE,
    RED_BASE,   RED_BASE,
    GREEN_BASE, GREEN_BASE,
    GREEN_BASE, GREEN_BASE,
    BLUE_BASE,  BLUE_BASE,
    BLUE_BASE,  BLUE_BASE,
};
static R3dMesh s_mesh_cube;

#define BGR555(r,g,b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

static uint16_t lerp555(uint16_t a, uint16_t b, int num, int den) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    return BGR555(ar + (br - ar) * num / den, ag + (bg - ag) * num / den, ab + (bb - ab) * num / den);
}

static void build_palette(uint16_t *cg) {
    cg[0] = BGR555(2, 2, 4);
    for (int s = 0; s < RAMP4; s++) {
        cg[RED_BASE   + s] = lerp555(BGR555(7, 2, 2), BGR555(31, 12, 10), s, RAMP4 - 1);
        cg[GREEN_BASE + s] = lerp555(BGR555(2, 7, 2), BGR555(12, 31, 12), s, RAMP4 - 1);
        cg[BLUE_BASE  + s] = lerp555(BGR555(2, 3, 8), BGR555(12, 16, 31), s, RAMP4 - 1);
    }
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

    /* scene constants (mirrors the demo_cube preview) */
    s_scene.near_z  = q16_from_double(0.5);
    s_scene.light   = vec3_q16_normalize(vec3_q16_make(q16_from_double(-0.4),
                                                       q16_from_double(0.55),
                                                       q16_from_double(-0.73)));
    s_scene.ambient = q16_from_double(0.30);
    s_scene.diffuse = q16_from_double(0.70);
    s_scene.base    = RED_BASE;
    s_scene.ramp    = RAMP4;
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

/* swizzle s_fbuf (8bpp) -> s_chr planar 4bpp tiles + fill the BG1 shadow tilemap */
static void encode_4bpp(void) {
    uint8_t *map = mg_state()->bg[0].shadow;        /* 32x32 cells, 2 B each */
    for (int i = 0; i < 1024; i++) {
        map[i*2 + 0] = (uint8_t)(BLANK_TILE & 0xFF);
        map[i*2 + 1] = (uint8_t)(BLANK_TILE >> 8);
    }
    memset(s_chr, 0, sizeof s_chr);
    for (int t = 0; t < NTILES; t++) {
        int tx = (t % TW) * 8, ty = (t / TW) * 8;
        for (int yy = 0; yy < 8; yy++) for (int xx = 0; xx < 8; xx++) {
            int bi  = s_fbuf[(ty + yy) * VW + (tx + xx)] & 0x0F;
            int bit = 7 - xx;
            s_chr[t][yy*2 + 0]      |= (uint8_t)(((bi >> 0) & 1) << bit);
            s_chr[t][yy*2 + 1]      |= (uint8_t)(((bi >> 1) & 1) << bit);
            s_chr[t][16 + yy*2 + 0] |= (uint8_t)(((bi >> 2) & 1) << bit);
            s_chr[t][16 + yy*2 + 1] |= (uint8_t)(((bi >> 3) & 1) << bit);
        }
        int r = t / TW, c = t % TW;
        int cell = (r + 1) * 32 + (c + 1);          /* centered, 1-tile margin */
        map[cell*2 + 0] = (uint8_t)(t & 0xFF);
        map[cell*2 + 1] = (uint8_t)((t >> 8) & 0xFF);
    }
    mg_state_dirty_bg(0, 0, MG_BG_TILEMAP_BYTES);
}

int copro_r3d_render(void) {
    if (!s_inited) copro_r3d_init();

    /* commit-ahead gate: don't stage past depth 2 (mirror h_frame_commit). */
    if ((cart_window_frame_staged() - cart_window_frame_consumed()) >= 2u)
        return -1;

    build_scene();
    r3d_render_dither(&s_scene, s_fbuf, VW, VH);

    /* one-time VRAM wipe so the backdrop/margins are clean (before any CHR). */
    if (s_need_clear) {
        mg_state_arm_clean_slate_vram_clear();
        s_need_clear = false;
    }

    /* palette -> shadow CGRAM */
    build_palette(mg_state()->cgram_shadow);
    mg_state_dirty_cgram(0, 32);                    /* 16 entries * 2 B */

    /* CHR swizzle + tilemap shadow */
    encode_4bpp();

    /* BG1 = 4bpp Mode-1 main layer */
    MgState *s = mg_state();
    s->bgmode = 1;
    s->bg[0].tilemap_word = TMAP_W;
    s->bg[0].chr_word     = CHR_W;
    s->bg[0].size_code    = 0;                      /* 32x32 */
    s->bg[0].enabled_main = true;

    /* CHR as transient slot chunks (each under the sub-frame byte budget) */
    for (int c = 0; c < NTILES; c += CHUNK_TILES) {
        int n = (NTILES - c < CHUNK_TILES) ? (NTILES - c) : CHUNK_TILES;
        mg_state_queue_dma_transient(&s_chr[c][0], (uint32_t)n * 32u,
                                     0x18, 0x01, (uint16_t)(CHR_W + c * 16));
    }

    mg_state_build_frame();
    cart_window_set_frame_ready(1);
    return 0;
}
