/* ============================================================
 *  gen_probe_dog.c — synthesize a FLAT-colour cartoon dog for the force-blank
 *  rendering-timing probe (snes/siphon_probe.s).
 *
 *  Plain SINGLE-LAYER 16-colour 4bpp (NOT the dogcat dual-layer hue/brightness
 *  split). Flat high-contrast regions so a force-blanked line shows as an obvious
 *  black gap and a stale/mis-fetched line shows as an obvious broken feature.
 *
 *  Image is 256x224 (full SNES screen) = 32x28 tiles. Tiles are DE-DUPLICATED
 *  (flat regions collapse to a handful of unique tiles) so the CHR fits the
 *  probe's ~32 KB CODE budget.
 *
 *  Output (raw binaries, .incbin'd by the ROM), written to <outdir>:
 *    probe_dog.pal   32 B    16 BGR555 words (CGRAM 0..15)
 *    probe_dog.chr   N*32 B  N unique 4bpp tiles (N printed at the end)
 *    probe_dog.map   2048 B  32x32 tilemap (28 rows used), tile index only
 *                            (palette 0, priority 0)
 *
 *  Build: gcc -O2 -o gen_probe_dog gen_probe_dog.c -lm
 *  Run  : ./gen_probe_dog <outdir>
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VW 256
#define VH 224
#define TW (VW/8)          /* 32 */
#define TH (VH/8)          /* 28 */

static unsigned char img[VH][VW];   /* palette index 0..15 per pixel */

static unsigned short bgr555(int r,int g,int b){ /* 0..31 each */
    return (unsigned short)(((b&31)<<10)|((g&31)<<5)|(r&31));
}

/* 16-colour FLAT palette (index 0 = backdrop/unused; the dog fills BG1 fully) */
static const int PAL[16][3] = {
    { 0, 0, 0},   /* 0  black backdrop (unused by the image) */
    {14,22,31},   /* 1  sky blue    (background) */
    {31,22,10},   /* 2  tan         (head/body)  */
    {17,10, 4},   /* 3  brown       (ears)       */
    {31,31,31},   /* 4  white       (muzzle/eye whites) */
    { 1, 1, 1},   /* 5  near-black  (nose/pupils/outline) */
    {31, 5, 6},   /* 6  red         (tongue)     */
    { 6,10,31},   /* 7  blue        (collar)     */
    { 8,24, 8},   /* 8  grass green (ground)     */
    {31,16, 7},   /* 9  dark tan    (head shade rim) */
    {24,26,31},   /* 10 pale blue   (spare)      */
    {20,12, 6},   /* 11 spare brown */
    {31,28,14},   /* 12 spare       */
    {12,12,14},   /* 13 spare grey  */
    {31,24,18},   /* 14 spare       */
    {10, 8, 6},   /* 15 spare       */
};

/* ---------------- flat primitives ---------------- */
static void px(int x,int y,int c){
    if(x<0||x>=VW||y<0||y>=VH) return; img[y][x]=(unsigned char)c;
}
static void fill(int c){ for(int y=0;y<VH;y++)for(int x=0;x<VW;x++) img[y][x]=(unsigned char)c; }
static void rect(int x0,int y0,int x1,int y1,int c){
    for(int y=y0;y<=y1;y++)for(int x=x0;x<=x1;x++) px(x,y,c);
}
static void disc(int cx,int cy,int r,int c){
    for(int y=cy-r;y<=cy+r;y++)for(int x=cx-r;x<=cx+r;x++){
        int dx=x-cx,dy=y-cy; if(dx*dx+dy*dy<=r*r) px(x,y,c);
    }
}
static void ellipse(int cx,int cy,int rx,int ry,int c){
    for(int y=cy-ry;y<=cy+ry;y++)for(int x=cx-rx;x<=cx+rx;x++){
        double dx=(double)(x-cx)/rx,dy=(double)(y-cy)/ry;
        if(dx*dx+dy*dy<=1.0) px(x,y,c);
    }
}

