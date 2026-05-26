/* fmv_encode.c — HOST FMV encoder (build tool, standalone, libc only).
 *
 * Takes a raw RGB24 frame (W*H*3 bytes) and encodes it the way the SNES 4bpp
 * FMV engine wants it: 8 palettes of 15 colors + 1 shared backdrop, each 8x8
 * tile assigned to its best palette, pixels quantized to 4bpp (with ordered
 * dither). Writes the per-frame block (CGRAM + tilemap + CHR), prints the
 * byte/vblank budget, the quantization error, and a preview .ppm (the
 * re-rendered result) so the quality can be eyeballed.
 *
 *   gcc -Wall -O2 -o fmv_encode src/video/tests/fmv_encode.c
 *   ffmpeg -i bbb.mp4 -vf scale=240:208 -frames:v 1 -f rawvideo -pix_fmt rgb24 bbb.rgb
 *   ./fmv_encode bbb.rgb           (no arg -> synthetic test frame)
 *
 * v1 quantizer: k-means group tiles by average colour into 8 palettes, then
 * median-cut each group to 15 colours. Iterative re-assignment by per-tile
 * error is the quality upgrade. Public domain (CC0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define W 240
#define H 208
#define TS 8
#define TW (W / TS)        /* 30 */
#define TH (H / TS)        /* 26 */
#define NTILES (TW * TH)   /* 780 */
#define NPAL 8
#define PCOL 15            /* usable colours per palette (1..15); 0 = shared backdrop */
#define DITHER 18          /* ordered-dither strength */

typedef struct { int r, g, b; } Col;

static Col img[W * H];

static const int bayer4[16] = { 0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static long cdist(Col a, Col b) {
    long dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr*dr + dg*dg + db*db;
}

/* ---- synthetic test frame (sky gradient, grass, a couple of blobs) ---- */
static void synth(void) {
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            Col c;
            if (y < H * 6 / 10) {                 /* sky: blue gradient */
                int t = y * 255 / (H * 6 / 10);
                c.r = 90 + t/3; c.g = 140 + t/4; c.b = 210 - t/5;
            } else {                              /* grass: green gradient */
                int t = (y - H*6/10) * 255 / (H*4/10);
                c.r = 40 + t/6; c.g = 110 + t/3; c.b = 40 + t/8;
            }
            int dx = x - W/3, dy = y - H/2;       /* an orange blob */
            if (dx*dx + dy*dy < 34*34) { c.r = 200; c.g = 120; c.b = 50; }
            int ex = x - 2*W/3, ey = y - 2*H/3;   /* a red flower */
            if (ex*ex + ey*ey < 12*12) { c.r = 210; c.g = 40; c.b = 60; }
            img[y*W + x] = c;
        }
}
static int load_rgb(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char *buf = malloc(W * H * 3);
    size_t n = fread(buf, 1, W * H * 3, f);
    fclose(f);
    if (n != (size_t)(W*H*3)) { free(buf); return 0; }
    for (int i = 0; i < W*H; i++) { img[i].r = buf[i*3]; img[i].g = buf[i*3+1]; img[i].b = buf[i*3+2]; }
    free(buf);
    return 1;
}

/* ---- median cut: reduce a pixel list to k representative colours ---- */
typedef struct { int lo, hi; } Range;
static Col *mc_px; static int mc_axis;
static int mc_cmp(const void *a, const void *b) {
    const Col *p = a, *q = b;
    int pa = mc_axis == 0 ? p->r : mc_axis == 1 ? p->g : p->b;
    int qa = mc_axis == 0 ? q->r : mc_axis == 1 ? q->g : q->b;
    return pa - qa;
}
static void median_cut(Col *px, int n, int k, Col *out) {
    if (n <= 0) { for (int i = 0; i < k; i++) out[i] = (Col){0,0,0}; return; }
    Range box[256]; int nb = 1; box[0] = (Range){0, n};
    while (nb < k) {
        /* pick the box with the largest colour spread */
        int best = -1; long bestlen = -1;
        for (int i = 0; i < nb; i++) {
            int lo = box[i].lo, hi = box[i].hi;
            if (hi - lo < 2) continue;
            int rmin=255,rmax=0,gmin=255,gmax=0,bmin=255,bmax=0;
            for (int j = lo; j < hi; j++) {
                Col c = px[j];
                if (c.r < rmin) rmin = c.r;
                if (c.r > rmax) rmax = c.r;
                if (c.g < gmin) gmin = c.g;
                if (c.g > gmax) gmax = c.g;
                if (c.b < bmin) bmin = c.b;
                if (c.b > bmax) bmax = c.b;
            }
            long len = (rmax-rmin) + (gmax-gmin) + (bmax-bmin);
            if (len > bestlen) { bestlen = len; best = i; }
        }
        if (best < 0) break;
        int lo = box[best].lo, hi = box[best].hi;
        int rmin=255,rmax=0,gmin=255,gmax=0,bmin=255,bmax=0;
        for (int j = lo; j < hi; j++) {
            Col c = px[j];
            if (c.r < rmin) rmin = c.r;
            if (c.r > rmax) rmax = c.r;
            if (c.g < gmin) gmin = c.g;
            if (c.g > gmax) gmax = c.g;
            if (c.b < bmin) bmin = c.b;
            if (c.b > bmax) bmax = c.b;
        }
        mc_axis =(rmax-rmin >= gmax-gmin && rmax-rmin >= bmax-bmin) ? 0
                : (gmax-gmin >= bmax-bmin) ? 1 : 2;
        mc_px = px; qsort(px + lo, hi - lo, sizeof(Col), mc_cmp);
        int mid = (lo + hi) / 2;
        box[best] = (Range){lo, mid};
        box[nb++] = (Range){mid, hi};
    }
    for (int i = 0; i < k; i++) {
        if (i < nb && box[i].hi > box[i].lo) {
            long sr=0,sg=0,sb=0; int lo=box[i].lo, hi=box[i].hi;
            for (int j = lo; j < hi; j++) { sr+=px[j].r; sg+=px[j].g; sb+=px[j].b; }
            int cnt = hi - lo;
            out[i] = (Col){ (int)(sr/cnt), (int)(sg/cnt), (int)(sb/cnt) };
        } else out[i] = (Col){0,0,0};
    }
}

