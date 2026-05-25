/* test_ppu.c — headless tests for the PPU rasterizer.
 *
 * Builds known PPU states, renders to a framebuffer, and asserts exact
 * pixel values. No display needed (renders to memory), so this runs in
 * run_tests.sh.
 *
 *   cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
 *      -o build/tests/ppu src/video/tests/test_ppu.c src/video/ppu.c
 *
 * Public domain (CC0). No warranty.
 */
#include "test_runner.h"
#include "video/ppu.h"
#include "video/present.h"

#include <stdint.h>
#include <string.h>

/* Test colors (BGR555) and their rendered RGBA at full brightness. */
#define RED555    0x001Fu
#define GREEN555  0x03E0u
#define BLUE555   0x7C00u
#define WHITE555  0x7FFFu

static PpuState  P;
static uint32_t  FB[PPU_SCREEN_W * PPU_SCREEN_H];

static uint32_t px(int x, int y) { return FB[(size_t)y * PPU_SCREEN_W + x]; }

static void fill8(uint8_t p[8][8], uint8_t v) {
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) p[r][c] = v;
}

static void set_tile_4bpp(uint16_t *vram, unsigned cw, unsigned tile,
                          uint8_t p[8][8]) {
    unsigned base = (cw + tile * 16u) & 0x7FFFu;
    for (unsigned r = 0; r < 8; r++) {
        unsigned p0 = 0, p1 = 0, p2 = 0, p3 = 0;
        for (unsigned x = 0; x < 8; x++) {
            unsigned v = p[r][x], b = 7u - x;
            p0 |= (v & 1u) << b;       p1 |= ((v >> 1) & 1u) << b;
            p2 |= ((v >> 2) & 1u) << b; p3 |= ((v >> 3) & 1u) << b;
        }
        vram[(base + r) & 0x7FFFu]      = (uint16_t)(p0 | (p1 << 8));
        vram[(base + 8u + r) & 0x7FFFu] = (uint16_t)(p2 | (p3 << 8));
    }
}

static void set_tile_2bpp(uint16_t *vram, unsigned cw, unsigned tile,
                          uint8_t p[8][8]) {
    unsigned base = (cw + tile * 8u) & 0x7FFFu;
    for (unsigned r = 0; r < 8; r++) {
        unsigned p0 = 0, p1 = 0;
        for (unsigned x = 0; x < 8; x++) {
            unsigned v = p[r][x], b = 7u - x;
            p0 |= (v & 1u) << b; p1 |= ((v >> 1) & 1u) << b;
        }
        vram[(base + r) & 0x7FFFu] = (uint16_t)(p0 | (p1 << 8));
    }
}

static void set_map(uint16_t *vram, unsigned tmw, unsigned tx, unsigned ty,
                    uint16_t entry) {
    vram[(tmw + ty * 32u + tx) & 0x7FFFu] = entry;
}

/* A standard one-4bpp-BG Mode 1 setup: BG1 on, tilemap @0x1000,
 * char @0x2000, palette colors loaded. Caller adds tiles + map. */
static void setup_mode1_bg1(void) {
    ppu_state_clear(&P);
    P.mode = 1;
    P.bg[0].on_main = true;
    P.bg[0].tilemap_word = 0x1000;
    P.bg[0].char_word    = 0x2000;
    P.bg[0].size = PPU_SC_32x32;
    P.cgram[0] = 0;            /* backdrop = black */
    P.cgram[5] = RED555;       /* pal0 index 5 */
    P.cgram[6] = GREEN555;     /* pal0 index 6 */
}

/* Write OAM entry i (low + high tables). x is signed (-256..255). */
static void set_sprite(uint8_t *oam, unsigned i, int x, unsigned y, unsigned tnum,
                       unsigned pal, unsigned prio, bool hf, bool vf, bool big) {
    oam[i * 4 + 0] = (uint8_t)((unsigned)x & 0xFFu);
    oam[i * 4 + 1] = (uint8_t)y;
    oam[i * 4 + 2] = (uint8_t)(tnum & 0xFFu);
    unsigned attr = ((tnum >> 8) & 1u) | ((pal & 7u) << 1) | ((prio & 3u) << 4)
                  | (hf ? 0x40u : 0u) | (vf ? 0x80u : 0u);
    oam[i * 4 + 3] = (uint8_t)attr;

    unsigned hb = 512u + (i >> 2);
    unsigned sh = (i & 3u) * 2u;
    unsigned xhi = ((unsigned)x >> 8) & 1u;
    unsigned bits = (xhi & 1u) | ((big ? 1u : 0u) << 1);
    oam[hb] = (uint8_t)((oam[hb] & ~(3u << sh)) | (bits << sh));
}