static void draw_dog(void){
    int cx=VW/2, cy=VH/2+6;
    fill(1);                                 /* sky-blue background */
    rect(0, VH-34, VW-1, VH-1, 8);           /* grass ground band */

    /* floppy ears (behind head) — brown */
    ellipse(cx-72, cy-6, 26, 54, 3);
    ellipse(cx+72, cy-6, 26, 54, 3);

    disc(cx, cy, 82, 9);                     /* head rim (dark tan) */
    disc(cx, cy, 78, 2);                     /* head (tan) */

    ellipse(cx, cy+36, 46, 32, 4);           /* muzzle (white) */

    /* eyes: white sclera + black pupil */
    disc(cx-32, cy-20, 15, 4);
    disc(cx+32, cy-20, 15, 4);
    disc(cx-30, cy-16, 7, 5);
    disc(cx+30, cy-16, 7, 5);

    disc(cx, cy+20, 14, 5);                  /* nose (black) */
    ellipse(cx, cy+58, 12, 18, 6);           /* tongue (red) */

    /* mouth lines (black), simple */
    rect(cx-1, cy+30, cx+1, cy+52, 5);

    rect(cx-64, cy+74, cx+64, cy+88, 7);     /* collar (blue) */
    disc(cx, cy+81, 6, 12);                  /* collar tag */
}

/* ---------------- swizzle one 8x8 cell -> 32-byte 4bpp planar tile --------- */
static void swizzle_tile(int tx,int ty,unsigned char out[32]){
    memset(out,0,32);
    for(int yy=0;yy<8;yy++)for(int xx=0;xx<8;xx++){
        int c=img[ty+yy][tx+xx]&15, bit=7-xx;
        out[yy*2+0]    |= ((c>>0)&1)<<bit;
        out[yy*2+1]    |= ((c>>1)&1)<<bit;
        out[16+yy*2+0] |= ((c>>2)&1)<<bit;
        out[16+yy*2+1] |= ((c>>3)&1)<<bit;
    }
}

int main(int argc,char**argv){
    const char *dir = argc>1?argv[1]:".";
    char path[512];
    #define J(n) (snprintf(path,sizeof path,"%s/%s",dir,n),path)

    draw_dog();

    /* palette */
    unsigned short pal[16];
    for(int i=0;i<16;i++) pal[i]=bgr555(PAL[i][0],PAL[i][1],PAL[i][2]);
    FILE *f=fopen(J("probe_dog.pal"),"wb"); fwrite(pal,2,16,f); fclose(f);

    /* de-duplicated CHR + 32x32 tilemap */
    static unsigned char uniq[1024][32];
    int nuniq=0;
    unsigned short map[32*32];
    memset(map,0,sizeof map);
    for(int r=0;r<TH;r++)for(int c=0;c<TW;c++){
        unsigned char t[32];
        swizzle_tile(c*8,r*8,t);
        int idx=-1;
        for(int u=0;u<nuniq;u++){ if(!memcmp(uniq[u],t,32)){ idx=u; break; } }
        if(idx<0){ idx=nuniq; memcpy(uniq[nuniq],t,32); nuniq++; }
        map[r*32+c]=(unsigned short)idx;   /* palette 0, priority 0 */
    }

    f=fopen(J("probe_dog.chr"),"wb"); fwrite(uniq,32,nuniq,f); fclose(f);
    f=fopen(J("probe_dog.map"),"wb"); fwrite(map,2,32*32,f); fclose(f);

    printf("gen_probe_dog: %d unique tiles (%d bytes CHR), map 2048 B, pal 32 B -> %s\n",
           nuniq, nuniq*32, dir);
    if(nuniq*16 > 0x3000)
        fprintf(stderr,"WARNING: CHR uses %d words (> $3000); font base must move.\n", nuniq*16);
    return 0;
}
