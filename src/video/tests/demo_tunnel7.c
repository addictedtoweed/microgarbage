/* demo_tunnel7.c — MANUAL PoC: Mode 7 perspective canyon (F-Zero style).
 *
 * The tiled per-scanline approach (demo_tunnel) bends + scrolls but a tiled
 * BG can't SCALE per line, so the channel never narrows -> no convergence.
 * Mode 7 can: per scanline the matrix X-scale a ∝ 1/(y-horizon), so the
 * ground (and the lava channel) genuinely converges to a vanishing point.
 *
 * Per scanline (below the horizon): z = DEPTH/(y-HOR) (forward distance);
 *   M7A   = z*256/FOCAL   (x-scale; far rows huge => features compressed)
 *   M7VOFS= cam_z + z - y  (places the plane forward-coord at this depth)
 *   M7HOFS= curve(cam_z+z) (the channel snakes)
 * Constants: M7D=256, M7B=M7C=0, centre (M7X,M7Y)=(128,0). Above the
 * horizon a=0 + vofs->a transparent plane row => violet sky backdrop.
 *
 * The MCU builds three ~224-entry HDMA tables/frame; the SNES PPU does the
 * rest. No per-pixel work, full 256x224, no crawl. (Rich mountain/Pompeii
 * background needs a tiled layer above the horizon — a mode-split follow-up.)
 *
 * Build + run (Windows):
 *   cc ... -o build/demo_tunnel7 src/video/ppu.c src/video/present_gl_win32.c \
 *      src/video/tests/demo_tunnel7.c -lopengl32 -lgdi32 -luser32
 * Headless render check: add -DTUNNEL_HEADLESS, link only ppu.c.
 *
 * Public domain (CC0). No warranty.
 */
#include "video/ppu.h"
#ifndef TUNNEL_HEADLESS
#include "video/present.h"
#include <windows.h>
#endif
#include <stdint.h>
#include <stdio.h>

#define BGR555(r,g,b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

#define HOR     70          /* horizon screen row                         */
#define DEPTH   14000.0f    /* z = DEPTH / (y - HOR)                       */
#define FOCAL   90.0f       /* screen->plane focal (x-scale = z*256/FOCAL) */
#define A_MAX   5000        /* beyond this x-scale the channel is sub-pixel: cut to sky */
#define VSCALE  0.14f       /* compress depth into the plane V so the far point fits */
#define SCROLLW 168         /* cam_z wrap (keeps far ty < 1024; multiple of band period) */
#define SKY_V   4           /* transparent plane row sampled above horizon */
#define CHAN_U  128         /* plane U of the lava channel (centre)        */

static PpuState P;
static uint32_t FB[PPU_SCREEN_W * PPU_SCREEN_H];
static uint16_t a_tab[PPU_SCREEN_H], hofs_tab[PPU_SCREEN_H], vofs_tab[PPU_SCREEN_H];

static float fsin(float x) {
    const float PI = 3.14159265f, TWO_PI = 6.28318531f;
    while (x >  PI) x -= TWO_PI;
    while (x < -PI) x += TWO_PI;
    float ax = (x < 0.0f) ? -x : x;
    return 1.27323954f * x - 0.405284735f * x * ax;
}

/* Mode 7 VRAM is interleaved: word w holds tilemap entry (low byte) AND
 * char pixel (high byte). Fill both for w in [0, 0x3FFF]. */
