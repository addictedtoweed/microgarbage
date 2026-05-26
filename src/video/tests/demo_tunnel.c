/* demo_tunnel.c — MANUAL visual PoC: HDMA per-scanline "road" canyon.
 *
 * A different, hardware-native approach to the canyon than the per-pixel
 * raycaster (demo_fly3d): NO per-pixel work, so no resampling crawl, full
 * 256x224, and dirt cheap. A single tiled BG holds a canyon texture (a
 * lava channel down the centre, rock to the sides); HDMA drives the BG's
 * scroll PER SCANLINE:
 *   - VOFS[y] = a depth gradient (near rows scroll fast, far rows slow) =>
 *     perspective + forward rush as cam_z advances (the classic road effect).
 *   - HOFS[y] = the course curve at that depth => the canyon snakes.
 * Above the horizon, the BG shows a transparent row so the violet backdrop
 * (later: a mountain/Pompeii background layer) shows through.
 *
 * This is the cart code path: the MCU builds two ~224-entry HDMA tables per
 * frame; the SNES PPU does the rest. Banking (Mode 2 offset-per-tile) and
 * the background layer are follow-ups.
 *
 * Build + run (Windows):
 *   cc -Wall -Wextra -Wpedantic -std=c11 -Iinclude -o build/demo_tunnel \
 *      src/video/ppu.c src/video/present_gl_win32.c \
 *      src/video/tests/demo_tunnel.c -lopengl32 -lgdi32 -luser32
 *
 * Headless render check (no GL): add -DTUNNEL_HEADLESS, link only ppu.c.
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#ifndef TUNNEL_HEADLESS
#include "video/present.h"
#include <windows.h>
#endif
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define BGR555(r,g,b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

#define CHAR_WORD 0x1000u      /* VRAM word base of BG1 tiles    */
#define TMAP_WORD 0x0000u      /* VRAM word base of BG1 tilemap  */
#define HOR  60                /* horizon screen row (sky above) */
#define PK   2600.0f           /* perspective scale              */

static PpuState P;
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
static uint16_t hofs[PPU_SCREEN_H], vofs[PPU_SCREEN_H];

/* ---- libm-free sine (parabolic approx) --------------------- */
static float fsin(float x) {
    const float PI = 3.14159265f, TWO_PI = 6.28318531f;
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    float ax = (x < 0.0f) ? -x : x;
    return 1.27323954f * x - 0.405284735f * x * ax;
}

/* Write one 4bpp 8x8 tile (idx 0..15 per pixel) at tile slot `t`. */
static void put_tile(unsigned t, uint8_t idx[8][8]) {
    unsigned base = CHAR_WORD + t * 16u;
    for (int row = 0; row < 8; row++) {
        unsigned bp0 = 0, bp1 = 0, bp2 = 0, bp3 = 0;
        for (int col = 0; col < 8; col++) {
            unsigned px = idx[row][col], bit = (unsigned)(7 - col);
            bp0 |= ((px >> 0) & 1u) << bit;
            bp1 |= ((px >> 1) & 1u) << bit;
            bp2 |= ((px >> 2) & 1u) << bit;
            bp3 |= ((px >> 3) & 1u) << bit;
        }
        P.vram[(base + (unsigned)row) & 0x7FFFu]        = (uint16_t)(bp0 | (bp1 << 8));
        P.vram[(base + 8u + (unsigned)row) & 0x7FFFu]   = (uint16_t)(bp2 | (bp3 << 8));
    }
}

