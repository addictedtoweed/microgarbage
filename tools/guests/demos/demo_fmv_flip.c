/* ============================================================
 *  demo_fmv_flip.c — isolated double-buffer flip test.
 *
 *  Loads frame 0 from /host/movie.fmv into buffer A (TM_A/CHR_A)
 *  and frame 1 into buffer B (TM_B/CHR_B). Uses tiny chunks (4160 B
 *  per CHR commit) so each upload is well within vblank — no chance
 *  of DMA overrunning into active display. Once both buffers are
 *  fully uploaded, ping-pongs BG1 between them so we can verify the
 *  buffer-swap mechanism in isolation from the streaming loop.
 *
 *  Controls:
 *    Auto-swap every 60 frames (~1 sec).
 *    LEFT  : hold A (override auto)
 *    RIGHT : hold B (override auto)
 *    START : resume auto-swap
 *    SELECT: exit
 *
 *  Layout: 32×32 tilemap, FMV at rows 1..26 (centered), BG1 Mode 1,
 *  4bpp, sprites disabled, mg_force_blank(8, 0). Frame 0's palette
 *  is loaded for both buffers (so frame 1's color reproduction may
 *  look slightly off, but the structural tilemap+CHR is the test).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_input.h"
#include "mg_frame.h"
#include "mg_panic.h"
#include "mg_bg.h"
#include "mg_gfx.h"
#include "fs.h"
#include "vm_runtime.h"

#include <stdint.h>

static int my_memcmp(const void *a, const void *b, uint32_t n) {
    const uint8_t *pa = (const uint8_t *)a, *pb = (const uint8_t *)b;
    for (uint32_t i = 0; i < n; i++) {
        if (pa[i] != pb[i]) return (int)pa[i] - (int)pb[i];
    }
    return 0;
}
static void my_memcpy(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

static int sys_write(int fd, const void *buf, unsigned n) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)buf;
    register unsigned a2 asm("a2") = n;
    register int      a7 asm("a7") = SYS_WRITE;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
static unsigned my_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static void dbg(const char *s) { sys_write(1, s, my_strlen(s)); }
static void dbg_u32(uint32_t v) {
    if (v == 0) { sys_write(1, "0", 1); return; }
    char tmp[12]; int t = 0;
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    char buf[12]; int n = 0;
    while (t > 0) buf[n++] = tmp[--t];
    sys_write(1, buf, n);
}

/* Layout matches demo_fmv.c. */
#define VW   240
#define VH   208
#define TW   (VW / 8)            /* 30 */
#define TH   (VH / 8)            /* 26 */
#define NTILES (TW * TH)         /* 780 */
#define BLANK_TILE NTILES

#define CGRAM_BYTES       256u
#define TILEMAP_BYTES     (NTILES * 2u)        /* 1560 */
#define CHR_BYTES         (NTILES * 32u)       /* 24960 */
#define VIDEO_BLOCK_BYTES (CGRAM_BYTES + TILEMAP_BYTES + CHR_BYTES)  /* 26776 */

#define CHR_A_WORD   0x0000
#define CHR_B_WORD   0x4000
#define TM_A_WORD    0x7C00
#define TM_B_WORD    0x7800

/* Tiny CHR chunk size — 4160 B = 130 tiles. 24960 / 4160 = 6 chunks.
 * 4160 × 8 = 33280 master cyc = 24 scanlines, well within vblank's 38.
 * No chance of overrun into active display, no per-NMI budget issue. */
#define CHR_CHUNK_BYTES   4160u
#define CHR_CHUNK_COUNT   (CHR_BYTES / CHR_CHUNK_BYTES)  /* 6 */

/* BSS budget at ~64 KB: 1024 discard + 2 × 26776 video + 2048 tilemap
 * = 56624 B. Fits. */
static uint8_t s_discard[1024];
static uint8_t s_video_a[VIDEO_BLOCK_BYTES];
static uint8_t s_video_b[VIDEO_BLOCK_BYTES];
static MgBgTile s_tilemap[32 * 32];

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* Loop-read until n bytes are consumed (FS short reads possible). */
static int read_full(int fd, void *buf, uint32_t n) {
    uint8_t *p = (uint8_t *)buf;
    uint32_t got = 0;
    while (got < n) {
        int r = fs_read(fd, p + got, n - got);
        if (r <= 0) return (int)got;
        got += (uint32_t)r;
    }
    return (int)got;
}