/* Sprites-only Mode 1 setup: OBJ on, BGs off, OBJ palette loaded. */
static void setup_obj(unsigned size_sel) {
    ppu_state_clear(&P);
    P.mode = 1;
    P.obj_on_main   = true;
    P.obj_size_sel  = (uint8_t)size_sel;
    P.obj_char_word = 0x4000;
    P.obj_gap_word  = 0;
    P.cgram[0]       = 0;          /* backdrop black */
    P.cgram[128 + 5] = RED555;     /* OBJ pal0 index 5 */
    P.cgram[128 + 6] = GREEN555;   /* OBJ pal0 index 6 */
}

/* ---- tests ------------------------------------------------- */

static void test_bgr555_conversion(void) {
    ASSERT_EQ_INT((long long)PRESENT_RGBA(255, 0, 0), (long long)ppu_bgr555_to_rgba(RED555));
    ASSERT_EQ_INT((long long)PRESENT_RGBA(0, 255, 0), (long long)ppu_bgr555_to_rgba(GREEN555));
    ASSERT_EQ_INT((long long)PRESENT_RGBA(0, 0, 255), (long long)ppu_bgr555_to_rgba(BLUE555));
    ASSERT_EQ_INT((long long)PRESENT_RGBA(255, 255, 255), (long long)ppu_bgr555_to_rgba(WHITE555));
    ASSERT_EQ_INT((long long)PRESENT_RGBA(0, 0, 0), (long long)ppu_bgr555_to_rgba(0));
}

static void test_basic_4bpp_tile(void) {
    setup_mode1_bg1();
    uint8_t pat[8][8]; fill8(pat, 5);             /* solid pixel value 5 */
    set_tile_4bpp(P.vram, 0x2000, 1, pat);
    set_map(P.vram, 0x1000, 0, 0, 1);             /* (0,0) -> tile 1 */
    ppu_render(&P, FB);

    uint32_t red = ppu_bgr555_to_rgba(RED555);
    uint32_t blk = ppu_bgr555_to_rgba(0);
    ASSERT_EQ_INT((long long)red, (long long)px(0, 0));
    ASSERT_EQ_INT((long long)red, (long long)px(7, 7));
    ASSERT_EQ_INT((long long)red, (long long)px(3, 4));
    ASSERT_EQ_INT((long long)blk, (long long)px(8, 0));     /* next tile (0) transparent -> backdrop */
    ASSERT_EQ_INT((long long)blk, (long long)px(100, 100)); /* backdrop */
}

static void test_horizontal_scroll(void) {
    setup_mode1_bg1();
    uint8_t pat[8][8]; fill8(pat, 5);
    set_tile_4bpp(P.vram, 0x2000, 1, pat);
    set_map(P.vram, 0x1000, 0, 0, 1);             /* two red tiles wide */
    set_map(P.vram, 0x1000, 1, 0, 1);
    P.bg[0].hofs = 8;                             /* scroll right by one tile */
    ppu_render(&P, FB);

    uint32_t red = ppu_bgr555_to_rgba(RED555);
    uint32_t blk = ppu_bgr555_to_rgba(0);
    ASSERT_EQ_INT((long long)red, (long long)px(0, 0));   /* sees bg x=8 (tile col 1, red) */
    ASSERT_EQ_INT((long long)red, (long long)px(7, 0));
    ASSERT_EQ_INT((long long)blk, (long long)px(8, 0));   /* bg x=16 -> tile col 2 (transparent) */
}

static void test_hflip(void) {
    setup_mode1_bg1();
    uint8_t pat[8][8]; fill8(pat, 0);
    for (int r = 0; r < 8; r++) pat[r][0] = 5;    /* red only in column 0 */
    set_tile_4bpp(P.vram, 0x2000, 1, pat);

    uint32_t red = ppu_bgr555_to_rgba(RED555);
    uint32_t blk = ppu_bgr555_to_rgba(0);

    set_map(P.vram, 0x1000, 0, 0, 1);             /* no flip */
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)red, (long long)px(0, 0));
    ASSERT_EQ_INT((long long)blk, (long long)px(7, 0));

    set_map(P.vram, 0x1000, 0, 0, (uint16_t)(1 | (1u << 14))); /* hflip */
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)blk, (long long)px(0, 0));
    ASSERT_EQ_INT((long long)red, (long long)px(7, 0));
}

