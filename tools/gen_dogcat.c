/* ============================================================
 *  gen_dogcat.c — synthesize two 240x200 dual-layer (60-colour) images (a dog and
 *  a cat) for the standalone rolling-buffer + siphon flip test (snes/dogcat_test.s).
 *
 *  Output (raw binaries, .incbin'd by the ROM):
 *    pal.bin       32 words CGRAM 0-31 (hues 16-31, brightness 4-7)
 *    tmap_a.bin    2048 B  parity-A shared BG1+BG3 tilemap (palette field 1)
 *    tmap_b.bin    2048 B  parity-B
 *    dog_bg1.bin  24000 B  750 tiles x 32  (4bpp hue)
 *    dog_bg3.bin  12000 B  750 tiles x 16  (2bpp brightness)
 *    cat_bg1.bin / cat_bg3.bin   likewise
 *
 *  The tilemap overlap math + tile/plane swizzle mirror src/mgapi/copro_r3d.c so the
 *  standalone test exercises the SAME VRAM scheme as the real renderer.
 *
 *  Build:  gcc -O2 -o gen_dogcat gen_dogcat.c
 *  Run  :  ./gen_dogcat <outdir>
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define VW 240
#define VH 200
#define TW (VW/8)          /* 30 */
#define TH (VH/8)          /* 25 */
#define NTILES (TW*TH)     /* 750 */
#define THIRD_TILES 250
#define TMAP_PAL1 0x0400u  /* palette field 1 (bit 10) */
#define BLANK_TILE 255

/* per-pixel: hue 0..15 (0 = backdrop) into BG1, brightness 0..3 into BG3 */
static unsigned char hue[VH][VW];
static unsigned char bri[VH][VW];

static unsigned short bgr555(int r,int g,int b){ /* r,g,b 0..31 */
    return (unsigned short)((b&31)<<10 | (g&31)<<5 | (r&31));
}

/* -------- palette: 16 hues (CGRAM 16-31) + 4 brightness grays (CGRAM 4-7) -------
 * final colour = half-add(hue, bright) = (hue+bright)/2, so hues are full-bright and
 * the gray ramp modulates them into 4 shades -> up to 15*4 = 60 distinct colours. */
static void write_palette(const char *path){
    unsigned short pal[32]; memset(pal,0,sizeof pal);
    static const int H[16][3] = {
        {0,0,0},    /* 0 backdrop (unused hue) */
        {31,6,6},   {31,18,4},  {31,31,6},  {14,31,6},
        {6,31,10},  {6,31,26},  {6,20,31},  {8,8,31},
        {18,6,31},  {31,6,26},  {31,6,14},  {24,16,10},
        {20,20,22}, {31,24,18}, {14,10,6},
    };
    for(int i=0;i<16;i++) pal[16+i]=bgr555(H[i][0],H[i][1],H[i][2]);
    /* brightness ramp: black .. near-white, half-added onto the hue */
    pal[4]=bgr555(0,0,0); pal[5]=bgr555(9,9,9); pal[6]=bgr555(19,19,19); pal[7]=bgr555(31,31,31);
    FILE *f=fopen(path,"wb"); fwrite(pal,2,32,f); fclose(f);
}