static void build_scene(void) {
    ppu_state_clear(&P);
    P.mode = 7;
    P.brightness = 15;

    P.cgram[0] = BGR555(28, 22, 31);                  /* violet sky / transparent */
    P.cgram[2] = BGR555(9, 8, 7);  P.cgram[3] = BGR555(13, 12, 10);  /* rock */
    P.cgram[8] = BGR555(31, 17, 3); P.cgram[9] = BGR555(31, 24, 9);  /* lava */

    for (unsigned w = 0; w < 0x4000u; w++) {
        /* tilemap cell (tu,tv): top 2 tile-rows transparent; a lava-tile
         * column at the centre (the channel) running down; rock elsewhere. */
        unsigned tu = w & 127u, tv = (w >> 7) & 127u;
        unsigned tile;
        if (tv < 2u)                     tile = 0u;   /* far transparent band       */
        else if (tu >= 13u && tu <= 18u) tile = 2u;   /* WIDER lava channel (centre)*/
        else if (tu >= 7u  && tu <= 24u) tile = 1u;   /* rock walls flanking it     */
        else                             tile = 0u;   /* sky beyond the canyon      */

        /* char pixel for char-word w: tile w/64, pixel (w%8, (w/8)%8). */
        unsigned ct = w >> 6, py = (w >> 3) & 7u;
        unsigned idx = (ct == 0u) ? 0u                       /* transparent  */
                     : (ct == 2u) ? (8u + (py & 1u))         /* lava bands   */
                     :              (2u + (py & 1u));         /* rock bands   */
        P.vram[w] = (uint16_t)((tile & 0xFFu) | ((idx & 0xFFu) << 8));
    }

    P.m7d = 256; P.m7b = 0; P.m7c = 0;                /* d=1.0; no shear */
    P.m7x = CHAN_U; P.m7y = 0;                        /* centre: plane U at screen centre */
    P.m7_over_transparent = true;                     /* sky beyond the plane (no wrap) */

    P.hdma[0].target = PPU_REG_M7A;    P.hdma[0].value = a_tab;
    P.hdma[1].target = PPU_REG_M7HOFS; P.hdma[1].value = hofs_tab;
    P.hdma[2].target = PPU_REG_M7VOFS; P.hdma[2].value = vofs_tab;
    P.hdma_count = 3;
}

static void fill_tables(float cam_z) {
    for (int y = 0; y < PPU_SCREEN_H; y++) {
        if (y <= HOR) {                               /* sky: a=0 -> sample plane (CHAN_U, SKY_V) */
            a_tab[y]    = 0;
            hofs_tab[y] = 0;
            vofs_tab[y] = (uint16_t)(int16_t)(SKY_V - y);
            continue;
        }
        float p = (float)(y - HOR);
        float z = DEPTH / p;                          /* no clamp: each row a distinct scale =>
                                                       * smooth narrowing (rows past the plane
                                                       * become sky via transparent-outside) */
        int a = (int)(z * 256.0f / FOCAL);            /* x-scale (8.8); big far => convergence */
        if (a > A_MAX) {                              /* sub-pixel channel: this is the horizon,
                                                       * show sky instead of a 1px sliver "wall" */
            a_tab[y] = 0; hofs_tab[y] = 0;
            vofs_tab[y] = (uint16_t)(int16_t)(SKY_V - y);
            continue;
        }
        a_tab[y]    = (uint16_t)(int16_t)a;
        int ty      = (int)(cam_z + z * VSCALE);      /* compress depth into the plane V */
        vofs_tab[y] = (uint16_t)(int16_t)(ty - y);
        float cd    = z > 1200.0f ? 1200.0f : z;      /* cap the CURVE distance so the far rows
                                                       * (where z lurches) share one stable bend */
        int curve   = (int)(70.0f * fsin((cam_z + cd) * 0.0009f));
        hofs_tab[y] = (uint16_t)(int16_t)curve;       /* dx = sx + hofs - 128 -> channel snakes */
    }
}

#ifdef TUNNEL_HEADLESS
int main(void) {
    build_scene();
    fill_tables(200.0f);
    ppu_render(&P, FB);
    for (int y = 0; y < PPU_SCREEN_H; y += 7) {
        for (int x = 0; x < PPU_SCREEN_W; x += 4) {
            uint32_t c = FB[y * PPU_SCREEN_W + x];
            unsigned r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF;
            char ch;
            if (b > 180 && r > 150) ch = ' ';
            else if (r > 150 && g < 150 && b < 90) ch = 'L';
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
    if (!present_init(PPU_SCREEN_W, PPU_SCREEN_H, "microgarbage - Mode 7 tunnel")) return 1;
    printf("GL: %s\nMode 7 perspective canyon (converging). Full 256x224.\n",
           present_gl_renderer());
    fflush(stdout);

    build_scene();
    double t0 = now_sec(), report = t0;
    unsigned frames = 0;
    while (!present_should_close()) {
        float cam_z = (float)((now_sec() - t0) * 90.0);
        while (cam_z >= (float)SCROLLW) cam_z -= (float)SCROLLW;   /* seamless band-scroll loop */
        fill_tables(cam_z);
        ppu_render(&P, FB);
        present_frame(FB);
        frames++;
        double now = now_sec();
        if (now - report >= 1.0) { printf("  %.1f fps\n", (double)frames / (now - report));
            fflush(stdout); report = now; frames = 0; }
    }
    present_shutdown();
    return 0;
}
#endif
