/* ============================================================
 *  ppu.c — SNES PPU rasterizer (Mode 0 / Mode 1 backgrounds).
 *
 *  Renders a decoded PpuState snapshot to a 256x224 RGBA framebuffer.
 *  See include/video/ppu.h for the contract and scope. Scanline-
 *  structured so per-line HDMA register changes drop in later.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "video/ppu.h"
#include "video/present.h"   /* PRESENT_RGBA — keep the pixel format in sync */

#include <string.h>

/* ---- color ------------------------------------------------- */

/* BGR555 -> RGBA8888 with master-brightness scaling (bright 0..15,
 * 15 = full). 5->8 bit expansion replicates the high bits. Brightness
 * is modeled as a simple linear scale — close enough for a test tool. */
static uint32_t color_to_fb(uint16_t bgr555, unsigned bright) {
    unsigned r5 = bgr555 & 0x1Fu;
    unsigned g5 = (bgr555 >> 5) & 0x1Fu;
    unsigned b5 = (bgr555 >> 10) & 0x1Fu;
    unsigned r8 = (r5 << 3) | (r5 >> 2);
    unsigned g8 = (g5 << 3) | (g5 >> 2);
    unsigned b8 = (b5 << 3) | (b5 >> 2);
    if (bright < 15u) {
        r8 = r8 * bright / 15u;
        g8 = g8 * bright / 15u;
        b8 = b8 * bright / 15u;
    }
    return PRESENT_RGBA(r8, g8, b8);
}

uint32_t ppu_bgr555_to_rgba(uint16_t bgr555) {
    return color_to_fb(bgr555, 15u);
}

/* ---- tile fetch -------------------------------------------- */

/* Pixel value (0..2^bpp-1) of tile `tile` at intra-tile (px,py), from
 * the SNES planar/interleaved format. 0 means "transparent" to the
 * caller. bpp is 2 or 4. */
static unsigned tile_pixel(const uint16_t *vram, uint16_t char_word,
                           unsigned tile, unsigned bpp,
                           unsigned px, unsigned py) {
    unsigned words_per_tile = bpp * 4u;            /* 2bpp=8, 4bpp=16 words */
    unsigned base = (char_word + tile * words_per_tile) & 0x7FFFu;
    unsigned bit  = 7u - px;

    uint16_t w01 = vram[(base + py) & 0x7FFFu];    /* planes 0 (lo) & 1 (hi) */
    unsigned p0 = ((unsigned)(w01 & 0xFFu) >> bit) & 1u;
    unsigned p1 = ((unsigned)(w01 >> 8)    >> bit) & 1u;
    unsigned val = p0 | (p1 << 1);

    if (bpp == 4u) {
        uint16_t w23 = vram[(base + 8u + py) & 0x7FFFu]; /* planes 2 & 3 */
        unsigned p2 = ((unsigned)(w23 & 0xFFu) >> bit) & 1u;
        unsigned p3 = ((unsigned)(w23 >> 8)    >> bit) & 1u;
        val |= (p2 << 2) | (p3 << 3);
    }
    return val;
}

/* ---- one BG layer ------------------------------------------ */

typedef struct {
    const PpuBg *bg;
    unsigned     bpp;       /* 2 or 4                                 */
    unsigned     palsize;   /* colors per palette (4 or 16)           */
    unsigned     palbase;   /* CGRAM base offset (Mode 0 per-BG band) */
} LayerInfo;

/* Sample BG layer `li` at screen pixel (sx,sy). Returns true if opaque
 * (pixel value != 0); fills *out_color (BGR555) and *out_prio. */
static bool bg_sample(const PpuState *p, const LayerInfo *li,
                      unsigned sx, unsigned sy,
                      uint16_t *out_color, unsigned *out_prio) {
    const PpuBg *bg = li->bg;
    unsigned mapw = (bg->size == PPU_SC_64x32 || bg->size == PPU_SC_64x64) ? 512u : 256u;
    unsigned maph = (bg->size == PPU_SC_32x64 || bg->size == PPU_SC_64x64) ? 512u : 256u;

    unsigned bgx = ((unsigned)sx + bg->hofs) & (mapw - 1u);
    unsigned bgy = ((unsigned)sy + bg->vofs) & (maph - 1u);
    unsigned tx = bgx >> 3, ty = bgy >> 3;
    unsigned fx = bgx & 7u, fy = bgy & 7u;

    /* Select the 32x32 sub-screen for 64-wide/tall maps. */
    unsigned off = 0u;
    if (mapw == 512u && (tx & 32u)) off += 0x400u;
    if (maph == 512u && (ty & 32u)) off += (mapw == 512u) ? 0x800u : 0x400u;

    unsigned idx = off + (ty & 31u) * 32u + (tx & 31u);
    uint16_t e = p->vram[(bg->tilemap_word + idx) & 0x7FFFu];

    unsigned px = PPU_TILE_HFLIP(e) ? 7u - fx : fx;
    unsigned py = PPU_TILE_VFLIP(e) ? 7u - fy : fy;
    unsigned val = tile_pixel(p->vram, bg->char_word, PPU_TILE_NUM(e),
                              li->bpp, px, py);
    if (val == 0u) return false;   /* transparent */

    unsigned ci = li->palbase + (unsigned)PPU_TILE_PAL(e) * li->palsize + val;
    *out_color = p->cgram[ci & (PPU_CGRAM_LEN - 1u)];
    *out_prio  = PPU_TILE_PRIO(e);
    return true;
}

