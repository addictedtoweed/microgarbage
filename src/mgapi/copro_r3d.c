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
/* TEAR-FREE 240x200 20fps: 3 THIRDS, double-buffered via OVERLAPPING CHR bases +
 * a shared palette-1 tilemap (docs/emitter-kernel.md; proven in snes/dbuf3_test.s).
 * third 0 = top (tiles 0..249, SHARED), 1 = mid (250..499), 2 = bot (500..749). */
#define NBANDS 3
#define THIRD_TILES 250
static const int s_third_t0[NBANDS + 1] = { 0, 250, 500, 750 };
/* delivery order: bottom, mid, TOP LAST (top is the shared band written under the
 * subframe-3 blank, then the 4-register reveal fires). */
static const int s_deliver_order[NBANDS] = { 2, 1, 0 };

/* --- VRAM word map (overlapping bases so the shared top is reached from both
 * frames' bases at different indices; beats the 10-bit tile-index limit). --- */
#define BG1_MIDA  0x0000u    /* BG1 4bpp: frame A mid / bot / SHARED top / B mid/bot */
#define BG1_BOTA  0x1000u
#define BG1_TOP   0x2000u    /* SHARED (base_A idx 512, base_B idx 0)                */
#define BG1_MIDB  0x3000u
#define BG1_BOTB  0x4000u
#define BG3_MIDA  0x5000u    /* BG3 2bpp thirds (0x800 spacing)                     */
#define BG3_BOTA  0x5800u
#define BG3_TOP   0x6000u    /* SHARED                                              */
#define BG3_MIDB  0x6800u
#define BG3_BOTB  0x7000u
#define TMAP_A_W  0x7800u    /* shared BG1+BG3 tilemap, parity A                    */
#define TMAP_B_W  0x7C00u    /* parity B                                            */
/* register values for the 4-register reveal (BG12NBA/BG34NBA in 0x1000w units;
 * BG1SC/BG3SC = tilemap base 0x400w units in bits 2-7). */
#define NBA1_A 0x00u         /* BG1 base_A = 0x0000 */
#define NBA1_B 0x02u         /* BG1 base_B = 0x2000 */
#define NBA3_A 0x05u         /* BG3 base3_A = 0x5000 */
#define NBA3_B 0x06u         /* BG3 base3_B = 0x6000 */
#define SC_VAL_A 0x78u       /* tilemap_A @ 0x7800 -> (0x7800/0x400)<<2 */
#define SC_VAL_B 0x7Cu       /* tilemap_B @ 0x7C00 */
/* palette field 1 in the shared tilemap: BG1(4bpp x16)->CGRAM 16-31 hues,
 * BG3(2bpp x4)->CGRAM 4-7 brightness (non-overlapping -> full 60 colours). */
#define TMAP_PAL1 0x0400u    /* palette 1 = bit 10 */
#define HUE_CG_BASE 16       /* BG1 palette-1 CGRAM base */
#define SUB_CG_BASE 4        /* BG3 palette-1 CGRAM base */
#define BLANK_TILE 255       /* a zeroed tile index in the alignment gap (250..255) */

/* Per-third sizes are CONSTANT (all thirds = THIRD_TILES). BG1 burst fits the
 * ~62-line force-blank window; BG3 is delivered by the per-line H-blank siphon. */
#define THIRD_BG1_BYTES (THIRD_TILES * 32)   /* 8000 @200 (fits force-blank burst) */
#define THIRD_BG3_BYTES (THIRD_TILES * 16)   /* 4000 @200 (siphoned over visible)  */

/* cart-window layout: per-subframe third CHR staging + a small descriptor mailbox
 * the emitted finish READS (dest + reveal — "read from a cart descriptor" per the
 * emitter vision). Tilemaps + palette are staged once at init. */
#define R3D_STAGE_BG1  0x0000u   /* one third's BG1 4bpp CHR */
#define R3D_STAGE_BG3  0x2000u   /* one third's BG3 2bpp CHR */
#define R3D_OFF_TMAP_A 0x3000u   /* parity-A shared tilemap (2 KB, init) */
#define R3D_OFF_TMAP_B 0x3800u   /* parity-B shared tilemap (2 KB, init) */
#define R3D_OFF_PAL    0x4000u   /* palette (64 B, init) */
#define R3D_MB         0x4100u   /* subframe descriptor: */
#define R3D_MB_BG1DST  (R3D_MB + 0u)   /* u16 LE: BG1 VRAM word dest for this third */
#define R3D_MB_BG3DST  (R3D_MB + 2u)   /* u16 LE: BG3 VRAM word dest */
#define R3D_MB_REVEAL  (R3D_MB + 4u)   /* u8: 0 = no reveal; 1 = reveal after this third */
#define R3D_MB_NBA1    (R3D_MB + 5u)   /* u8: BG12NBA to latch on reveal */
#define R3D_MB_NBA3    (R3D_MB + 6u)   /* u8: BG34NBA */
#define R3D_MB_SC      (R3D_MB + 7u)   /* u8: BG1SC = BG3SC */

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
static bool        s_have_frame;     /* v2.46: s_chr holds a rendered frame (ahead) */
static int         s_build_parity;   /* 0=A, 1=B: the OFF-SCREEN parity being filled */