/* ---------------- primitive drawing into (hue,bri) ---------------- */
static void px(int x,int y,int h,int b){
    if(x<0||x>=VW||y<0||y>=VH) return; hue[y][x]=(unsigned char)h; bri[y][x]=(unsigned char)b;
}
static void disc(int cx,int cy,int r,int h){
    for(int y=cy-r;y<=cy+r;y++)for(int x=cx-r;x<=cx+r;x++){
        int dx=x-cx,dy=y-cy; double d=sqrt(dx*dx+dy*dy);
        if(d<=r){ /* top-lit shading -> 4 brightness levels */
            double sh=1.0 - (0.55*d/r) - 0.35*((double)(y-(cy-r))/(2*r));
            int b=(int)(sh*3.99); if(b<0)b=0; if(b>3)b=3;
            px(x,y,h,b);
        }
    }
}
static void tri(int x0,int y0,int x1,int y1,int x2,int y2,int h,int b){
    int minx=x0<x1?(x0<x2?x0:x2):(x1<x2?x1:x2), maxx=x0>x1?(x0>x2?x0:x2):(x1>x2?x1:x2);
    int miny=y0<y1?(y0<y2?y0:y2):(y1<y2?y1:y2), maxy=y0>y1?(y0>y2?y0:y2):(y1>y2?y1:y2);
    for(int y=miny;y<=maxy;y++)for(int x=minx;x<=maxx;x++){
        int d0=(x1-x0)*(y-y0)-(y1-y0)*(x-x0);
        int d1=(x2-x1)*(y-y1)-(y2-y1)*(x-x1);
        int d2=(x0-x2)*(y-y2)-(y0-y2)*(x-x2);
        if((d0>=0&&d1>=0&&d2>=0)||(d0<=0&&d1<=0&&d2<=0)) px(x,y,h,b);
    }
}
static void ellipse(int cx,int cy,int rx,int ry,int h,int b){
    for(int y=cy-ry;y<=cy+ry;y++)for(int x=cx-rx;x<=cx+rx;x++){
        double dx=(double)(x-cx)/rx,dy=(double)(y-cy)/ry;
        if(dx*dx+dy*dy<=1.0) px(x,y,h,b);
    }
}

static void clear_bg(int h){
    for(int y=0;y<VH;y++)for(int x=0;x<VW;x++){
        int b=1+(y*3)/VH;                 /* gentle vertical brightness gradient */
        hue[y][x]=(unsigned char)h; bri[y][x]=(unsigned char)b;
    }
}

/* Non-repeating 60-colour plasma background: incommensurate frequencies + an x*y
 * term make (almost) every 8x8 tile unique, so a dropped/misplaced siphon line or a
 * tear shows up ANYWHERE (nothing is masked by flat colour). Identical for both
 * images, so the whole background is re-delivered every frame and MUST stay steady. */
static void pattern_bg(void){
    for(int y=0;y<VH;y++)for(int x=0;x<VW;x++){
        double a = sin(x*0.101) + sin(y*0.133) + sin((x+y)*0.071) + sin((x-y)*0.053);
        double b = cos(x*0.047+y*0.089) + sin(y*0.117) + sin(x*y*0.00063);
        int hue_i = 1 + (int)(fabs(a*30.0 + b*17.0)) % 15;   /* 1..15 */
        int bri_i =     (int)(fabs(b*19.0 + a*11.0 + x*0.5 + y*0.3)) % 4; /* 0..3 */
        hue[y][x]=(unsigned char)hue_i; bri[y][x]=(unsigned char)bri_i;
    }
}

static void draw_dog(void){
    pattern_bg();                         /* busy 60-colour diagnostic background */
    int cx=VW/2, cy=VH/2+8;
    /* floppy ears (behind head) */
    ellipse(cx-58,cy-6,20,40,12,1); ellipse(cx+58,cy-6,20,40,12,1);
    disc(cx,cy,58,3);                     /* head, orange */
    ellipse(cx,cy+28,34,24,2,2);          /* snout, lighter */
    disc(cx,cy+18,9,15);                  /* nose */
    disc(cx-24,cy-14,9,7);  disc(cx+24,cy-14,9,7);   /* eyes cyan */
    disc(cx-24,cy-14,4,0);  disc(cx+24,cy-14,4,0);   /* pupils */
    for(int i=-1;i<=1;i++){ for(int x=0;x<26;x++){ px(cx+18+x,cy+22+i*6,7,3); px(cx-18-x,cy+22+i*6,7,3);} }
}
static void draw_cat(void){
    pattern_bg();                         /* same background as dog (must stay steady) */
    int cx=VW/2, cy=VH/2+6;
    tri(cx-52,cy-56, cx-20,cy-56, cx-40,cy-8, 10,2);  /* left ear */
    tri(cx+52,cy-56, cx+20,cy-56, cx+40,cy-8, 10,2);  /* right ear */
    tri(cx-46,cy-52, cx-26,cy-52, cx-38,cy-16, 1,3);  /* inner ears */
    tri(cx+46,cy-52, cx+26,cy-52, cx+38,cy-16, 1,3);
    disc(cx,cy,54,10);                    /* head, magenta-ish */
    ellipse(cx,cy+22,26,18,14,2);         /* muzzle */
    tri(cx-7,cy+12, cx+7,cy+12, cx,cy+20, 1,0);       /* nose (pink) */
    ellipse(cx-22,cy-10,10,13,3,3); ellipse(cx+22,cy-10,10,13,3,3);  /* eyes yellow */
    ellipse(cx-22,cy-10,3,9,0,0);  ellipse(cx+22,cy-10,3,9,0,0);     /* slit pupils */
    for(int i=-1;i<=1;i++){ for(int x=0;x<34;x++){ px(cx+14+x,cy+20+i*7,7,3); px(cx-14-x,cy+20+i*7,7,3);} }
}