static void test_vflip(void) {
    setup_mode1_bg1();
    uint8_t pat[8][8]; fill8(pat, 0);
    for (int c = 0; c < 8; c++) pat[0][c] = 5;    /* red only in row 0 */
    set_tile_4bpp(P.vram, 0x2000, 1, pat);

    uint32_t red = ppu_bgr555_to_rgba(RED555);
    uint32_t blk = ppu_bgr555_to_rgba(0);

    set_map(P.vram, 0x1000, 0, 0, 1);             /* no flip */
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)red, (long long)px(0, 0));
    ASSERT_EQ_INT((long long)blk, (long long)px(0, 7));

    set_map(P.vram, 0x1000, 0, 0, (uint16_t)(1 | (1u << 15))); /* vflip */
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)blk, (long long)px(0, 0));
    ASSERT_EQ_INT((long long)red, (long long)px(0, 7));
}

static void test_two_layer_transparency(void) {
    setup_mode1_bg1();
    /* BG2 behind BG1: where BG1 is transparent, BG2 shows. */
    P.bg[1].on_main = true;
    P.bg[1].tilemap_word = 0x1100;
    P.bg[1].char_word    = 0x3000;
    P.bg[1].size = PPU_SC_32x32;

    uint8_t bg1[8][8]; fill8(bg1, 0);
    for (int r = 0; r < 8; r++) for (int c = 0; c < 4; c++) bg1[r][c] = 5; /* left half red */
    set_tile_4bpp(P.vram, 0x2000, 1, bg1);
    set_map(P.vram, 0x1000, 0, 0, 1);

    uint8_t bg2[8][8]; fill8(bg2, 6);            /* solid green (value 6) */
    set_tile_4bpp(P.vram, 0x3000, 1, bg2);
    set_map(P.vram, 0x1100, 0, 0, 1);

    ppu_render(&P, FB);
    uint32_t red   = ppu_bgr555_to_rgba(RED555);
    uint32_t green = ppu_bgr555_to_rgba(GREEN555);
    ASSERT_EQ_INT((long long)red,   (long long)px(0, 0)); /* BG1 opaque wins */
    ASSERT_EQ_INT((long long)green, (long long)px(5, 0)); /* BG1 transparent -> BG2 */
}

static void test_priority_bit(void) {
    setup_mode1_bg1();
    /* BG1 prio 0 (red) vs BG2 prio 1 (green), both opaque at (0,0).
     * Mode 1 puts BG2-prio1 above BG1-prio0, so green must win. */
    P.bg[1].on_main = true;
    P.bg[1].tilemap_word = 0x1100;
    P.bg[1].char_word    = 0x3000;

    uint8_t solid5[8][8]; fill8(solid5, 5);
    set_tile_4bpp(P.vram, 0x2000, 1, solid5);
    set_map(P.vram, 0x1000, 0, 0, 1);            /* BG1 prio 0 */

    uint8_t solid6[8][8]; fill8(solid6, 6);
    set_tile_4bpp(P.vram, 0x3000, 1, solid6);
    set_map(P.vram, 0x1100, 0, 0, (uint16_t)(1 | (1u << 13))); /* BG2 prio 1 */

    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)ppu_bgr555_to_rgba(GREEN555), (long long)px(0, 0));
}

static void test_2bpp_bg3(void) {
    ppu_state_clear(&P);
    P.mode = 1;
    P.bg[2].on_main = true;                       /* BG3 = 2bpp in mode 1 */
    P.bg[2].tilemap_word = 0x1000;
    P.bg[2].char_word    = 0x2000;
    P.cgram[0] = 0;
    P.cgram[2] = BLUE555;                         /* pal0 (2bpp) index 2 */

    uint8_t pat[8][8]; fill8(pat, 2);             /* solid value 2 */
    set_tile_2bpp(P.vram, 0x2000, 1, pat);
    set_map(P.vram, 0x1000, 0, 0, 1);
    ppu_render(&P, FB);

    ASSERT_EQ_INT((long long)ppu_bgr555_to_rgba(BLUE555), (long long)px(0, 0));
    ASSERT_EQ_INT((long long)ppu_bgr555_to_rgba(0),       (long long)px(8, 0));
}