/* render scratch */
static uint8_t  s_fbuf[VW * VH];
static uint8_t  s_chr [NTILES][32];   /* BG1 4bpp base CHR (whole frame) */
static uint8_t  s_chr2[NTILES][16];   /* BG3 2bpp sub  CHR */
static uint8_t  s_tmap1[2048];        /* BG1 tilemap (host, fixed) */
static uint8_t  s_tmap3[2048];        /* BG3 tilemap */
static uint16_t s_pal[32];            /* CGRAM 0-31: palette-1 split (16-31 hues, 4-7 sub) */
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
/* Palette-1 layout for the shared tilemap: BG1 hues -> CGRAM 16-31, BG3 brightness
 * -> CGRAM 4-7 (non-overlapping; 4bpp scales the palette field x16, 2bpp x4). */
static void build_palette(void) {
    memset(s_pal, 0, sizeof s_pal);
    for (int i = 0; i < 16; i++) s_pal[HUE_CG_BASE + i] = hc_base[i];   /* 16-31 hues */
    for (int i = 0; i < 4;  i++) s_pal[SUB_CG_BASE + i] = hc_sub[i];    /* 4-7 brightness */
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
    s_have_frame = false;
    s_build_parity = 1;    /* init displays parity A; the first frame builds B */

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

/* Shared-tilemap index for raster tile N (0..749), per parity. Derived from the
 * overlapping-base VRAM map (docs/emitter-kernel.md): each frame's 3 thirds sit at
 * indices 0/256/512 from its own base; the SHARED top third is at 512 for frame A
 * (base 0x0000) and 0 for frame B (base 0x2000). BG1 and BG3 land on the SAME
 * indices from their respective bases, so one tilemap drives both layers. */
static uint16_t tmap_idx_A(int N) {
    if (N < THIRD_TILES)     return (uint16_t)(512 + N);              /* top (shared)  */
    if (N < 2 * THIRD_TILES) return (uint16_t)(N - THIRD_TILES);     /* mid  -> mid_A */
    return (uint16_t)(256 + (N - 2 * THIRD_TILES));                  /* bot  -> bot_A */
}
static uint16_t tmap_idx_B(int N) {
    if (N < THIRD_TILES)     return (uint16_t)N;                     /* top (shared)  */
    if (N < 2 * THIRD_TILES) return (uint16_t)(256 + (N - THIRD_TILES));
    return (uint16_t)(512 + (N - 2 * THIRD_TILES));
}

/* Build the TWO parity tilemaps (s_tmap1 = parity A, s_tmap3 = parity B), each a
 * 32x32 shared BG1+BG3 map on palette field 1. Fixed for the frame; the CHR
 * content is what ping-pongs. Image at cells (0..24, 0..29); margins = blank tile. */
static void fill_tilemaps(void) {
    uint16_t blank = (uint16_t)(BLANK_TILE | TMAP_PAL1);
    for (int i = 0; i < 1024; i++) {
        s_tmap1[i*2 + 0] = (uint8_t)(blank & 0xFF);
        s_tmap1[i*2 + 1] = (uint8_t)(blank >> 8);
        s_tmap3[i*2 + 0] = (uint8_t)(blank & 0xFF);
        s_tmap3[i*2 + 1] = (uint8_t)(blank >> 8);
    }
    for (int t = 0; t < NTILES; t++) {
        int r = t / TW, c = t % TW;
        int cell = r * 32 + c;
        uint16_t a = (uint16_t)(tmap_idx_A(t) | TMAP_PAL1);
        uint16_t b = (uint16_t)(tmap_idx_B(t) | TMAP_PAL1);
        s_tmap1[cell*2 + 0] = (uint8_t)(a & 0xFF);
        s_tmap1[cell*2 + 1] = (uint8_t)(a >> 8);
        s_tmap3[cell*2 + 0] = (uint8_t)(b & 0xFF);
        s_tmap3[cell*2 + 1] = (uint8_t)(b >> 8);
    }
}

/* Off-screen VRAM word dest for a given third + building parity. The top/shared
 * third (0) always lands in the shared slot (BG1_TOP/BG3_TOP) — both parities read
 * it, at different tile indices, via their own bases. Mid/bot ping-pong A<->B. */
static void slot_for(int third, int parity, uint16_t *bg1, uint16_t *bg3) {
    switch (third) {
    case 0:  *bg1 = BG1_TOP;  *bg3 = BG3_TOP;  break;                 /* shared */
    case 1:  *bg1 = parity ? BG1_MIDB : BG1_MIDA;
             *bg3 = parity ? BG3_MIDB : BG3_MIDA;  break;
    default: *bg1 = parity ? BG1_BOTB : BG1_BOTA;
             *bg3 = parity ? BG3_BOTB : BG3_BOTA;  break;
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

/* ============================================================
 *  Full-emitter double-buffer ISR (docs/emitter-kernel.md): the coprocessor bakes
 *  the ENTIRE H/V virtual-NMI as 65816 into the cart-window NMI region; the SNES
 *  kernel copies it to $0E00 and points RAMVEC_IRQ there (ISR mode) — no state
 *  machine, chainer, or descriptor slot walk. Layout: entry `JMP` + init + ONE
 *  mailbox-driven finish + start. The finish READS the per-subframe VRAM dests +
 *  reveal from the cart-window descriptor mailbox (R3D_MB), so one routine drives
 *  the A<->B ping-pong (no baked per-band set). The cycle toggles purely by
 *  patching the entry JMP operand: start(V=top_lb) <-> finish(V=vis_end).
 *
 *  Stage A (here): finish force-blanks and BURSTS both BG1 (8 KB) + BG3 (4 KB) in
 *  the blank window. 12 KB slightly overruns the ~10.5 KB window -> a ~9-line black
 *  bar at the top. Stage B splits BG1 (burst) from BG3 (per-line H-blank siphon) to
 *  reclaim those lines. Proving the double-buffer architecture first isolates a
 *  black screen (architecture) from a siphon-timing bug.
 * ============================================================ */
#define ISR_TOP_LB     9u            /* start fires here (line 9): the LATE unblank (dot
                                      * 240) primes the fetch on lines 10-11 so line 12
                                      * is clean — the dogcat_test top-flicker fix.     */
/* The BG3 siphon starts AFTER the beam has drawn the top third, so writing the
 * SHARED BG3-top slot is invisible this frame (top third scanned out from the old,
 * complete slot) and lands fully for the next -> no shared-top tear. Rather than
 * push the per-line rate up to cover 4 KB in the shorter window (which risks the
 * H-blank ceiling), we siphon at the PROVEN 20 B/line from a safely-late start and
 * burst the small leftover BG3 tail in the finish blank (also after the top third
 * is scanned -> equally tear-free). Raise ISR_SIP_FIRST if the top third still
 * tears; keep FIRST <= 113 so the tail burst stays under the ~10 KB blank window. */
#define ISR_SIP_FIRST  100u          /* first siphon line (safely past the drawn top) */
#define ISR_SIP_LINES  113u          /* 213 - 100                                     */
#define ISR_SIP_BYTES  28u           /* wider window (hdot 240) carries 24-28 B reliably;
                                      * more per line -> smaller finish tail -> no flicker */
#define ISR_FINISH_LINE 213u         /* finish fires here (bottom lb, after the siphon)*/
#define ISR_SIP_DELIVERED (ISR_SIP_LINES * ISR_SIP_BYTES)          /* 2260 via siphon */
#define ISR_BG3_TAIL_SRC  (R3D_STAGE_BG3 + ISR_SIP_DELIVERED)      /* staging tail     */
#define ISR_BG3_TAIL_LEN  (THIRD_BG3_BYTES - ISR_SIP_DELIVERED)    /* 1740 via burst   */
#define ISR_BG3_TAIL_WOFF (ISR_SIP_DELIVERED / 2)                  /* 1130 dest words  */
#define ISR_HTIME_EVT  240u          /* LATE unblank (dot 240, right margin): force-blank
                                      * still covers the left of the turn-on line while
                                      * the rows below prime -> no stale top row. Was 22
                                      * (full-line unblank at line start = 1px stale).  */
/* H-dot where the siphon fires its right-margin force-blank + DMA. The blank eats
 * from here to the end of the visible line (~dot 277), so too LOW = a black strip on
 * the right of the siphoned lines (blend "missing"); too HIGH = the DMA/unblank runs
 * late into the next line's left edge. Our per-line DMA is only 20 B (10 words ~40
 * dots), so it fits a late fire: push past the visible edge to hide the blank. Sweep
 * 260 (wide window, visible strip) .. 276 (tight, blank hidden). */
#define ISR_HTIME_SIP  240u          /* H for siphon: earlier fire -> wider H-blank window */
#define ISR_ENTRY_ADDR 0x0E00u
#define ISR_ENTRY_OPND 0x0E01u
#define ISR_ABI_JOY    0x0DE6u       /* K_ABI_JOYPAD */
#define ISR_BANK       0xC0u
#define ISR_FRAME_DONE 0xC079C1u
/* siphon running state, WRAM below the $0E00 image (kernel-free scratch). */
#define ISR_SIP_SRC    0x0DF0u       /* u16 running staging source offset */
#define ISR_SIP_REM    0x0DF2u       /* u8  remaining siphon lines        */
#define ISR_SIP_VLINE  0x0DF3u       /* u8  current siphon V line         */
/* descriptor mailbox long addresses (cart window = bank $C0; offset X -> $C0:X). */
#define ISR_MB_BG1DST_LA (0xC00000u | R3D_MB_BG1DST)   /* u16: BG1 VRAM word dest */
#define ISR_MB_BG3DST_LA (0xC00000u | R3D_MB_BG3DST)   /* u16: BG3 VRAM word dest */
#define ISR_MB_REVEAL_LA (0xC00000u | R3D_MB_REVEAL)   /* u8:  reveal flag        */
#define ISR_MB_NBA1_LA   (0xC00000u | R3D_MB_NBA1)     /* u8:  BG12NBA on reveal  */
#define ISR_MB_NBA3_LA   (0xC00000u | R3D_MB_NBA3)     /* u8:  BG34NBA on reveal  */
#define ISR_MB_SC_LA     (0xC00000u | R3D_MB_SC)       /* u8:  BG1SC = BG3SC      */

static uint8_t  s_isr[1024];
static unsigned s_isrn;
static void ie8 (uint8_t b)  { if (s_isrn < sizeof s_isr) s_isr[s_isrn++] = b; }
static void ie16(uint16_t w) { ie8((uint8_t)(w & 0xFF)); ie8((uint8_t)(w >> 8)); }
static void ipatch16(unsigned off, uint16_t w) {
    s_isr[off] = (uint8_t)(w & 0xFF); s_isr[off + 1] = (uint8_t)(w >> 8);
}
static uint16_t iaddr(unsigned off) { return (uint16_t)(ISR_ENTRY_ADDR + off); }

/* lda #v ; sta abs   (M=1) */
static void e_stimm8(uint8_t v, uint16_t abs) {
    ie8(0xA9); ie8(v); ie8(0x8D); ie16(abs);
}
/* rep#$20 ; lda #v ; sta abs ; sep#$20   (M=1 in/out) */
static void e_stimm16(uint16_t v, uint16_t abs) {
    ie8(0xC2); ie8(0x20); ie8(0xA9); ie16(v); ie8(0x8D); ie16(abs); ie8(0xE2); ie8(0x20);
}
/* rep#$30; pha; phx; phy; sep#$20; lda f:$004211 (ack) */
static void e_prologue(void) {
    ie8(0xC2); ie8(0x30); ie8(0x48); ie8(0xDA); ie8(0x5A);
    ie8(0xE2); ie8(0x20); ie8(0xAF); ie8(0x11); ie8(0x42); ie8(0x00);
}
/* rep#$30; ply; plx; pla; rti */
static void e_end(void) {
    ie8(0xC2); ie8(0x30); ie8(0x7A); ie8(0xFA); ie8(0x68); ie8(0x40);
}
/* one baked channel-0 VRAM DMA: BBAD0/DMAP0/A1T0L/DAS0L + VMAIN/VMADDL + MDMAEN.
 * A1B0 (bank) assumed already set. M=1 in/out. */
static void e_dma_vram(uint16_t src, uint16_t size, uint16_t dest) {
    e_stimm8(0x18, 0x4301);          /* bbus VMDATAL -> BBAD0 */
    e_stimm8(0x01, 0x4300);          /* dmap word    -> DMAP0 */
    ie8(0xC2); ie8(0x20);            /* rep #$20 */
    ie8(0xA9); ie16(src);  ie8(0x8D); ie16(0x4302);   /* lda #src ; sta A1T0L */
    ie8(0xA9); ie16(size); ie8(0x8D); ie16(0x4305);   /* lda #size; sta DAS0L */
    ie8(0xE2); ie8(0x20);            /* sep #$20 */
    e_stimm8(0x80, 0x2115);          /* VMAIN incr-after-high */
    e_stimm16(dest, 0x2116);         /* VMADDL = dest */
    e_stimm8(0x01, 0x420B);          /* MDMAEN ch0 */
}

/* one baked channel-0 CGRAM DMA (mode 0, 1 byte -> CGDATA). M=1 in/out. */
static void e_dma_cgram(uint16_t src, uint16_t size, uint8_t cgadd) {
    e_stimm8(0x22, 0x4301);          /* bbus CGDATA -> BBAD0 */
    e_stimm8(0x00, 0x4300);          /* dmap 1-byte -> DMAP0 */
    ie8(0xC2); ie8(0x20);
    ie8(0xA9); ie16(src);  ie8(0x8D); ie16(0x4302);
    ie8(0xA9); ie16(size); ie8(0x8D); ie16(0x4305);
    ie8(0xE2); ie8(0x20);
    e_stimm8(cgadd, 0x2121);         /* CGADD */
    e_stimm8(0x01, 0x420B);          /* MDMAEN ch0 */
}

/* lda f:la24  (opcode $AF; width follows M — 8-bit under sep, 16-bit under rep). */
static void e_lda_long(uint32_t la) {
    ie8(0xAF); ie8((uint8_t)(la & 0xFF));
    ie8((uint8_t)((la >> 8) & 0xFF)); ie8((uint8_t)((la >> 16) & 0xFF));
}

/* channel-0 VRAM DMA whose VRAM word dest is READ (16-bit) from a cart-window
 * mailbox long address (plus a baked word offset) instead of baked. src16 + size16
 * baked; A1B0 set by caller. M=1 in/out. */
static void e_dma_vram_mb_off(uint16_t src, uint16_t size, uint32_t mb_dest_la,
                              uint16_t dest_woff) {
    e_stimm8(0x18, 0x4301);          /* BBAD0 = VMDATAL */
    e_stimm8(0x01, 0x4300);          /* DMAP0 = word, incr */
    e_stimm8(0x80, 0x2115);          /* VMAIN incr-after-high */
    ie8(0xC2); ie8(0x20);            /* rep #$20 */
    ie8(0xA9); ie16(src);  ie8(0x8D); ie16(0x4302);   /* lda #src ; sta A1T0L */
    ie8(0xA9); ie16(size); ie8(0x8D); ie16(0x4305);   /* lda #size; sta DAS0L */
    e_lda_long(mb_dest_la);                           /* lda f:[dest] (16b) */
    if (dest_woff) { ie8(0x18); ie8(0x69); ie16(dest_woff); }  /* clc; adc #woff */
    ie8(0x8D); ie16(0x2116);                          /* sta VMADDL */
    ie8(0xE2); ie8(0x20);            /* sep #$20 */
    e_stimm8(0x01, 0x420B);          /* MDMAEN ch0 */
}

/* mailbox-driven reveal: if R3D_MB_REVEAL != 0, latch BG12NBA/BG34NBA/BG1SC/BG3SC
 * from the mailbox (atomic parity flip; runs during the finish force-blank). */
static void e_reveal(void) {
    e_lda_long(ISR_MB_REVEAL_LA);                     /* lda f:reveal -> sets Z */
    ie8(0xF0); ie8(28);                               /* beq +28 (skip block) */
    e_lda_long(ISR_MB_NBA1_LA); ie8(0x8D); ie16(0x210B);  /* BG12NBA */
    e_lda_long(ISR_MB_NBA3_LA); ie8(0x8D); ie16(0x210C);  /* BG34NBA */
    e_lda_long(ISR_MB_SC_LA);   ie8(0x8D); ie16(0x2107);  /* BG1SC */
    e_lda_long(ISR_MB_SC_LA);   ie8(0x8D); ie16(0x2109);  /* BG3SC (shared tilemap) */
}

/* One-time init (runs FIRST): PPU config + DMA both parity tilemaps + palette, set
 * NMITIMEN to V-IRQ only, display parity A. Chains to start (V=top_lb). */
static void e_init(unsigned *startpatch_imm) {
    e_prologue();
    e_stimm8(0x80, 0x2100);                          /* force-blank */
    e_stimm8(0x30, 0x4200);                          /* NMITIMEN: H+V IRQ, NMI off, no auto-joy */
    e_stimm8(ISR_HTIME_EVT, 0x4207); ie8(0x9C); ie16(0x4208);  /* HTIMEL=22, HTIMEH=0 */
    e_stimm8(0x01, 0x2105);                          /* BGMODE = Mode 1 */
    e_stimm8(0x01, 0x212C);                          /* TM = BG1 main */
    e_stimm8(0x04, 0x212D);                          /* TS = BG3 sub */
    e_stimm8(0x02, 0x2130);                          /* CGWSEL */
    e_stimm8(0x41, 0x2131);                          /* CGADSUB: BG1, half, add */
    /* BGVOFS = -8 ($3F8) on both layers -> centre cell-row-1 in the 12/12 letterbox.
     * Write-twice ($F8 low, $03 high). */
    ie8(0xA9); ie8(0xF8); ie8(0x8D); ie16(0x210E);   /* BG1VOFS */
    ie8(0xA9); ie8(0x03); ie8(0x8D); ie16(0x210E);
    ie8(0xA9); ie8(0xF8); ie8(0x8D); ie16(0x2112);   /* BG3VOFS */
    ie8(0xA9); ie8(0x03); ie8(0x8D); ie16(0x2112);
    /* initial display = parity A */
    e_stimm8(NBA1_A,   0x210B);                       /* BG12NBA */
    e_stimm8(NBA3_A,   0x210C);                       /* BG34NBA */
    e_stimm8(SC_VAL_A, 0x2107);                       /* BG1SC */
    e_stimm8(SC_VAL_A, 0x2109);                       /* BG3SC (shared) */
    /* DMA static data: palette -> CGRAM 0, both parity tilemaps -> VRAM. */
    e_stimm8(ISR_BANK, 0x4304);                       /* A1B0 bank */
    e_dma_cgram(R3D_OFF_PAL, (uint16_t)sizeof s_pal, 0);
    e_dma_vram(R3D_OFF_TMAP_A, 2048, TMAP_A_W);
    e_dma_vram(R3D_OFF_TMAP_B, 2048, TMAP_B_W);
    /* patch entry -> start (fixup) */
    ie8(0xC2); ie8(0x20);
    ie8(0xA9); *startpatch_imm = s_isrn; ie16(0x0000);
    ie8(0x8D); ie16(ISR_ENTRY_OPND);
    ie8(0xE2); ie8(0x20);
    e_stimm8(ISR_TOP_LB, 0x4209);
    ie8(0x9C); ie16(0x420A);
    e_end();
}

/* Single mailbox-driven finish (V=vis_end): force-blank, burst BG1+BG3 from the
 * shared staging slots to the mailbox dests (Stage A: both in the blank window),
 * apply the mailbox reveal, strobe FRAME_DONE, hand to start (V=top_lb). */
static void e_finish(unsigned *startpatch_imm) {
    e_prologue();
    e_stimm8(0x80, 0x2100);          /* force-blank INIDISP */
    e_stimm8(ISR_BANK, 0x4304);      /* A1B0 source bank */
    e_dma_vram_mb_off(R3D_STAGE_BG1, THIRD_BG1_BYTES, ISR_MB_BG1DST_LA, 0);  /* BG1 8 KB */
    /* BG3 tail: the siphon delivered the first ISR_SIP_DELIVERED bytes over the
     * visible field; burst the remainder here in the blank (still after the top third
     * is scanned -> tear-free). BG1(8000)+tail(~1740) ~= 9.7 KB fits the ~10 KB window,
     * so start's IRQ is never dropped (the Stage-A 12 KB-overrun flicker). */
    e_dma_vram_mb_off(ISR_BG3_TAIL_SRC, ISR_BG3_TAIL_LEN, ISR_MB_BG3DST_LA,
                      ISR_BG3_TAIL_WOFF);
    e_reveal();                      /* atomic parity flip if this third revealed */
    ie8(0xAF); ie8(0xC1); ie8(0x79); ie8(0xC0);       /* lda f:$C079C1  strobe FRAME_DONE */
    /* patch entry -> start (fixup) */
    ie8(0xC2); ie8(0x20);
    ie8(0xA9); *startpatch_imm = s_isrn; ie16(0x0000);
    ie8(0x8D); ie16(ISR_ENTRY_OPND);
    ie8(0xE2); ie8(0x20);
    e_stimm8(ISR_TOP_LB, 0x4209);    /* VTIMEL = top_lb */
    ie8(0x9C); ie16(0x420A);         /* stz VTIMEH */
    e_end();
}

/* start (fires at (22,12)): while STILL force-blanked (finish left $80 on through
 * vblank), seed the BG3 siphon channel — VMADDL from the mailbox MUST be set blanked
 * (writing it mid-display corrupts) — then unblank, read joypads, move H to the right
 * margin, and hand to the per-line siphon (V=13). */
static void e_start(unsigned *siphonpatch_imm) {
    e_prologue();
    /* SENTINEL: the guest sets frame_ready ($C0:7800) only AFTER both BG1+BG3 are
     * staged. If it's 0 here, the worker hasn't finished staging this subframe —
     * reading a half-written BG3 is the mgapi thread race that flickers the colour
     * math. So HOLD: unblank the last complete frame, re-arm start, retry next frame
     * (entry stays -> start, no siphon/finish this frame, no FRAME_DONE). */
    ie8(0xAF); ie8(0x00); ie8(0x78); ie8(0xC0);      /* lda f:$C07800 (frame_ready) */
    ie8(0xD0); unsigned ready_rel = s_isrn; ie8(0x00);   /* bne @ready (rel fixed below) */
    e_stimm8(0x0F, 0x2100);                          /* unblank -> show last frame */
    e_stimm8(ISR_HTIME_EVT, 0x4207); ie8(0x9C); ie16(0x4208);   /* HTIME = event dot */
    e_stimm8(ISR_TOP_LB, 0x4209);    ie8(0x9C); ie16(0x420A);   /* VTIME = start line */
    e_end();
    s_isr[ready_rel] = (uint8_t)(s_isrn - (ready_rel + 1));      /* @ready lands here */
    /* --- seed BG3 siphon ch0 (blanked): BBAD0/DMAP0/VMAIN/A1B0 + VMADDL + src/rem --- */
    e_stimm8(0x18, 0x4301);          /* BBAD0 = VMDATAL */
    e_stimm8(0x01, 0x4300);          /* DMAP0 = word, incr src */
    e_stimm8(0x80, 0x2115);          /* VMAIN incr-after-high */
    e_stimm8(ISR_BANK, 0x4304);      /* A1B0 = staging bank $C0 */
    ie8(0xC2); ie8(0x20);            /* rep #$20 */
    e_lda_long(ISR_MB_BG3DST_LA); ie8(0x8D); ie16(0x2116);       /* VMADDL = [MB_BG3DST] */
    ie8(0xA9); ie16(R3D_STAGE_BG3); ie8(0x8D); ie16(ISR_SIP_SRC);/* src = staging base */
    ie8(0xE2); ie8(0x20);            /* sep #$20 */
    e_stimm8((uint8_t)ISR_SIP_LINES, ISR_SIP_REM);   /* remaining = 200 lines */
    e_stimm8(ISR_SIP_FIRST, ISR_SIP_VLINE);          /* first siphon line = 13 */
    /* --- unblank the visible field, read joypads --- */
    e_stimm8(0x0F, 0x2100);          /* unblank */
    ie8(0x9C); ie16(0x2121);         /* stz CGADD */
    ie8(0x20); ie16(ISR_ABI_JOY);    /* jsr K_ABI_JOYPAD (leaves A8/I8) */
    /* --- H -> right margin, hand to the per-line siphon (V=13) --- */
    e_stimm8((uint8_t)(ISR_HTIME_SIP & 0xFF), 0x4207);   /* HTIMEL = 260 & 255 */
    e_stimm8((uint8_t)(ISR_HTIME_SIP >> 8),   0x4208);   /* HTIMEH = 260 >> 8 */
    ie8(0xC2); ie8(0x20);            /* rep #$20 */
    ie8(0xA9); *siphonpatch_imm = s_isrn; ie16(0x0000);  /* lda #<siphon> */
    ie8(0x8D); ie16(ISR_ENTRY_OPND); /* sta $0E01 */
    ie8(0xE2); ie8(0x20);            /* sep #$20 */
    e_stimm8(ISR_SIP_FIRST, 0x4209); /* VTIMEL = 13 */
    ie8(0x9C); ie16(0x420A);         /* stz VTIMEH */
    e_end();
}

/* Per-line BG3 siphon (fires at (260, line) for lines 13..212). LEAN: saves ONLY A
 * (no X/Y — the kernel's ISR-mode @loop reloads A each pass; RTI restores P). Each
 * line moves ISR_SIP_BYTES of BG3 into the off-screen slot via a right-margin
 * force-blank DMA; VMADDL persists + auto-increments (set once in start), so only
 * A1T0L/DAS0L are re-seeded. After the 200th line it hands to finish (HTIME->22,
 * entry->finish, V=213). Mirrors snes/siphon_hdot_test.s (clean at 20 B/line). */
static void e_siphon(unsigned *finishpatch_imm) {
    ie8(0xC2); ie8(0x30);            /* rep #$30 (known width) */
    ie8(0x48);                       /* pha  (save A16) */
    ie8(0xE2); ie8(0x20);            /* sep #$20 (A8) */
    ie8(0xAD); ie16(0x4211);         /* lda $4211  ack TIMEUP */
    /* per-line source + count (VMADDL auto-increments, not touched) */
    ie8(0xC2); ie8(0x20);            /* rep #$20 */
    ie8(0xAD); ie16(ISR_SIP_SRC);    /* lda ISR_SIP_SRC */
    ie8(0x8D); ie16(0x4302);         /* sta A1T0L */
    ie8(0xA9); ie16(ISR_SIP_BYTES);  /* lda #ISR_SIP_BYTES */
    ie8(0x8D); ie16(0x4305);         /* sta DAS0L */
    ie8(0xE2); ie8(0x20);            /* sep #$20 */
    /* right-margin force-blank DMA (no spin) */
    e_stimm8(0x8F, 0x2100);          /* force-blank */
    e_stimm8(0x01, 0x420B);          /* MDMAEN ch0 */
    e_stimm8(0x0F, 0x2100);          /* unblank */
    /* advance src += ISR_SIP_BYTES */
    ie8(0xC2); ie8(0x20);            /* rep #$20 */
    ie8(0xAD); ie16(ISR_SIP_SRC);
    ie8(0x18);                       /* clc */
    ie8(0x69); ie16(ISR_SIP_BYTES);  /* adc #ISR_SIP_BYTES */
    ie8(0x8D); ie16(ISR_SIP_SRC);
    ie8(0xE2); ie8(0x20);            /* sep #$20 */
    /* dec remaining; 0 -> @done (hand to finish) */
    ie8(0xCE); ie16(ISR_SIP_REM);    /* dec ISR_SIP_REM (8-bit) */
    ie8(0xF0); ie8(16);              /* beq +16 (-> @done) */
    /* @next_line: march VTIME down one line (H persists at 260) */
    ie8(0xEE); ie16(ISR_SIP_VLINE);  /* inc ISR_SIP_VLINE */
    ie8(0xAD); ie16(ISR_SIP_VLINE);  /* lda ISR_SIP_VLINE */
    ie8(0x8D); ie16(0x4209);         /* sta VTIMEL */
    ie8(0x9C); ie16(0x420A);         /* stz VTIMEH */
    ie8(0xC2); ie8(0x20);            /* rep #$20 */
    ie8(0x68);                       /* pla */
    ie8(0x40);                       /* rti */
    /* @done: last BG3 chunk landed -> hand to finish */
    e_stimm8(ISR_HTIME_EVT, 0x4207); /* HTIMEL = 22 */
    ie8(0x9C); ie16(0x4208);         /* stz HTIMEH */
    ie8(0xC2); ie8(0x20);            /* rep #$20 */
    ie8(0xA9); *finishpatch_imm = s_isrn; ie16(0x0000);  /* lda #<finish> */
    ie8(0x8D); ie16(ISR_ENTRY_OPND); /* sta $0E01 */
    ie8(0xE2); ie8(0x20);            /* sep #$20 */
    e_stimm8(ISR_FINISH_LINE, 0x4209);  /* VTIMEL = 213 */
    ie8(0x9C); ie16(0x420A);         /* stz VTIMEH */
    ie8(0xC2); ie8(0x20);            /* rep #$20 */
    ie8(0x68);                       /* pla */
    ie8(0x40);                       /* rti */
}

/* Build the ISR image, stage it to the cart-window NMI region, bump the version
 * (kernel copies it to $0E00), and raise COPRO_ISR_MODE. Runs once. */
static void install_isr_image(void) {
    s_isrn = 0;
    ie8(0x4C); unsigned entry_opnd = s_isrn; ie16(0x0000);   /* entry: JMP <init> */
    unsigned init_off = s_isrn, init_startpatch;
    e_init(&init_startpatch);
    unsigned start_off = s_isrn, start_siphonpatch;
    e_start(&start_siphonpatch);
    unsigned siphon_off = s_isrn, siphon_finishpatch;
    e_siphon(&siphon_finishpatch);
    unsigned finish_off = s_isrn, finish_startpatch;
    e_finish(&finish_startpatch);
    ipatch16(entry_opnd, iaddr(init_off));            /* entry  -> init (runs once) */
    ipatch16(init_startpatch,    iaddr(start_off));   /* init   -> start */
    ipatch16(start_siphonpatch,  iaddr(siphon_off));  /* start  -> siphon */
    ipatch16(siphon_finishpatch, iaddr(finish_off));  /* siphon -> finish */
    ipatch16(finish_startpatch,  iaddr(start_off));   /* finish -> start */
    /* ORDER: raise ISR mode BEFORE the version bump. The kernel's version-poll
     * pass copies the image to $0E00 AND sets K_NMI_CUSTOM=1; if it saw the bump
     * without ISR mode already set, state A would `jsr` this full ISR (which ends
     * in RTI, not RTS) for one frame and corrupt the stack. With ISR mode set
     * first, the same pass enters ISR mode (RAMVEC->$0E00) so state A never runs. */
    { static const uint8_t on = 1; cart_window_load_blob(CW_OFF_ISR_MODE, &on, 1); }
    cart_window_load_blob(CW_OFF_NMI_CODE, s_isr, s_isrn);
    cart_window_bump_nmi_version();
    fprintf(stderr, "[r3d] ISR image installed (%u bytes) -> ISR mode ARMED\n", s_isrn);
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

    /* Ahead-render: keep s_chr ONE frame ahead so every third delivery is a fast
     * memcpy from a resident frame, never a just-in-time raster (avoids shear). */
    if (!s_have_frame) {
        build_scene();
        r3d_render(&s_scene, s_fbuf, VW, VH);      /* flat per-face shade (no dither) */
        encode_dual();
        s_have_frame = true;
    }

    /* One-time: stage the two shared parity tilemaps + palette into the cart window,
     * set the 12/12 letterbox, and install the emitted double-buffer ISR (its init
     * routine DMAs tilemaps/palette to VRAM and displays parity A). */
    if (!s_init_done) {
        cart_window_store_u16_le(CW_OFF_KERNEL_LAYOUT, (uint16_t)(12u | (12u << 8)));
        /* No kernel HDMA in the emitter path: zero frame_dma's ch1-6 config. */
        static const uint8_t zeros[7 * 8] = {0};
        cart_window_load_blob(0x7968u /* COPRO_HDMA_CONFIG */, zeros, sizeof zeros);
        build_palette();
        fill_tilemaps();
        cart_window_load_blob(R3D_OFF_PAL,    s_pal,   sizeof s_pal);
        cart_window_load_blob(R3D_OFF_TMAP_A, s_tmap1, sizeof s_tmap1);   /* parity A */
        cart_window_load_blob(R3D_OFF_TMAP_B, s_tmap3, sizeof s_tmap3);   /* parity B */
        install_isr_image();
        s_deliver      = 0;
        s_build_parity = 1;              /* init shows parity A; first frame builds B */
        s_init_done    = true;
        if (s_trace)
            fprintf(stderr, "[r3d] init: tilemaps+palette staged, double-buffer ISR armed\n");
    }

    /* Deliver this subframe's third of the BUILDING (off-screen) parity. Order is
     * bottom, mid, TOP-LAST — the shared top third lands under the subframe-3 blank,
     * immediately before its reveal. */
    int sub   = s_deliver;                 /* 0..2 within this frame       */
    int third = s_deliver_order[sub];      /* 2=bot, 1=mid, 0=top (last)   */
    int t0    = s_third_t0[third];

    /* stage the third's CHR into the shared per-subframe staging slots */
    cart_window_load_blob(R3D_STAGE_BG1, &s_chr [t0][0], THIRD_BG1_BYTES);
    cart_window_load_blob(R3D_STAGE_BG3, &s_chr2[t0][0], THIRD_BG3_BYTES);

    /* descriptor mailbox: the emitted finish READS these — VRAM word dests for the
     * building parity + the 4-register reveal (only on the shared top third). */
    uint16_t bg1_dst, bg3_dst;
    slot_for(third, s_build_parity, &bg1_dst, &bg3_dst);
    cart_window_store_u16_le(R3D_MB_BG1DST, bg1_dst);
    cart_window_store_u16_le(R3D_MB_BG3DST, bg3_dst);
    {
        uint8_t desc[4];
        if (third == 0) {                  /* reveal -> flip display to the built parity */
            desc[0] = 1;
            desc[1] = s_build_parity ? NBA1_B   : NBA1_A;
            desc[2] = s_build_parity ? NBA3_B   : NBA3_A;
            desc[3] = s_build_parity ? SC_VAL_B : SC_VAL_A;
        } else {
            desc[0] = desc[1] = desc[2] = desc[3] = 0;
        }
        cart_window_load_blob(R3D_MB_REVEAL, desc, sizeof desc);
    }

    cart_window_set_frame_ready(1);

    if (s_trace)
        fprintf(stderr, "[r3d] sub %d -> third %d (tiles %d..%d) build=%c bg1=%04X bg3=%04X%s\n",
                sub, third, t0, t0 + THIRD_TILES, s_build_parity ? 'B' : 'A',
                bg1_dst, bg3_dst, third == 0 ? " REVEAL" : "");

    /* advance; on frame wrap flip the building parity + render the next frame ahead
     * (this frame's CHR is now fully copied out of s_chr). */
    s_deliver = sub + 1;
    if (s_deliver >= NBANDS) {
        s_deliver      = 0;
        s_build_parity ^= 1;
        build_scene();
        r3d_render(&s_scene, s_fbuf, VW, VH);      /* flat per-face shade (no dither) */
        encode_dual();
    }
    return 0;
}
