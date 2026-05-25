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

/* Color math: combine two BGR555 colors per channel (add or subtract,
 * optionally halved), clamped to 0..31. */
static uint16_t color_math(uint16_t a, uint16_t b, bool sub, bool half) {
    int ar = a & 31, ag = (a >> 5) & 31, ab = (a >> 10) & 31;
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int rr = sub ? ar - br : ar + br;
    int rg = sub ? ag - bg : ag + bg;
    int rb = sub ? ab - bb : ab + bb;
    if (half) { rr /= 2; rg /= 2; rb /= 2; }
    if (rr < 0) rr = 0; if (rr > 31) rr = 31;
    if (rg < 0) rg = 0; if (rg > 31) rg = 31;
    if (rb < 0) rb = 0; if (rb > 31) rb = 31;
    return (uint16_t)(rr | (rg << 5) | (rb << 10));
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
                      unsigned sx, unsigned sy, uint16_t hofs, uint16_t vofs,
                      uint16_t *out_color, unsigned *out_prio) {
    const PpuBg *bg = li->bg;
    unsigned mapw = (bg->size == PPU_SC_64x32 || bg->size == PPU_SC_64x64) ? 512u : 256u;
    unsigned maph = (bg->size == PPU_SC_32x64 || bg->size == PPU_SC_64x64) ? 512u : 256u;

    unsigned bgx = ((unsigned)sx + hofs) & (mapw - 1u);
    unsigned bgy = ((unsigned)sy + vofs) & (maph - 1u);
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

/* ---- sprites (OBJ) ----------------------------------------- */

#define OBJ_LAYER 4u   /* OrderEntry.layer value meaning "sprites" */

/* The 8 OBJ size pairs (small / large), selected by OBSEL bits 5-7;
 * the per-sprite size bit (OAM high table) picks small or large. */
static const struct { uint8_t sw, sh, lw, lh; } OBJ_SIZES[8] = {
    {  8,  8, 16, 16 }, {  8,  8, 32, 32 }, {  8,  8, 64, 64 },
    { 16, 16, 32, 32 }, { 16, 16, 64, 64 }, { 32, 32, 64, 64 },
    { 16, 32, 32, 64 }, { 16, 32, 32, 32 },
};

/* Build this scanline's sprite line buffers: for each x, whether a
 * sprite pixel is present, its color (BGR555), and its 2-bit priority.
 * Sprites draw in OAM order with the LOWEST index in front, so we walk
 * 0..127 and keep the first opaque writer at each x. Pixel value 0 is
 * transparent; OBJ palettes live at CGRAM 128 + pal*16. */
static void render_obj_line(const PpuState *p, unsigned y,
                            bool *op, uint16_t *col, unsigned *prio) {
    for (unsigned x = 0; x < PPU_SCREEN_W; x++) { op[x] = false; col[x] = 0; prio[x] = 0; }
    if (!p->obj_on_main) return;

    const uint8_t *oam = p->oam;
    unsigned sel = p->obj_size_sel & 7u;

    for (unsigned i = 0; i < 128u; i++) {
        const uint8_t *e = oam + i * 4u;
        unsigned hbyte = oam[512u + (i >> 2)];
        unsigned shift = (i & 3u) * 2u;
        unsigned xhi = (hbyte >> shift) & 1u;
        unsigned big = (hbyte >> (shift + 1u)) & 1u;

        unsigned w = big ? OBJ_SIZES[sel].lw : OBJ_SIZES[sel].sw;
        unsigned h = big ? OBJ_SIZES[sel].lh : OBJ_SIZES[sel].sh;

        unsigned sy = e[1];
        if (!(y >= sy && y < sy + h)) continue;       /* scanline misses sprite */

        int sx = (int)((unsigned)e[0] | (xhi << 8));  /* 9-bit, signed */
        if (sx >= 256) sx -= 512;

        unsigned attr  = e[3];
        bool     vflip = (attr >> 7) & 1u;
        bool     hflip = (attr >> 6) & 1u;
        unsigned sprio = (attr >> 4) & 3u;
        unsigned pal   = (attr >> 1) & 7u;
        unsigned tnum  = e[2] | ((attr & 1u) << 8);   /* 9-bit tile */
        unsigned page  = (tnum >> 8) & 1u;
        unsigned base_lo = tnum & 0xFFu;
        uint16_t page_base = (uint16_t)(p->obj_char_word + (page ? p->obj_gap_word : 0u));

        unsigned row = y - sy;
        if (vflip) row = h - 1u - row;
        unsigned tr = row >> 3, py = row & 7u;

        for (unsigned xx = 0; xx < w; xx++) {
            int screen_x = sx + (int)xx;
            if (screen_x < 0 || screen_x >= (int)PPU_SCREEN_W) continue;
            if (op[screen_x]) continue;               /* lower-index sprite wins */
            unsigned cin = hflip ? (w - 1u - xx) : xx; /* source column (flip-aware) */
            unsigned tc = cin >> 3, px = cin & 7u;
            unsigned cell = (base_lo + tr * 16u + tc) & 0xFFu;  /* 16-wide OBJ grid */
            unsigned val = tile_pixel(p->vram, page_base, cell, 4u, px, py);
            if (val == 0u) continue;                  /* transparent */
            op[screen_x]   = true;
            col[screen_x]  = p->cgram[(128u + pal * 16u + val) & (PPU_CGRAM_LEN - 1u)];
            prio[screen_x] = sprio;
        }
    }
}

/* ---- compositing order ------------------------------------- */

typedef struct { uint8_t layer; uint8_t prio; } OrderEntry;

/* Build the front-to-back BG order for the mode. Returns the count and
 * fills `ord`. `ord` must hold at least 8 entries. */
static unsigned build_order(const PpuState *p, OrderEntry *ord) {
    unsigned n = 0;
    const uint8_t O = (uint8_t)OBJ_LAYER;
    if (p->mode == 0u) {
        /* Sprites interleave at 4 priority levels with BG1/BG2 over BG3/BG4. */
        static const OrderEntry m0[] = {
            {4,3},{0,1},{1,1},{4,2},{0,0},{1,0},{4,1},{2,1},{3,1},{4,0},{2,0},{3,0},
        };
        for (unsigned i = 0; i < sizeof m0 / sizeof m0[0]; i++) ord[n++] = m0[i];
    } else { /* mode 1 (BG1/BG2 4bpp, BG3 2bpp) */
        if (p->bg3_priority) ord[n++] = (OrderEntry){2, 1};
        ord[n++] = (OrderEntry){O, 3};
        ord[n++] = (OrderEntry){0, 1};
        ord[n++] = (OrderEntry){1, 1};
        ord[n++] = (OrderEntry){O, 2};
        ord[n++] = (OrderEntry){0, 0};
        ord[n++] = (OrderEntry){1, 0};
        ord[n++] = (OrderEntry){O, 1};
        if (!p->bg3_priority) ord[n++] = (OrderEntry){2, 1};
        ord[n++] = (OrderEntry){O, 0};
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

/* Apply this scanline's HDMA register overrides onto the effective
 * scroll / brightness used to render the line. */
static void apply_hdma(const PpuState *p, unsigned y,
                       uint16_t eff_hofs[4], uint16_t eff_vofs[4],
                       unsigned *eff_bright) {
    for (unsigned c = 0; c < p->hdma_count && c < PPU_HDMA_MAX; c++) {
        const PpuHdmaChannel *ch = &p->hdma[c];
        if (!ch->value) continue;
        uint16_t v = ch->value[y];
        switch (ch->target) {
        case PPU_REG_BG1_HOFS: eff_hofs[0] = v; break;
        case PPU_REG_BG1_VOFS: eff_vofs[0] = v; break;
        case PPU_REG_BG2_HOFS: eff_hofs[1] = v; break;
        case PPU_REG_BG2_VOFS: eff_vofs[1] = v; break;
        case PPU_REG_BG3_HOFS: eff_hofs[2] = v; break;
        case PPU_REG_BG3_VOFS: eff_vofs[2] = v; break;
        case PPU_REG_BG4_HOFS: eff_hofs[3] = v; break;
        case PPU_REG_BG4_VOFS: eff_vofs[3] = v; break;
        case PPU_REG_BRIGHTNESS: *eff_bright = v & 15u; break;
        case PPU_REG_NONE: default: break;
        }
    }
}

/* Resolve one screen (main or sub) at a pixel: walk front-to-back and
 * return the first opaque, enabled layer at its priority, else backdrop.
 * en[] is per-layer enable (0-3 BG, 4 OBJ). Writes the winning layer id
 * (0-3 BG, 4 OBJ, 5 backdrop) to *out_layer. */
static uint16_t composite(const OrderEntry *ord, unsigned norder,
                          const bool op[4], const uint16_t col[4], const unsigned pr[4],
                          bool obj_op, uint16_t obj_col, unsigned obj_prio,
                          const bool en[5], uint16_t backdrop, int *out_layer) {
    for (unsigned o = 0; o < norder; o++) {
        unsigned l = ord[o].layer;
        if (l == OBJ_LAYER) {
            if (en[4] && obj_op && obj_prio == ord[o].prio) { *out_layer = 4; return obj_col; }
        } else if (en[l] && op[l] && pr[l] == ord[o].prio) {
            *out_layer = (int)l;
            return col[l];
        }
    }
    *out_layer = 5;
    return backdrop;
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
    OrderEntry ord[16];
    unsigned nlayers = build_layers(p, li);
    unsigned norder  = build_order(p, ord);

    /* Per-screen layer enables (BG0-3, OBJ) and whether any color math
     * is configured at all (skip the second composite if not). */
    const bool main_en[5] = { p->bg[0].on_main, p->bg[1].on_main,
                              p->bg[2].on_main, p->bg[3].on_main, p->obj_on_main };
    const bool sub_en[5]  = { p->bg[0].on_sub,  p->bg[1].on_sub,
                              p->bg[2].on_sub,  p->bg[3].on_sub,  p->obj_on_sub };
    bool cm_any = p->cm_bg[0] || p->cm_bg[1] || p->cm_bg[2] || p->cm_bg[3]
                || p->cm_obj  || p->cm_backdrop;

    /* Per-scanline sprite line buffers. */
    bool     obj_op[PPU_SCREEN_W];
    uint16_t obj_col[PPU_SCREEN_W];
    unsigned obj_prio[PPU_SCREEN_W];

    for (unsigned y = 0; y < PPU_SCREEN_H; y++) {
        /* Effective per-scanline registers (base + this line's HDMA). */
        uint16_t eff_hofs[4], eff_vofs[4];
        unsigned eff_bright = p->brightness;
        for (unsigned i = 0; i < 4; i++) {
            eff_hofs[i] = p->bg[i].hofs;
            eff_vofs[i] = p->bg[i].vofs;
        }
        apply_hdma(p, y, eff_hofs, eff_vofs, &eff_bright);

        render_obj_line(p, y, obj_op, obj_col, obj_prio);
        uint32_t *row = fb + (size_t)y * PPU_SCREEN_W;
        for (unsigned x = 0; x < PPU_SCREEN_W; x++) {
            /* Sample each layer used by either screen once. */
            bool     op[4]  = { false, false, false, false };
            uint16_t col[4] = { 0, 0, 0, 0 };
            unsigned pr[4]  = { 0, 0, 0, 0 };
            for (unsigned l = 0; l < nlayers; l++) {
                if (li[l].bg->on_main || li[l].bg->on_sub)
                    op[l] = bg_sample(p, &li[l], x, y, eff_hofs[l], eff_vofs[l],
                                      &col[l], &pr[l]);
            }

            int ml;
            uint16_t out = composite(ord, norder, op, col, pr,
                                     obj_op[x], obj_col[x], obj_prio[x],
                                     main_en, p->cgram[0], &ml);

            /* Color math: blend the main pixel with the subscreen (or a
             * fixed color) where the winning main layer enables it. */
            if (cm_any) {
                bool cmf = (ml == 4) ? p->cm_obj
                         : (ml == 5) ? p->cm_backdrop
                                     : p->cm_bg[ml];
                if (cmf) {
                    uint16_t operand;
                    if (p->cm_use_subscreen) {
                        int sl;
                        operand = composite(ord, norder, op, col, pr,
                                            obj_op[x], obj_col[x], obj_prio[x],
                                            sub_en, p->cm_fixed_color, &sl);
                    } else {
                        operand = p->cm_fixed_color;
                    }
                    out = color_math(out, operand, p->cm_subtract, p->cm_half);
                }
            }

            row[x] = color_to_fb(out, eff_bright);
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
