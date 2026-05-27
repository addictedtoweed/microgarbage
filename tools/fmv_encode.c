/* fmv_encode.c — HOST FMV encoder (build tool, standalone, libc only).
 *
 * Encodes raw RGB24 frames the way the SNES 4bpp FMV engine wants: 8 palettes
 * of 15 colors + 1 shared backdrop, each 8x8 tile assigned to its best palette,
 * pixels quantized to 4bpp with ordered dither.
 *
 *   gcc -Wall -O2 -o tools/fmv_encode tools/fmv_encode.c
 *   # ...or just use tools/encode_fmv.sh, which builds + runs this for you.
 *   # one frame -> fmv_frame.bin + fmv_preview.ppm:
 *   ./fmv_encode
 *   ./fmv_encode bbb.rgb
 *   # a clip (raw RGB24 stream, W*H*3 per frame) + audio (s16le stereo) -> .fmv:
 *   ffmpeg -i bbb.mp4 -t 6 -vf scale=240:208,fps=20 -f rawvideo -pix_fmt rgb24 bbb.rgb
 *   ffmpeg -i bbb.mp4 -t 6 -vn -ar 44100 -ac 2 -f s16le bbb.pcm
 *   ./fmv_encode bbb.rgb out.fmv bbb.pcm        # omit the .pcm for silent audio
 *   # a synthetic test clip (no source needed; silent audio):
 *   ./fmv_encode synth 40 synth.fmv
 *
 * .fmv (FMV2) = 32B header, then nframes interleaved audio+video units:
 *   header: "FMV2", u16 w,h,fps,audio_channels, u32 nframes, u32 audio_rate,
 *           u16 audio_bits, u16 _, u32 audio_bytes_per_frame, u32 _
 *   unit:   [ audio chunk (audio_bytes_per_frame) | video block (BLOCK) ]
 *           audio first so the real-time mixer gets priority when streamed off
 *           SD: one sequential read per frame demuxes to the mixer + the PPU.
 *   video block = [ CGRAM 256B | tilemap NTILES*2 | CHR NTILES*32 ] = 26776 B.
 *   audio chunk = RATE/FPS sample-frames of s16le stereo = 8820 B @ 44100/20.
 *
 * v1 quantizer: k-means group tiles by average color into 8 palettes, then
 * median-cut each group to 15 colors. Public domain (CC0).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#define W 240
#define H 208
#define TS 8
#define TW (W / TS)
#define TH (H / TS)
#define NTILES (TW * TH)
#define NPAL 8
#define PCOL 15            /* usable colors per palette (1..15); 0 = shared backdrop */
#define DITHER 18
#define FPS 20
#define RATE  44100        /* audio sample rate (CD); must divide evenly by FPS */
#define ACH   2            /* audio channels (stereo) */
#define ABITS 16           /* audio bits/sample (s16le, matches the mixer)      */
#define BLOCK (NPAL*16*2 + NTILES*2 + NTILES*32)   /* 256 + 1560 + 24960 = 26776 */
#define ABYTES (RATE/FPS*ACH*(ABITS/8))            /* audio bytes per video frame = 8820 */
#define HDRSZ 32
_Static_assert(RATE % FPS == 0, "RATE must divide evenly by FPS for an exact A/V interleave");

typedef struct { int r, g, b; } Col;

static Col     img[W * H];
static Col     palette[NPAL][PCOL];
static Col     bg;
static int     tilepal[NTILES];
static Col     tileavg[NTILES];
static uint8_t chr[NTILES][32];
static double  mse;

static const int bayer4[16] = { 0,8,2,10, 12,4,14,6, 3,11,1,9, 15,7,13,5 };

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static long cdist(Col a, Col b) {
    long dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
    return dr*dr + dg*dg + db*db;
}
static void tile_pixels(int t, Col *dst) {
    int tx = (t % TW) * TS, ty = (t / TW) * TS, n = 0;
    for (int y = 0; y < TS; y++)
        for (int x = 0; x < TS; x++) dst[n++] = img[(ty+y)*W + (tx+x)];
}

