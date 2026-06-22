/* ============================================================
 *  fmv_video_stream.c — FMV_VIDEO stream producer. See header.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "fmv_video_stream.h"

#include "cart_frame_types.h"     /* MgCompleteFrame, StagedSlot, mg_bg_sc, mg_chr_page */
#include "vm/vm_host_fs.h"        /* vm_host_fs_route_read */

#include <stdlib.h>
#include <string.h>

/* --- FMV2 layout (must match tools/fmv_encode.c + demo_fmv.c) --- */
#define FMV_TW          30u                 /* tiles wide  (240/8)  */
#define FMV_TH          26u                 /* tiles tall  (208/8)  */
#define FMV_NTILES      (FMV_TW * FMV_TH)   /* 780                  */
#define FMV_BLANK_TILE  FMV_NTILES          /* 780 — backdrop tile  */
#define FMV_CGRAM_BYTES 256u                /* 128 BGR555 entries   */
#define FMV_TM_SRC_BYTES (FMV_NTILES * 2u)  /* 1560 raw cells       */
#define FMV_CHR_BYTES   (FMV_NTILES * 32u)  /* 24960                */

/* Double-buffered VRAM layout (demo_fmv.c). */
#define CHR_A_WORD  0x0000u
#define CHR_B_WORD  0x4000u
#define TM_A_WORD   0x7C00u
#define TM_B_WORD   0x7800u

/* 15 fps FULL-BURST: all 780 tiles delivered in vblank over 4 sub-frames, NO
 * siphon. Pixel-perfect (no active-display VRAM writes) AND correct speed for
 * the native-15fps movie.fmv. Each sub-frame <= the ~7452 B (8,8) burst budget:
 *   SF0 = CGRAM(256)+TM(2048)+C1(4800) = 7104;  SF1..3 = C2/C3/C4 = 6720 each. */
#define CHR_C1  4800u   /* 150 tiles (SF0, after CGRAM+tilemap) */
#define CHR_C2  6720u   /* 210 tiles (SF1) */
#define CHR_C3  6720u   /* 210 tiles (SF2) */
#define CHR_C4  6720u   /* 210 tiles (SF3) */
#define CHR_USED (CHR_C1 + CHR_C2 + CHR_C3 + CHR_C4)  /* 24960 = 780 tiles */
#define TM_BYTES   2048u   /* 32×32 cells × 2 */

/* Payload byte offsets within MgCompleteFrame.payload. */
#define OFF_CGRAM  0u
#define OFF_TM     (OFF_CGRAM + FMV_CGRAM_BYTES)   /* 256  */
#define OFF_C1     (OFF_TM + TM_BYTES)             /* 2304 */
#define OFF_C2     (OFF_C1 + CHR_C1)
#define OFF_C3     (OFF_C2 + CHR_C2)
#define OFF_C4     (OFF_C3 + CHR_C3)
#define PAYLOAD_USED (OFF_C4 + CHR_C4)             /* 27264 — full 780 tiles */

/* DMA descriptor encodings (copro_mg_state.c). */
#define BBUS_VMDATAL 0x18u
#define BBUS_CGDATA  0x22u
#define DMAP_1B_1R   0x00u
#define DMAP_2B_2R   0x01u

typedef struct {
    int          fd;
    uint32_t     frame_n;
    uint32_t     nframes;
    uint32_t     abytes;        /* audio bytes per unit (skipped here) */
    uint8_t     *unit;          /* abytes + FMV_VIDEO_BLOCK_BYTES read scratch */
    uint32_t     unit_bytes;
    uint8_t      prev_pal[FMV_CGRAM_BYTES];  /* frame N-1's palette (pairing) */
    MgDmaEngine *dma;
    bool         eof;
} FmvVideoCtx;

static int read_full(int fd, void *buf, uint32_t n) {
    uint8_t *p = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < n) {
        int32_t r = vm_host_fs_route_read(fd, p + got, n - got);
        if (r <= 0) break;
        got += (uint32_t)r;
    }
    return (int)got;
}