/* ---- compositing order ------------------------------------- */

typedef struct { uint8_t layer; uint8_t prio; } OrderEntry;

/* Build the front-to-back BG order for the mode. Returns the count and
 * fills `ord`. `ord` must hold at least 8 entries. */
static unsigned build_order(const PpuState *p, OrderEntry *ord) {
    unsigned n = 0;
    if (p->mode == 0u) {
        /* BG1/BG2 above BG3/BG4, priority-1 tiles above priority-0. */
        static const OrderEntry m0[] = {
            {0,1},{1,1},{0,0},{1,0},{2,1},{3,1},{2,0},{3,0},
        };
        for (unsigned i = 0; i < sizeof m0 / sizeof m0[0]; i++) ord[n++] = m0[i];
    } else { /* mode 1 (BG1/BG2 4bpp, BG3 2bpp) */
        if (p->bg3_priority) ord[n++] = (OrderEntry){2, 1};
        ord[n++] = (OrderEntry){0, 1};
        ord[n++] = (OrderEntry){1, 1};
        ord[n++] = (OrderEntry){0, 0};
        ord[n++] = (OrderEntry){1, 0};
        if (!p->bg3_priority) ord[n++] = (OrderEntry){2, 1};
        ord[n++] = (OrderEntry){2, 0};
    }
    return n;
}

/* Layer table for the mode: bpp / palette size / CGRAM base per BG. */
static unsigned build_layers(const PpuState *p, LayerInfo *li) {
    if (p->mode == 0u) {
        for (unsigned i = 0; i < 4u; i++) {
            li[i].bg = &p->bg[i];
            li[i].bpp = 2u; li[i].palsize = 4u;
            li[i].palbase = i * 32u;     /* BG1:0 BG2:32 BG3:64 BG4:96 */
        }
        return 4u;
    }
    /* mode 1 */
    li[0].bg = &p->bg[0]; li[0].bpp = 4u; li[0].palsize = 16u; li[0].palbase = 0u;
    li[1].bg = &p->bg[1]; li[1].bpp = 4u; li[1].palsize = 16u; li[1].palbase = 0u;
    li[2].bg = &p->bg[2]; li[2].bpp = 2u; li[2].palsize = 4u;  li[2].palbase = 0u;
    return 3u;
}

/* ---- frame ------------------------------------------------- */

void ppu_render(const PpuState *p, uint32_t *fb) {
    if (!p || !fb) return;

    if (p->forced_blank) {
        for (int i = 0; i < PPU_SCREEN_W * PPU_SCREEN_H; i++)
            fb[i] = PRESENT_RGBA(0, 0, 0);
        return;
    }

    LayerInfo li[4];
    OrderEntry ord[8];
    unsigned nlayers = build_layers(p, li);
    unsigned norder  = build_order(p, ord);
    uint16_t backdrop = p->cgram[0];

    for (unsigned y = 0; y < PPU_SCREEN_H; y++) {
        uint32_t *row = fb + (size_t)y * PPU_SCREEN_W;
        for (unsigned x = 0; x < PPU_SCREEN_W; x++) {
            /* Sample each main-screen layer once. */
            bool     op[4]  = { false, false, false, false };
            uint16_t col[4] = { 0, 0, 0, 0 };
            unsigned pr[4]  = { 0, 0, 0, 0 };
            for (unsigned l = 0; l < nlayers; l++) {
                if (li[l].bg->on_main)
                    op[l] = bg_sample(p, &li[l], x, y, &col[l], &pr[l]);
            }
            /* Walk front-to-back; first opaque match at its priority wins. */
            uint16_t out = backdrop;
            for (unsigned o = 0; o < norder; o++) {
                unsigned l = ord[o].layer;
                if (op[l] && pr[l] == ord[o].prio) { out = col[l]; break; }
            }
            row[x] = color_to_fb(out, p->brightness);
        }
    }
}

void ppu_state_clear(PpuState *p) {
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->mode = 0u;
    p->brightness = 15u;
    p->forced_blank = false;
}