static void test_mode0_cgram_bands(void) {
    /* Mode 0: all BGs are 2bpp and each draws from its own 32-entry
     * CGRAM band (BG1:0, BG2:32, BG3:64, BG4:96). Prove BG2 uses base 32. */
    ppu_state_clear(&P);
    P.mode = 0;
    P.bg[1].on_main = true;                       /* BG2 */
    P.bg[1].tilemap_word = 0x1000;
    P.bg[1].char_word    = 0x2000;
    P.cgram[0] = 0;
    P.cgram[33] = GREEN555;                        /* band 32 + pal0*4 + value1 */

    uint8_t pat[8][8]; fill8(pat, 1);              /* solid value 1 */
    set_tile_2bpp(P.vram, 0x2000, 1, pat);
    set_map(P.vram, 0x1000, 0, 0, 1);
    ppu_render(&P, FB);

    ASSERT_EQ_INT((long long)ppu_bgr555_to_rgba(GREEN555), (long long)px(0, 0));
    ASSERT_EQ_INT((long long)ppu_bgr555_to_rgba(0),        (long long)px(100, 100));
}

static void test_forced_blank(void) {
    setup_mode1_bg1();
    uint8_t pat[8][8]; fill8(pat, 5);
    set_tile_4bpp(P.vram, 0x2000, 1, pat);
    set_map(P.vram, 0x1000, 0, 0, 1);
    P.forced_blank = true;
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)PRESENT_RGBA(0, 0, 0), (long long)px(0, 0)); /* black despite red tile */
}

static void test_brightness_half(void) {
    setup_mode1_bg1();
    uint8_t pat[8][8]; fill8(pat, 5);
    set_tile_4bpp(P.vram, 0x2000, 1, pat);
    set_map(P.vram, 0x1000, 0, 0, 1);
    P.brightness = 7;                             /* ~half of 15 */
    ppu_render(&P, FB);
    /* red 255 scaled: 255*7/15 = 119 */
    ASSERT_EQ_INT((long long)PRESENT_RGBA(119, 0, 0), (long long)px(0, 0));
}

static void test_sprite_basic(void) {
    setup_obj(0);                                  /* 8x8 small */
    uint8_t pat[8][8]; fill8(pat, 5);
    set_tile_4bpp(P.vram, 0x4000, 1, pat);
    set_sprite(P.oam, 0, 10, 20, 1, 0, 0, false, false, false);
    ppu_render(&P, FB);

    uint32_t red = ppu_bgr555_to_rgba(RED555);
    uint32_t blk = ppu_bgr555_to_rgba(0);
    ASSERT_EQ_INT((long long)red, (long long)px(10, 20));   /* top-left */
    ASSERT_EQ_INT((long long)red, (long long)px(17, 27));   /* bottom-right (8x8) */
    ASSERT_EQ_INT((long long)blk, (long long)px(18, 20));   /* just right */
    ASSERT_EQ_INT((long long)blk, (long long)px(10, 28));   /* just below */
    ASSERT_EQ_INT((long long)blk, (long long)px(9, 20));    /* just left */
}

static void test_sprite_hflip(void) {
    setup_obj(0);
    uint8_t pat[8][8]; fill8(pat, 0);
    for (int r = 0; r < 8; r++) pat[r][0] = 5;     /* red only in column 0 */
    set_tile_4bpp(P.vram, 0x4000, 1, pat);

    uint32_t red = ppu_bgr555_to_rgba(RED555);
    uint32_t blk = ppu_bgr555_to_rgba(0);

    set_sprite(P.oam, 0, 10, 20, 1, 0, 0, false, false, false);
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)red, (long long)px(10, 20));
    ASSERT_EQ_INT((long long)blk, (long long)px(17, 20));

    set_sprite(P.oam, 0, 10, 20, 1, 0, 0, true, false, false); /* hflip */
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)blk, (long long)px(10, 20));
    ASSERT_EQ_INT((long long)red, (long long)px(17, 20));       /* red now rightmost */
}

static void test_sprite_16x16(void) {
    setup_obj(0);                                  /* pair 0: small 8x8 / large 16x16 */
    uint8_t pat[8][8]; fill8(pat, 5);
    /* 16x16 = 2x2 cells: base_lo + tr*16 + tc -> 1, 2, 17, 18 */
    set_tile_4bpp(P.vram, 0x4000, 1, pat);
    set_tile_4bpp(P.vram, 0x4000, 2, pat);
    set_tile_4bpp(P.vram, 0x4000, 17, pat);
    set_tile_4bpp(P.vram, 0x4000, 18, pat);
    set_sprite(P.oam, 0, 10, 20, 1, 0, 0, false, false, true);  /* big */
    ppu_render(&P, FB);

    uint32_t red = ppu_bgr555_to_rgba(RED555);
    uint32_t blk = ppu_bgr555_to_rgba(0);
    ASSERT_EQ_INT((long long)red, (long long)px(10, 20));   /* TL */
    ASSERT_EQ_INT((long long)red, (long long)px(25, 35));   /* BR (16x16) */
    ASSERT_EQ_INT((long long)blk, (long long)px(26, 20));   /* just right */
    ASSERT_EQ_INT((long long)blk, (long long)px(10, 36));   /* just below */
}