/* Fill one ring slot with a complete cart-window-ready frame. */
static bool fmv_video_fill(void *vctx, void *slot) {
    FmvVideoCtx     *c = (FmvVideoCtx *)vctx;
    MgCompleteFrame *f = (MgCompleteFrame *)slot;

    if (c->frame_n >= c->nframes) { c->eof = true; return false; }
    if (read_full(c->fd, c->unit, c->unit_bytes) != (int)c->unit_bytes) {
        c->eof = true; return false;
    }

    /* Split the unit: [audio abytes][CGRAM 256][tilemap 1560][CHR 24960]. */
    const uint8_t *cg  = c->unit + c->abytes;
    const uint8_t *tms = cg + FMV_CGRAM_BYTES;
    /* chr starts after the raw tilemap; offset within the unit for the DMA. */
    uint32_t chr_off = c->abytes + FMV_CGRAM_BYTES + FMV_TM_SRC_BYTES;

    /* Buffer alternation (back = this frame's write target; front = the
     * buffer built last frame, now to be displayed). */
    bool back_is_a    = (c->frame_n % 2u) == 0u;
    uint16_t back_chr  = back_is_a ? CHR_A_WORD : CHR_B_WORD;
    uint16_t back_tm   = back_is_a ? TM_A_WORD  : TM_B_WORD;
    uint16_t front_chr = back_is_a ? CHR_B_WORD : CHR_A_WORD;
    uint16_t front_tm  = back_is_a ? TM_B_WORD  : TM_A_WORD;

    MgDmaRegion dst  = { MG_DMA_MEM_PSRAM, f->payload, MG_FRAME_PAYLOAD_BYTES };
    MgDmaRegion unit = { MG_DMA_MEM_HOST,  c->unit,    c->unit_bytes };
    MgDmaRegion prev = { MG_DMA_MEM_HOST,  c->prev_pal, sizeof c->prev_pal };

    /* CGRAM = the PREVIOUS frame's palette (pairs with the FRONT buffer we
     * display this frame). Frame 0's prev_pal is zeroed → black, matching
     * demo_fmv.c skipping the load on frame 0 (front buffer is empty). */
    mg_dma_copy(c->dma, dst, OFF_CGRAM, prev, 0, FMV_CGRAM_BYTES);

    /* Tilemap: 32×32 of BLANK_TILE with the 30×26 FMV cells splatted into
     * the centered region (init_tilemap_margins + splat_fmv_tilemap). Built
     * directly into the slot (a transform, not a plain copy). */
    {
        uint16_t *tm = (uint16_t *)(f->payload + OFF_TM);
        for (unsigned i = 0; i < 32u * 32u; i++) tm[i] = (uint16_t)FMV_BLANK_TILE;
        for (unsigned r = 0; r < FMV_TH; r++) {
            memcpy(&tm[(r + 1u) * 32u + 1u], tms + r * (FMV_TW * 2u), FMV_TW * 2u);
        }
    }

    /* CHR: all 780 tiles delivered by the vblank burst (no siphon). */
    mg_dma_copy(c->dma, dst, OFF_C1, unit, chr_off, CHR_USED);

    f->payload_used = PAYLOAD_USED;

    /* Sub-frames — 15 fps full-burst, 4 vblank sub-frames (each ≤ ~7452 B):
     *   SF0 = CGRAM + tilemap + C1, SF1 = C2, SF2 = C3, SF3 = C4. */
    SubFrame *sf0 = &f->subframes[0];
    sf0->slot_count = 3;
    sf0->slots[0] = (StagedSlot){ .bbus=BBUS_CGDATA,  .dmap=DMAP_1B_1R, .prep=0u,       .src=OFF_CGRAM, .size=FMV_CGRAM_BYTES };
    sf0->slots[1] = (StagedSlot){ .bbus=BBUS_VMDATAL, .dmap=DMAP_2B_2R, .prep=back_tm,  .src=OFF_TM,    .size=TM_BYTES };
    sf0->slots[2] = (StagedSlot){ .bbus=BBUS_VMDATAL, .dmap=DMAP_2B_2R, .prep=back_chr, .src=OFF_C1,    .size=CHR_C1 };

    SubFrame *sf1 = &f->subframes[1];
    sf1->slot_count = 1;
    sf1->slots[0] = (StagedSlot){ .bbus=BBUS_VMDATAL, .dmap=DMAP_2B_2R, .prep=(uint16_t)(back_chr + CHR_C1/2u),               .src=OFF_C2, .size=CHR_C2 };

    SubFrame *sf2 = &f->subframes[2];
    sf2->slot_count = 1;
    sf2->slots[0] = (StagedSlot){ .bbus=BBUS_VMDATAL, .dmap=DMAP_2B_2R, .prep=(uint16_t)(back_chr + (CHR_C1+CHR_C2)/2u),      .src=OFF_C3, .size=CHR_C3 };

    SubFrame *sf3 = &f->subframes[3];
    sf3->slot_count = 1;
    sf3->slots[0] = (StagedSlot){ .bbus=BBUS_VMDATAL, .dmap=DMAP_2B_2R, .prep=(uint16_t)(back_chr + (CHR_C1+CHR_C2+CHR_C3)/2u), .src=OFF_C4, .size=CHR_C4 };

    f->subframe_count = 4;

    /* PPU batch: flip BG1 to the FRONT buffer (displaying last frame's CHR
     * with last frame's palette = the cgram we just staged). Mode 1, BG1
     * main, vofs = -1 (the demo's 1px content shift). */
    memset(&f->ppu_batch, 0, sizeof f->ppu_batch);
    f->ppu_batch.bgmode  = 0x01u;                       /* MG_BG_MODE_1 */
    f->ppu_batch.bg1sc   = mg_bg_sc(front_tm, 0u);      /* 32×32 */
    f->ppu_batch.bg12nba = mg_chr_page(front_chr);
    f->ppu_batch.tm      = 0x01u;                       /* BG1 on main screen */
    f->ppu_batch.bg1vofs = (uint16_t)0xFFFFu;           /* vofs = -1 */

    memset(&f->mode7, 0, sizeof f->mode7);

    /* No siphon — all 780 tiles delivered by the vblank burst (15 fps full
     * height). siphon_bytes=0 => the kernel's State B takes the @b_no_siphon
     * path (no per-scanline VRAM DMA). */
    f->siphon_bytes = 0u;
    f->siphon_lines = 0u;
    f->siphon_src   = 0u;
    f->siphon_vram  = 0u;

    /* Save this frame's palette for next frame's pairing. */
    memcpy(c->prev_pal, cg, FMV_CGRAM_BYTES);
    c->frame_n++;
    return true;
}