static Col palette[NPAL][PCOL];
static Col bg;
static int tilepal[NTILES];
static Col tileavg[NTILES];

static void tile_pixels(int t, Col *dst) {          /* gather a tile's 64 pixels */
    int tx = (t % TW) * TS, ty = (t / TW) * TS, n = 0;
    for (int y = 0; y < TS; y++)
        for (int x = 0; x < TS; x++) dst[n++] = img[(ty+y)*W + (tx+x)];
}

int main(int argc, char **argv) {
    if (argc > 1) {
        if (!load_rgb(argv[1])) { fprintf(stderr, "can't read %s as %dx%d RGB24\n", argv[1], W, H); return 1; }
        printf("loaded %s (%dx%d)\n", argv[1], W, H);
    } else { synth(); printf("synthetic test frame (%dx%d)\n", W, H); }

    /* backdrop = global average */
    { long sr=0,sg=0,sb=0; for (int i=0;i<W*H;i++){sr+=img[i].r;sg+=img[i].g;sb+=img[i].b;}
      bg = (Col){(int)(sr/(W*H)),(int)(sg/(W*H)),(int)(sb/(W*H))}; }

    /* tile average colours */
    for (int t = 0; t < NTILES; t++) {
        Col px[64]; tile_pixels(t, px);
        long sr=0,sg=0,sb=0; for (int i=0;i<64;i++){sr+=px[i].r;sg+=px[i].g;sb+=px[i].b;}
        tileavg[t] = (Col){(int)(sr/64),(int)(sg/64),(int)(sb/64)};
    }

    /* k-means group tiles into NPAL clusters by average colour */
    Col cen[NPAL];
    for (int c = 0; c < NPAL; c++) cen[c] = tileavg[c * NTILES / NPAL];
    for (int it = 0; it < 10; it++) {
        for (int t = 0; t < NTILES; t++) {
            long bestd = -1; int bestc = 0;
            for (int c = 0; c < NPAL; c++) { long d = cdist(tileavg[t], cen[c]); if (bestd<0||d<bestd){bestd=d;bestc=c;} }
            tilepal[t] = bestc;
        }
        for (int c = 0; c < NPAL; c++) {
            long sr=0,sg=0,sb=0; int n=0;
            for (int t=0;t<NTILES;t++) if (tilepal[t]==c){sr+=tileavg[t].r;sg+=tileavg[t].g;sb+=tileavg[t].b;n++;}
            if (n) cen[c] = (Col){(int)(sr/n),(int)(sg/n),(int)(sb/n)};
        }
    }

    /* per cluster: median-cut its pixels to PCOL colours */
    static Col pool[NTILES * 64];
    for (int c = 0; c < NPAL; c++) {
        int n = 0;
        for (int t = 0; t < NTILES; t++) if (tilepal[t] == c) { Col px[64]; tile_pixels(t, px); memcpy(pool+n, px, 64*sizeof(Col)); n += 64; }
        median_cut(pool, n, PCOL, palette[c]);
    }

    /* quantize -> 4bpp indices, with ordered dither; build CHR + measure error */
    static uint8_t chr[NTILES][32];
    double mse = 0;
    for (int t = 0; t < NTILES; t++) {
        int p = tilepal[t], tx = (t%TW)*TS, ty = (t/TW)*TS;
        memset(chr[t], 0, 32);
        for (int yy = 0; yy < TS; yy++)
            for (int xx = 0; xx < TS; xx++) {
                Col c = img[(ty+yy)*W + (tx+xx)];
                int d = (bayer4[((ty+yy)&3)*4 + ((tx+xx)&3)] - 8) * DITHER / 8;
                Col cd = { clampi(c.r+d,0,255), clampi(c.g+d,0,255), clampi(c.b+d,0,255) };
                int bi = 0; long bd = cdist(cd, bg);            /* index 0 = backdrop */
                for (int k = 0; k < PCOL; k++) { long dd = cdist(cd, palette[p][k]); if (dd < bd) { bd = dd; bi = k+1; } }
                Col q = (bi == 0) ? bg : palette[p][bi-1];
                mse += cdist(q, c);
                /* pack 4bpp planar (SNES): rows 0-7 planes0,1 then planes2,3 */
                int row = yy, bit = 7 - xx;
                chr[t][row*2 + 0]  |= ((bi>>0)&1) << bit;
                chr[t][row*2 + 1]  |= ((bi>>1)&1) << bit;
                chr[t][16 + row*2 + 0] |= ((bi>>2)&1) << bit;
                chr[t][16 + row*2 + 1] |= ((bi>>3)&1) << bit;
            }
    }
    mse /= (double)(W * H * 3);

    /* ---- write preview PPM (re-rendered) ---- */
    FILE *pp = fopen("fmv_preview.ppm", "wb");
    if (pp) {
        fprintf(pp, "P6\n%d %d\n255\n", W, H);
        /* re-decode from indices: redo quantization decision to get the colour */
        static unsigned char out[W*H*3];
        for (int t = 0; t < NTILES; t++) {
            int p = tilepal[t], tx=(t%TW)*TS, ty=(t/TW)*TS;
            for (int yy=0; yy<TS; yy++) for (int xx=0; xx<TS; xx++) {
                Col c = img[(ty+yy)*W+(tx+xx)];
                int d = (bayer4[((ty+yy)&3)*4+((tx+xx)&3)] - 8) * DITHER / 8;
                Col cd = { clampi(c.r+d,0,255), clampi(c.g+d,0,255), clampi(c.b+d,0,255) };
                int bi=0; long bd=cdist(cd,bg);
                for (int k=0;k<PCOL;k++){ long dd=cdist(cd,palette[p][k]); if(dd<bd){bd=dd;bi=k+1;} }
                Col q = (bi==0)?bg:palette[p][bi-1];
                int o = ((ty+yy)*W + (tx+xx))*3; out[o]=q.r; out[o+1]=q.g; out[o+2]=q.b;
            }
        }
        fwrite(out, 1, W*H*3, pp); fclose(pp);
    }

    /* ---- write the encoded frame block: CGRAM, tilemap, CHR ---- */
    FILE *fo = fopen("fmv_frame.bin", "wb");
    if (fo) {
        for (int p = 0; p < NPAL; p++)                  /* CGRAM: 8 palettes x 16, color0 = shared bg */
            for (int c = 0; c < 16; c++) {
                Col col = (c == 0) ? bg : palette[p][c-1];
                uint16_t bgr = (uint16_t)((col.r>>3) | ((col.g>>3)<<5) | ((col.b>>3)<<10));
                fputc(bgr & 0xFF, fo); fputc(bgr >> 8, fo);
            }
        for (int t = 0; t < NTILES; t++) {              /* tilemap: raster index | palette<<10 */
            uint16_t e = (uint16_t)t | (uint16_t)(tilepal[t] << 10);
            fputc(e & 0xFF, fo); fputc(e >> 8, fo);
        }
        fwrite(chr, 1, sizeof chr, fo);                 /* CHR: 4bpp tiles */
        fclose(fo);
    }

    /* ---- byte budget ---- */
    int chr_b = NTILES * 32, tmap_b = NTILES * 2, cgram_b = NPAL * 16 * 2;
    int total = chr_b + tmap_b + cgram_b;
    int dma_per_vblank = 54 * 1364 / 8;                 /* 240x208 letterbox window */
    int vblanks = (total + dma_per_vblank - 1) / dma_per_vblank;
    printf("tiles=%d  CHR=%d B  tilemap=%d B  CGRAM=%d B  total=%d B\n",
           NTILES, chr_b, tmap_b, cgram_b, total);
    printf("DMA window=%d B/vblank  ->  %.2f vblanks/frame  -> %d vblanks = %.0f fps\n",
           dma_per_vblank, (double)total / dma_per_vblank, vblanks, 60.0 / vblanks);
    printf("colour MSE/channel = %.1f  (lower is better; 0 = lossless)\n", mse);
    printf("wrote fmv_preview.ppm + fmv_frame.bin (%d-byte block)\n", total);
    return 0;
}