static void test_sprite_negative_x(void) {
    setup_obj(0);
    uint8_t pat[8][8]; fill8(pat, 5);
    set_tile_4bpp(P.vram, 0x4000, 1, pat);
    set_sprite(P.oam, 0, -4, 20, 1, 0, 0, false, false, false); /* 4px off the left */
    ppu_render(&P, FB);

    uint32_t red = ppu_bgr555_to_rgba(RED555);
    uint32_t blk = ppu_bgr555_to_rgba(0);
    ASSERT_EQ_INT((long long)red, (long long)px(0, 20));   /* visible part */
    ASSERT_EQ_INT((long long)red, (long long)px(3, 20));   /* last visible col (-4+7) */
    ASSERT_EQ_INT((long long)blk, (long long)px(4, 20));   /* past the sprite */
}

static void test_sprite_lowest_index_wins(void) {
    setup_obj(0);
    uint8_t r5[8][8]; fill8(r5, 5);  set_tile_4bpp(P.vram, 0x4000, 1, r5); /* red */
    uint8_t r6[8][8]; fill8(r6, 6);  set_tile_4bpp(P.vram, 0x4000, 2, r6); /* green */
    set_sprite(P.oam, 0, 10, 20, 1, 0, 0, false, false, false); /* sprite 0: red */
    set_sprite(P.oam, 1, 10, 20, 2, 0, 0, false, false, false); /* sprite 1: green */
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)ppu_bgr555_to_rgba(RED555), (long long)px(10, 20)); /* lower index in front */
}

static void test_sprite_vs_bg_priority(void) {
    ppu_state_clear(&P);
    P.mode = 1;
    /* BG1 (green) */
    P.bg[0].on_main = true;
    P.bg[0].tilemap_word = 0x1000;
    P.bg[0].char_word    = 0x2000;
    P.cgram[0] = 0;
    P.cgram[6] = GREEN555;                          /* BG pal0 index 6 */
    uint8_t g[8][8]; fill8(g, 6); set_tile_4bpp(P.vram, 0x2000, 1, g);
    /* OBJ (red) */
    P.obj_on_main = true; P.obj_size_sel = 0; P.obj_char_word = 0x4000;
    P.cgram[128 + 5] = RED555;
    uint8_t r[8][8]; fill8(r, 5); set_tile_4bpp(P.vram, 0x4000, 1, r);

    uint32_t red = ppu_bgr555_to_rgba(RED555);
    uint32_t green = ppu_bgr555_to_rgba(GREEN555);

    /* sprite prio 2 over BG1 prio 0 -> sprite wins */
    set_map(P.vram, 0x1000, 0, 0, 1);                          /* BG1 prio 0 */
    set_sprite(P.oam, 0, 0, 0, 1, 0, 2, false, false, false);  /* sprite prio 2 */
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)red, (long long)px(0, 0));

    /* sprite prio 0 under BG1 prio 1 -> BG wins */
    set_map(P.vram, 0x1000, 0, 0, (uint16_t)(1 | (1u << 13))); /* BG1 prio 1 */
    set_sprite(P.oam, 0, 0, 0, 1, 0, 0, false, false, false);  /* sprite prio 0 */
    ppu_render(&P, FB);
    ASSERT_EQ_INT((long long)green, (long long)px(0, 0));
}

int main(void) {
    TEST_SUITE("ppu");
    RUN(test_bgr555_conversion);
    RUN(test_basic_4bpp_tile);
    RUN(test_horizontal_scroll);
    RUN(test_hflip);
    RUN(test_vflip);
    RUN(test_two_layer_transparency);
    RUN(test_priority_bit);
    RUN(test_2bpp_bg3);
    RUN(test_mode0_cgram_bands);
    RUN(test_forced_blank);
    RUN(test_brightness_half);
    RUN(test_sprite_basic);
    RUN(test_sprite_hflip);
    RUN(test_sprite_16x16);
    RUN(test_sprite_negative_x);
    RUN(test_sprite_lowest_index_wins);
    RUN(test_sprite_vs_bg_priority);
    return TEST_SUITE_RESULT();
}