static void build_scene(void) {
    ppu_state_clear(&P);
    P.mode = 1;
    P.brightness = 15;

    /* palette (BG1 uses palette 0 = cgram[0..15]); [0] also = backdrop */
    P.cgram[0]  = BGR555(28, 22, 31);                 /* violet sky / transparent */
    P.cgram[2]  = BGR555(7, 6, 6);   P.cgram[3] = BGR555(10, 9, 8);   /* rock */
    P.cgram[4]  = BGR555(13, 12, 10); P.cgram[5] = BGR555(16, 15, 13);
    P.cgram[8]  = BGR555(31, 16, 2);  P.cgram[9] = BGR555(31, 23, 7); /* lava */
    P.cgram[10] = BGR555(27, 9, 1);   P.cgram[11] = BGR555(31, 28, 11);

    /* tiles: 0 transparent, 1 rock, 2 lava (horizontal bands for motion) */
    uint8_t tr[8][8], rock[8][8], lava[8][8];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            tr[y][x]   = 0;
            rock[y][x] = (uint8_t)(3 + ((y >> 1) & 1));                          /* clean rock bands 3..4 */
            lava[y][x] = (uint8_t)(8 + (((y >> 1) & 1) << 1) + ((x + y) & 1));   /* lava glows 8..11 */
        }
    put_tile(0, tr); put_tile(1, rock); put_tile(2, lava);

    /* tilemap 32x32: row 0 transparent; rows 1..31 = lava channel (centre
     * cols) flanked by rock, repeating down -> a continuous channel. */
    for (unsigned ty = 0; ty < 32; ty++)
        for (unsigned tx = 0; tx < 32; tx++) {
            unsigned tile = (ty == 0) ? 0u : ((tx >= 14u && tx <= 17u) ? 2u : 1u);
            P.vram[(TMAP_WORD + ty * 32u + tx) & 0x7FFFu] = (uint16_t)tile;
        }

    P.bg[0].tilemap_word = TMAP_WORD;
    P.bg[0].char_word    = CHAR_WORD;
    P.bg[0].size         = PPU_SC_32x32;
    P.bg[0].hofs = 0; P.bg[0].vofs = 0;
    P.bg[0].on_main = true;

    P.hdma[0].target = PPU_REG_BG1_HOFS; P.hdma[0].value = hofs;
    P.hdma[1].target = PPU_REG_BG1_VOFS; P.hdma[1].value = vofs;
    P.hdma_count = 2;
}

/* Per-frame HDMA tables: the road/perspective effect. */
static void fill_tables(float cam_z) {
    for (int y = 0; y < PPU_SCREEN_H; y++) {
        if (y <= HOR) {                                /* sky: show transparent texture row 0 */
            vofs[y] = (uint16_t)(0 - y);
            hofs[y] = 0;
            continue;
        }
        float p    = (float)(y - HOR);                 /* 1..(223-HOR), larger = nearer */
        float dist = PK / p;                           /* far near the horizon, near at the bottom */
        int   tex  = 8 + ((int)(cam_z + dist) % 248);  /* canyon texture rows [8,255] */
        vofs[y]    = (uint16_t)(tex - y);              /* (y+vofs)&255 == tex */
        /* smooth snake: gentle bend vs depth (capped so the compressed far
         * rows don't oscillate), drifting slowly as we advance. */
        float cd    = dist > 700.0f ? 700.0f : dist;
        int   curve = (int)(40.0f * fsin(cam_z * 0.010f + cd * 0.0016f));
        hofs[y]     = (uint16_t)curve;
    }
}

#ifdef TUNNEL_HEADLESS
int main(void) {
    build_scene();
    fill_tables(120.0f);
    ppu_render(&P, FB);
    for (int y = 0; y < PPU_SCREEN_H; y += 8) {
        for (int x = 0; x < PPU_SCREEN_W; x += 4) {
            uint32_t c = FB[y * PPU_SCREEN_W + x];
            unsigned r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
            char ch;
            if (b > 180 && r > 150) ch = ' ';                       /* violet sky */
            else if (r > 150 && g < 150 && b < 90) ch = 'L';        /* lava */
            else { unsigned lum = (r + g + b) / 3; ch = lum < 60 ? '.' : lum < 110 ? ':' : '#'; }
            putchar(ch);
        }
        putchar('\n');
    }
    return 0;
}
#else
static double now_sec(void) {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}
int main(void) {
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - HDMA tunnel")) return 1;
    printf("GL renderer : %s\n", present_gl_renderer());
    printf("HDMA per-scanline road canyon (no raycast). Full 256x224.\n");
    fflush(stdout);

    build_scene();
    double t0 = now_sec(), report = t0;
    unsigned frames = 0;
    while (!present_should_close()) {
        float cam_z = (float)((now_sec() - t0) * 220.0);   /* forward speed */
        fill_tables(cam_z);
        ppu_render(&P, FB);
        present_frame(FB);
        frames++;
        double now = now_sec();
        if (now - report >= 1.0) {
            printf("  %.1f fps\n", (double)frames / (now - report));
            fflush(stdout); report = now; frames = 0;
        }
    }
    present_shutdown();
    return 0;
}
#endif