/* ---- frame sources ---- */
static void synth(int fr) {                         /* animated test frame */
    int bx = W/4 + (fr * 5) % (W/2);                /* orange blob slides across */
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            Col c;
            if (y < H*6/10) { int t = y*255/(H*6/10); c = (Col){90+t/3, 140+t/4, 210-t/5}; }
            else { int t = (y-H*6/10)*255/(H*4/10); c = (Col){40+t/6, 110+t/3, 40+t/8}; }
            int dx = x - bx, dy = y - H/2;
            if (dx*dx + dy*dy < 30*30) c = (Col){200,120,50};
            img[y*W + x] = c;
        }
}
static int read_frame(FILE *f) {
    static unsigned char buf[W*H*3];
    if (fread(buf, 1, sizeof buf, f) != sizeof buf) return 0;
    for (int i = 0; i < W*H; i++) img[i] = (Col){buf[i*3], buf[i*3+1], buf[i*3+2]};
    return 1;
}

/* ---- median cut ---- */
typedef struct { int lo, hi; } Range;
static int mc_axis;
static int mc_cmp(const void *a, const void *b) {
    const Col *p = a, *q = b;
    int pa = mc_axis==0 ? p->r : mc_axis==1 ? p->g : p->b;
    int qa = mc_axis==0 ? q->r : mc_axis==1 ? q->g : q->b;
    return pa - qa;
}
static void box_range(Col *px, int lo, int hi, int *rng, int *amax) {
    int mn[3] = {255,255,255}, mx[3] = {0,0,0};
    for (int j = lo; j < hi; j++) {
        int v[3] = { px[j].r, px[j].g, px[j].b };
        for (int a = 0; a < 3; a++) { if (v[a]<mn[a]) mn[a]=v[a]; if (v[a]>mx[a]) mx[a]=v[a]; }
    }
    *rng = (mx[0]-mn[0]) + (mx[1]-mn[1]) + (mx[2]-mn[2]);
    *amax = (mx[0]-mn[0] >= mx[1]-mn[1] && mx[0]-mn[0] >= mx[2]-mn[2]) ? 0
          : (mx[1]-mn[1] >= mx[2]-mn[2]) ? 1 : 2;
}
static void median_cut(Col *px, int n, int k, Col *out) {
    if (n <= 0) { for (int i = 0; i < k; i++) out[i] = (Col){0,0,0}; return; }
    Range box[256]; int nb = 1; box[0] = (Range){0, n};
    while (nb < k) {
        int best = -1; long bestr = -1;
        for (int i = 0; i < nb; i++) {
            if (box[i].hi - box[i].lo < 2) continue;
            int rng, ax; box_range(px, box[i].lo, box[i].hi, &rng, &ax);
            if (rng > bestr) { bestr = rng; best = i; }
        }
        if (best < 0) break;
        int rng, ax; box_range(px, box[best].lo, box[best].hi, &rng, &ax);
        mc_axis = ax; qsort(px + box[best].lo, box[best].hi - box[best].lo, sizeof(Col), mc_cmp);
        int mid = (box[best].lo + box[best].hi) / 2;
        Range b2 = { mid, box[best].hi };
        box[best].hi = mid; box[nb++] = b2;
    }
    for (int i = 0; i < k; i++) {
        if (i < nb && box[i].hi > box[i].lo) {
            long sr=0,sg=0,sb=0; int lo=box[i].lo, hi=box[i].hi;
            for (int j=lo;j<hi;j++){ sr+=px[j].r; sg+=px[j].g; sb+=px[j].b; }
            int cnt = hi-lo; out[i] = (Col){(int)(sr/cnt),(int)(sg/cnt),(int)(sb/cnt)};
        } else out[i] = (Col){0,0,0};
    }
}