static bool fmv_video_at_eof(void *vctx) {
    return ((FmvVideoCtx *)vctx)->eof;
}

static void fmv_video_close(void *vctx) {
    FmvVideoCtx *c = (FmvVideoCtx *)vctx;
    if (c) { free(c->unit); free(c); }
}

StreamHandle fmv_video_stream_open(int fd, uint32_t abytes, uint32_t nframes,
                                   MgDmaEngine *dma, uint32_t depth) {
    if (fd < 0 || !dma) return STREAM_HANDLE_INVALID;

    FmvVideoCtx *c = (FmvVideoCtx *)calloc(1, sizeof *c);
    if (!c) return STREAM_HANDLE_INVALID;
    c->fd         = fd;
    c->abytes     = abytes;
    c->nframes    = nframes;
    c->dma        = dma;
    c->unit_bytes = abytes + FMV_VIDEO_BLOCK_BYTES;
    c->unit       = (uint8_t *)malloc(c->unit_bytes);
    if (!c->unit) { free(c); return STREAM_HANDLE_INVALID; }

    StreamProducer p;
    p.fill   = fmv_video_fill;
    p.at_eof = fmv_video_at_eof;
    p.close  = fmv_video_close;
    p.ctx    = c;

    StreamHandle h = stream_arbiter_register_producer(&p, (uint32_t)sizeof(MgCompleteFrame), depth);
    if (h == STREAM_HANDLE_INVALID) { free(c->unit); free(c); }
    return h;
}