/* ---------------- swizzle (hue,bri) -> BG1 4bpp + BG3 2bpp CHR ---------------- */
static void swizzle(const char *bg1path,const char *bg3path){
    unsigned char *bg1=calloc(NTILES,32), *bg3=calloc(NTILES,16);
    for(int t=0;t<NTILES;t++){
        int tx=(t%TW)*8, ty=(t/TW)*8;
        for(int yy=0;yy<8;yy++)for(int xx=0;xx<8;xx++){
            int h=hue[ty+yy][tx+xx]&15, b=bri[ty+yy][tx+xx]&3, bit=7-xx;
            bg1[t*32+yy*2+0]    |= ((h>>0)&1)<<bit;
            bg1[t*32+yy*2+1]    |= ((h>>1)&1)<<bit;
            bg1[t*32+16+yy*2+0] |= ((h>>2)&1)<<bit;
            bg1[t*32+16+yy*2+1] |= ((h>>3)&1)<<bit;
            bg3[t*16+yy*2+0]    |= ((b>>0)&1)<<bit;
            bg3[t*16+yy*2+1]    |= ((b>>1)&1)<<bit;
        }
    }
    FILE *f;
    f=fopen(bg1path,"wb"); fwrite(bg1,1,NTILES*32,f); fclose(f);
    f=fopen(bg3path,"wb"); fwrite(bg3,1,NTILES*16,f); fclose(f);
    free(bg1); free(bg3);
}

/* ---- shared-tilemap overlap indices (mirror copro_r3d.c tmap_idx_A/B) ---- */
static unsigned tmap_idx_A(int N){
    if(N<THIRD_TILES)     return 512+N;
    if(N<2*THIRD_TILES)   return N-THIRD_TILES;
    return 256+(N-2*THIRD_TILES);
}
static unsigned tmap_idx_B(int N){
    if(N<THIRD_TILES)     return N;
    if(N<2*THIRD_TILES)   return 256+(N-THIRD_TILES);
    return 512+(N-2*THIRD_TILES);
}
static void write_tilemaps(const char *pa,const char *pb){
    unsigned char a[2048], b[2048];
    unsigned short blank=BLANK_TILE|TMAP_PAL1;
    for(int i=0;i<1024;i++){ a[i*2]=blank&255; a[i*2+1]=blank>>8; b[i*2]=blank&255; b[i*2+1]=blank>>8; }
    for(int t=0;t<NTILES;t++){
        int r=t/TW,c=t%TW,cell=r*32+c;
        unsigned short va=tmap_idx_A(t)|TMAP_PAL1, vb=tmap_idx_B(t)|TMAP_PAL1;
        a[cell*2]=va&255; a[cell*2+1]=va>>8; b[cell*2]=vb&255; b[cell*2+1]=vb>>8;
    }
    FILE *f; f=fopen(pa,"wb"); fwrite(a,1,2048,f); fclose(f);
             f=fopen(pb,"wb"); fwrite(b,1,2048,f); fclose(f);
}

int main(int argc,char**argv){
    const char *dir = argc>1?argv[1]:".";
    char a[512],b[512];
    #define J(buf,n) (snprintf(buf,sizeof buf,"%s/%s",dir,n),buf)
    write_palette(J(a,"pal.bin"));
    write_tilemaps(J(a,"tmap_a.bin"), J(b,"tmap_b.bin"));
    draw_dog();  swizzle(J(a,"dog_bg1.bin"), J(b,"dog_bg3.bin"));
    draw_cat();  swizzle(J(a,"cat_bg1.bin"), J(b,"cat_bg3.bin"));
    printf("gen_dogcat: wrote pal/tmap_a/tmap_b + dog/cat bg1/bg3 to %s\n", dir);
    return 0;
}