/* ---- quantize the current img into palette/bg/tilepal/chr (+ mse) ---- */
static void quantize(void) {
    bg = (Col){0, 0, 0};        /* black backdrop: free black letterbox + true black shadows */

    for (int t=0;t<NTILES;t++){ Col px[64]; tile_pixels(t,px);
        long ar=0,ag=0,ab=0; for(int i=0;i<64;i++){ar+=px[i].r;ag+=px[i].g;ab+=px[i].b;}
        tileavg[t]=(Col){(int)(ar/64),(int)(ag/64),(int)(ab/64)}; }

    Col cen[NPAL];
    for (int c=0;c<NPAL;c++) cen[c]=tileavg[c*NTILES/NPAL];
    for (int it=0; it<10; it++) {
        for (int t=0;t<NTILES;t++){ long bd=-1; int bc=0;
            for(int c=0;c<NPAL;c++){ long d=cdist(tileavg[t],cen[c]); if(bd<0||d<bd){bd=d;bc=c;} }
            tilepal[t]=bc; }
        for (int c=0;c<NPAL;c++){ long cr=0,cg=0,cb=0; int n=0;
            for(int t=0;t<NTILES;t++) if(tilepal[t]==c){cr+=tileavg[t].r;cg+=tileavg[t].g;cb+=tileavg[t].b;n++;}
            if(n) cen[c]=(Col){(int)(cr/n),(int)(cg/n),(int)(cb/n)}; }
    }

    static Col pool[NTILES*64];
    for (int c=0;c<NPAL;c++){ int n=0;
        for(int t=0;t<NTILES;t++) if(tilepal[t]==c){ Col px[64]; tile_pixels(t,px); memcpy(pool+n,px,64*sizeof(Col)); n+=64; }
        median_cut(pool,n,PCOL,palette[c]); }

    mse = 0;
    for (int t=0;t<NTILES;t++){ int p=tilepal[t], tx=(t%TW)*TS, ty=(t/TW)*TS;
        memset(chr[t],0,32);
        for(int yy=0;yy<TS;yy++) for(int xx=0;xx<TS;xx++){
            Col c=img[(ty+yy)*W+(tx+xx)];
            int d=(bayer4[((ty+yy)&3)*4+((tx+xx)&3)]-8)*DITHER/8;
            Col cd={clampi(c.r+d,0,255),clampi(c.g+d,0,255),clampi(c.b+d,0,255)};
            int bi=0; long bd=cdist(cd,bg);
            for(int k=0;k<PCOL;k++){ long dd=cdist(cd,palette[p][k]); if(dd<bd){bd=dd;bi=k+1;} }
            Col q=(bi==0)?bg:palette[p][bi-1]; mse+=cdist(q,c);
            int row=yy, bit=7-xx;
            chr[t][row*2+0]      |= ((bi>>0)&1)<<bit;
            chr[t][row*2+1]      |= ((bi>>1)&1)<<bit;
            chr[t][16+row*2+0]   |= ((bi>>2)&1)<<bit;
            chr[t][16+row*2+1]   |= ((bi>>3)&1)<<bit;
        } }
    mse /= (double)(W*H*3);
}

/* ---- output ---- */
static void write_block(FILE *o) {
    for (int p=0;p<NPAL;p++) for (int c=0;c<16;c++){
        Col col = (c==0)?bg:palette[p][c-1];
        uint16_t bgr=(uint16_t)((col.r>>3)|((col.g>>3)<<5)|((col.b>>3)<<10));
        fputc(bgr&0xFF,o); fputc(bgr>>8,o);
    }
    for (int t=0;t<NTILES;t++){ uint16_t e=(uint16_t)t|(uint16_t)(tilepal[t]<<10); fputc(e&0xFF,o); fputc(e>>8,o); }
    fwrite(chr,1,sizeof chr,o);
}
static void write_preview(const char *path) {
    FILE *pp=fopen(path,"wb"); if(!pp){ perror(path); return; }
    fprintf(pp,"P6\n%d %d\n255\n",W,H);
    static unsigned char out[W*H*3];
    for (int t=0;t<NTILES;t++){ int p=tilepal[t], tx=(t%TW)*TS, ty=(t/TW)*TS;
        for(int yy=0;yy<TS;yy++) for(int xx=0;xx<TS;xx++){
            Col c=img[(ty+yy)*W+(tx+xx)];
            int d=(bayer4[((ty+yy)&3)*4+((tx+xx)&3)]-8)*DITHER/8;
            Col cd={clampi(c.r+d,0,255),clampi(c.g+d,0,255),clampi(c.b+d,0,255)};
            int bi=0; long bd=cdist(cd,bg);
            for(int k=0;k<PCOL;k++){ long dd=cdist(cd,palette[p][k]); if(dd<bd){bd=dd;bi=k+1;} }
            Col q=(bi==0)?bg:palette[p][bi-1];
            int oo=((ty+yy)*W+(tx+xx))*3; out[oo]=q.r; out[oo+1]=q.g; out[oo+2]=q.b;
        } }
    fwrite(out,1,W*H*3,pp); fclose(pp);
}
static void w16(FILE *f, unsigned v){ fputc(v&0xFF,f); fputc((v>>8)&0xFF,f); }
static void w32(FILE *f, unsigned v){ fputc(v&0xFF,f); fputc((v>>8)&0xFF,f); fputc((v>>16)&0xFF,f); fputc((v>>24)&0xFF,f); }
static void hdr(FILE *o, unsigned nframes){     /* 32-byte FMV2 header */
    fwrite("FMV2",1,4,o);
    w16(o,W); w16(o,H); w16(o,FPS); w16(o,ACH);
    w32(o,nframes);                             /* offset 12, patched at end */
    w32(o,RATE); w16(o,ABITS); w16(o,0);
    w32(o,ABYTES); w32(o,0);
}
/* one frame's worth of audio: ABYTES from af (or silence if af==NULL/short),
   written before its video block so the mixer leads when streamed. */