/* Skip-read n bytes (= read into discard buffer in 1KB chunks). */
static void skip_bytes(int fd, uint32_t n) {
    while (n > 0) {
        uint32_t to_read = n > sizeof(s_discard) ? sizeof(s_discard) : n;
        int r = fs_read(fd, s_discard, to_read);
        if (r <= 0) return;
        n -= (uint32_t)r;
    }
}

static void init_tilemap_margins(void) {
    MgBgTile blank;
    blank.word = (uint16_t)BLANK_TILE;
    for (uint32_t i = 0; i < 32 * 32; i++) s_tilemap[i] = blank;
}

static void splat_fmv_tilemap(const uint8_t *tm_bytes) {
    for (uint32_t r = 0; r < TH; r++) {
        const uint8_t *src = tm_bytes + r * (TW * 2);
        MgBgTile *dst = &s_tilemap[(r + 1) * 32 + 1];
        my_memcpy(dst, src, TW * 2);
    }
}

/* One-commit helper: commit + wait for the frame to be fully consumed. */
static void commit_and_wait(void) {
    mg_frame_commit();
    mg_wait_frame();
}

void _start(void) {
    dbg("\r\nfmv_flip: _start\r\n");

    /* NOTE: deliberately skip mg_ppu_clean_slate(). The clean_slate
     * call arms two things that destroy this isolated test:
     *   1. 64 KB VRAM-clear DMA at slot 0 — 1.46 frames of force-blank
     *      while the slot fires (fine in itself).
     *   2. A 60-frame BG-shadow forced re-upload window that stages
     *      a 2 KB DMA to bg[0].tilemap_word EVERY commit. Since the
     *      shadow content is empty zeros (this demo doesn't use
     *      mg_bg_blit), each commit zeroes whatever tilemap_word
     *      currently points at, which wipes out our transient
     *      tilemap upload from the previous commit.
     * Skipping clean_slate avoids both. VRAM starts at whatever the
     * previous demo / cold boot left; our uploads overwrite the
     * regions we care about (TM_A, TM_B, CHR_A, CHR_B). */

    /* BG1 Mode 1, single tilemap at TM_A_WORD.
     *
     * DIAGNOSTIC v2: use ONE nametable shared between both buffers.
     * BG1's tilemap_word is FIXED at TM_A_WORD throughout. Only
     * the CHR base (BG12NBA) swaps between CHR_A and CHR_B in the
     * ping-pong loop. This eliminates the tilemap-swap mechanism
     * as a possible culprit. */
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, TM_A_WORD, MG_BG_SIZE_32x32, CHR_A_WORD);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);
    /* Full-frame force-blank during setup so the user doesn't see
     * the in-progress upload state. Switched to (8, 0) after both
     * buffers are populated. */
    mg_force_blank(112, 112);

    init_tilemap_margins();

    /* --- Read 2 frames from movie.fmv (audio skipped) --- */
    int fd = fs_open("/host/movie.fmv", O_RDONLY);
    if (fd < 0) {
        dbg("fmv_flip: open /host/movie.fmv failed\r\n");
        sys_exit(1);
    }

    uint8_t hdr[32];
    if (read_full(fd, hdr, 32) != 32 || my_memcmp(hdr, "FMV2", 4) != 0) {
        dbg("fmv_flip: not FMV2\r\n");
        fs_close(fd);
        sys_exit(1);
    }
    uint32_t abytes = rd_u32le(hdr + 24);
    dbg("fmv_flip: abytes="); dbg_u32(abytes); dbg("\r\n");

    /* Skip ahead 300 frames (~20 seconds at 15 fps) past the fade-in
     * from black at the start of BBB. */
    dbg("fmv_flip: seeking 300 frames in...\r\n");
    for (uint32_t i = 0; i < 300; i++) {
        skip_bytes(fd, abytes);
        skip_bytes(fd, VIDEO_BLOCK_BYTES);
    }

    /* Frame 300 → buffer A. */
    skip_bytes(fd, abytes);
    if (read_full(fd, s_video_a, VIDEO_BLOCK_BYTES) != (int)VIDEO_BLOCK_BYTES) {
        dbg("fmv_flip: frame 300 read short\r\n");
        fs_close(fd);
        sys_exit(1);
    }

    /* Skip ahead another 30 frames (~2 sec) before B. */
    for (uint32_t i = 0; i < 30; i++) {
        skip_bytes(fd, abytes);
        skip_bytes(fd, VIDEO_BLOCK_BYTES);
    }

    /* Frame 331 → buffer B. */
    skip_bytes(fd, abytes);
    if (read_full(fd, s_video_b, VIDEO_BLOCK_BYTES) != (int)VIDEO_BLOCK_BYTES) {
        dbg("fmv_flip: frame 331 read short\r\n");
        fs_close(fd);
        sys_exit(1);
    }
    fs_close(fd);
    dbg("fmv_flip: 2 frames loaded\r\n");

    const uint8_t *cg_a  = s_video_a;
    const uint8_t *tm_a  = cg_a + CGRAM_BYTES;
    const uint8_t *chr_a = tm_a + TILEMAP_BYTES;

    const uint8_t *cg_b  = s_video_b;
    const uint8_t *tm_b  = cg_b + CGRAM_BYTES;
    const uint8_t *chr_b = tm_b + TILEMAP_BYTES;

    /* --- Upload frame 0 to buffer A across 8 commits (1 chunk each) --- */
    dbg("fmv_flip: uploading frame 0 to A...\r\n");

    /* Commit 1: palette (frame 0's CGRAM) */
    mg_palette_load(0, (const uint16_t *)cg_a, 128);
    commit_and_wait();

    /* Commit 2: ONE shared tilemap at TM_A_WORD (using frame 0's
     * tilemap data; frame 1 uses the same since cell indices match,
     * only palette bits could differ — and both frames typically
     * share most palette assignments since BBB scenes don't change
     * dramatically over 2 sec). */
    splat_fmv_tilemap(tm_a);
    MG_OR_PANIC(mg_chr_upload_transient(TM_A_WORD, s_tilemap, TILEMAP_BYTES));
    commit_and_wait();

    /* Commits 3..8: CHR A → CHR_A_WORD */
    for (uint32_t i = 0; i < CHR_CHUNK_COUNT; i++) {
        uint16_t vram = (uint16_t)(CHR_A_WORD + (i * CHR_CHUNK_BYTES) / 2u);
        MG_OR_PANIC(mg_chr_upload_transient(vram,
                                            chr_a + i * CHR_CHUNK_BYTES,
                                            CHR_CHUNK_BYTES));
        commit_and_wait();
    }

    /* --- Upload frame 1 to buffer B across 7 commits --- */
    dbg("fmv_flip: uploading frame 1 to B...\r\n");

    /* (Skip palette — both buffers display with frame 0's palette for
     * this test; we're verifying buffer swap structure, not chroma.) */

    /* (No second tilemap upload — using the one shared TM_A_WORD.) */

    /* Commits: CHR B → CHR_B_WORD in 6 chunks */
    for (uint32_t i = 0; i < CHR_CHUNK_COUNT; i++) {
        uint16_t vram = (uint16_t)(CHR_B_WORD + (i * CHR_CHUNK_BYTES) / 2u);
        MG_OR_PANIC(mg_chr_upload_transient(vram,
                                            chr_b + i * CHR_CHUNK_BYTES,
                                            CHR_CHUNK_BYTES));
        commit_and_wait();
    }

    dbg("fmv_flip: both buffers loaded\r\n");
    (void)chr_b;

    /* Unblank for normal display. */
    mg_force_blank(8, 0);
    mg_frame_commit();
    mg_wait_frame();
    dbg("fmv_flip: unblanked, entering ping-pong loop\r\n");

    /* --- Ping-pong loop --- */
    bool   show_a   = true;
    bool   auto_swap = true;
    uint32_t counter = 0;
    /* Initial setup already points BG1 → A. */

    uint32_t loop_iter = 0;
    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_SELECT)) sys_exit(0);

        if (loop_iter < 3) {
            dbg("fmv_flip: loop iter "); dbg_u32(loop_iter); dbg("\r\n");
        }
        loop_iter++;

        bool override_to_a = mg_pad_held(pads.p0, MG_BTN_LEFT);
        bool override_to_b = mg_pad_held(pads.p0, MG_BTN_RIGHT);
        bool resume_auto   = mg_pad_pressed(pads.p0, MG_BTN_START);

        if (resume_auto) auto_swap = true;
        if (override_to_a || override_to_b) auto_swap = false;

        if (override_to_a) {
            show_a = true;
        } else if (override_to_b) {
            show_a = false;
        } else if (auto_swap) {
            counter++;
            if (counter >= 60) {
                counter = 0;
                show_a = !show_a;
            }
        }

        /* Tilemap stays fixed at TM_A_WORD; only CHR base swaps. */
        if (show_a) {
            mg_bg_setup(MG_BG_LAYER_1, TM_A_WORD,
                        MG_BG_SIZE_32x32, CHR_A_WORD);
        } else {
            mg_bg_setup(MG_BG_LAYER_1, TM_A_WORD,
                        MG_BG_SIZE_32x32, CHR_B_WORD);
        }

        mg_frame_commit();
        mg_wait_frame();
    }
}