static void write_audio(FILE *o, FILE *af){
    static unsigned char abuf[ABYTES];
    size_t got = af ? fread(abuf,1,ABYTES,af) : 0;
    if (got < (size_t)ABYTES) memset(abuf+got, 0, ABYTES-got);
    fwrite(abuf,1,ABYTES,o);
}

int main(int argc, char **argv) {
    if (argc >= 4 && !strcmp(argv[1], "synth")) {       /* synth N out.fmv (silent audio) */
        int n = atoi(argv[2]); FILE *o = fopen(argv[3], "wb"); if (!o) { perror(argv[3]); return 1; }
        hdr(o, n);
        for (int i = 0; i < n; i++) { synth(i); quantize(); write_audio(o,NULL); write_block(o); if (i==0) write_preview("fmv_preview.ppm"); }
        fclose(o);
        printf("synth: %d frames -> %s  (%d+%d B/frame, total %ld B, %.1fs @ %dfps, silent)\n",
               n, argv[3], ABYTES, BLOCK, (long)HDRSZ + (long)n*(ABYTES+BLOCK), (double)n/FPS, FPS);
        return 0;
    }
    if (argc >= 3) {            /* VIDEO(-=stdin) out.fmv [AUDIO.pcm] [-n N] */
        FILE *in;
        if (!strcmp(argv[1], "-")) {
#ifdef _WIN32
            _setmode(_fileno(stdin), _O_BINARY);        /* raw bytes, not text mode */
#endif
            in = stdin;
        } else { in = fopen(argv[1], "rb"); if (!in) { perror(argv[1]); return 1; } }
        FILE *o = fopen(argv[2], "wb"); if (!o) { perror(argv[2]); return 1; }
        FILE *af = NULL; int maxf = (1<<30);
        for (int i = 3; i < argc; i++) {                /* optional audio path and/or -n N */
            if (!strcmp(argv[i], "-n") && i+1 < argc) maxf = atoi(argv[++i]);
            else if (strcmp(argv[i], "none") != 0) { af = fopen(argv[i], "rb"); if (!af) perror(argv[i]); }
        }
        hdr(o, 0);                                      /* nframes patched at end */
        int nf = 0;
        while (nf < maxf && read_frame(in)) { quantize(); write_audio(o,af); write_block(o); if (nf==0) write_preview("fmv_preview.ppm"); nf++; }
        fseek(o, 12, SEEK_SET); w32(o, nf);
        fclose(o); if (in != stdin) fclose(in); if (af) fclose(af);
        printf("encoded %d frames -> %s  (%d+%d B/frame, total %ld B, %.1fs @ %dfps, audio %s)\n",
               nf, argv[2], ABYTES, BLOCK, (long)HDRSZ + (long)nf*(ABYTES+BLOCK), (double)nf/FPS, FPS,
               af ? "embedded" : "silent");
        return 0;
    }

    /* no args (or single .rgb): one frame -> fmv_frame.bin + preview */
    if (argc == 2) { FILE *in = fopen(argv[1], "rb"); if (in && read_frame(in)) { fclose(in); printf("loaded %s\n", argv[1]); }
                     else { synth(0); printf("synthetic frame\n"); } }
    else { synth(0); printf("synthetic frame\n"); }
    quantize();
    FILE *o = fopen("fmv_frame.bin", "wb"); if (o) { write_block(o); fclose(o); } else perror("fmv_frame.bin");
    write_preview("fmv_preview.ppm");
    int dpv = 54*1364/8, vb = (BLOCK + dpv - 1)/dpv;
    printf("tiles=%d  block=%d B  -> %d vblanks = %d fps  |  MSE/ch=%.1f  | wrote fmv_frame.bin, fmv_preview.ppm\n",
           NTILES, BLOCK, vb, 60/vb, mse);
    return 0;
}
