/* demo_fmv_still.c — minimal video-path test for FMV.
 *
 * Reads ONE frame from /host/movie.fmv, uploads it, and holds it on
 * screen until START. No audio, no per-frame loop, no force-blank.
 * Isolates "does the FMV's CGRAM + tilemap + CHR ever reach the PPU"
 * from all the timing/streaming/pacing issues of the full player.
 *
 * If you see a still BBB frame: video path works, the full player's
 * "no video" is a pacing/budget problem.
 * If you see black: the upload path itself is broken (DMA budget,
 * BG state, palette, something fundamental).
 *
 * Public domain (CC0). No warranty.
 */
#include "mg_input.h"
#include "mg_frame.h"
#include "mg_bg.h"
#include "mg_gfx.h"
#include "fs.h"
#include "vm_runtime.h"

#include <stdint.h>

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
static void sys_sleep_ticks(unsigned n) {
    register unsigned a0 asm("a0") = n;
    register int      a7 asm("a7") = SYS_SLEEP_TICKS;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
}
static unsigned my_strlen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void dbg(const char *s) { sys_write(1, s, my_strlen(s)); }

#define VW   240
#define VH   208
#define TW   (VW / 8)
#define TH   (VH / 8)
#define NTILES (TW * TH)
#define BLANK_TILE NTILES
#define CGRAM_BYTES  (8 * 16 * 2)
#define TILEMAP_BYTES (NTILES * 2)
#define CHR_BYTES    (NTILES * 32)
#define VIDEO_BLOCK_BYTES (CGRAM_BYTES + TILEMAP_BYTES + CHR_BYTES)
#define TMAP_W   0x0000
#define CHR_W    0x2000
#define FMV2_HDR 32
#define AUDIO_BYTES_PER_FRAME 8820

static uint8_t  s_video[VIDEO_BLOCK_BYTES];
static MgBgTile s_tilemap[32 * 32];

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

void _start(void) {
    dbg("\r\nfmv_still: _start\r\n");

    mg_ppu_clean_slate();
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, TMAP_W, MG_BG_SIZE_32x32, CHR_W);
    mg_bg_enable(MG_BG_LAYER_1, true, false);

    /* Force-blank 8 top + 8 bottom is REQUIRED here even for a static
     * frame: MG_SUBFRAME_BYTE_BUDGET (9200) is sized for
     * vblank + force-blank lines combined. Without force-blank, the
     * per-NMI DMA capacity drops to ~6479 B (38 vblank lines × 1364
     * mcyc / 8 mcyc/byte), so sub-frame 0 (cgram+tilemap+chr1 = 9024 B)
     * truncates mid-CHR — most tiles end up at zero CHR = palette[0] =
     * "black screen." With force-blank 8/8 we get 38+16=54 lines of
     * budget → ~9200 B/NMI, matching the packer's assumption. */
    /* v2.16: TOP-only HDMA mode — see demo_fmv.c for the rationale.
     * Same per-NMI DMA budget as the streaming player; the static
     * single-commit case benefits from the same protection against
     * HDMA-mid-DMA INIDISP override that the player needs. */
    mg_force_blank(8, 0);
    dbg("fmv_still: BG1 set up\r\n");

    /* Fill margins with blank tile, FMV occupies (1,1)..(30,26). */
    MgBgTile blank;
    blank.word = (uint16_t)BLANK_TILE;
    for (uint32_t i = 0; i < 32 * 32; i++) s_tilemap[i] = blank;

    int fd = fs_open("/host/movie.fmv", O_RDONLY);
    if (fd < 0) { dbg("fmv_still: open FAILED\r\n"); sys_exit(1); }

    /* Skip 32-byte header + first frame's 8820-byte audio chunk; we
     * only want frame 0's video block. */
    uint8_t scratch[64];
    if (read_full(fd, scratch, FMV2_HDR) != FMV2_HDR) { dbg("hdr read\r\n"); sys_exit(1); }
    uint32_t to_skip = AUDIO_BYTES_PER_FRAME;
    while (to_skip > 0) {
        uint32_t want = to_skip > sizeof(scratch) ? (uint32_t)sizeof(scratch) : to_skip;
        if (read_full(fd, scratch, want) != (int)want) { dbg("skip\r\n"); sys_exit(1); }
        to_skip -= want;
    }
    if (read_full(fd, s_video, VIDEO_BLOCK_BYTES) != VIDEO_BLOCK_BYTES) {
        dbg("fmv_still: video read FAILED\r\n"); sys_exit(1);
    }
    fs_close(fd);
    dbg("fmv_still: frame 0 loaded\r\n");

    /* upload CGRAM + tilemap + CHR */
    const uint8_t *cg  = s_video;
    const uint8_t *tm  = cg + CGRAM_BYTES;
    const uint8_t *chr = tm + TILEMAP_BYTES;

    mg_palette_load(0, (const uint16_t *)cg, 128);
    /* Splat the 30x26 tilemap into rows 1..26, cols 1..30. */
    for (uint32_t r = 0; r < TH; r++) {
        const uint8_t *src = tm + r * (TW * 2);
        MgBgTile *dst = &s_tilemap[(r + 1) * 32 + 1];
        for (uint32_t c = 0; c < TW; c++) {
            dst[c].word = (uint16_t)(src[c * 2] | (src[c * 2 + 1] << 8));
        }
    }
    mg_bg_upload(MG_BG_LAYER_1, 0, 0, s_tilemap, 32 * 32);

    /* v2.10: chunk CHR into 3 sub-frame-budget-sized DMAs (6720 +
     * 9120 + 9120 = 24960). bsnes-plus's per-NMI DMA budget is
     * ~9200 B; a single 24960 B DMA truncates mid-transfer and
     * leaves tiles 288-779 as zero CHR (rendering as palette[0],
     * which BBB's encoder picks as a dark color → "all black"
     * symptom). flush_subframes() at commit time packs these 3
     * slots into 3 sub-frames so all of CHR lands across 3 NMIs
     * (~50 ms after commit). The hold-until-START loop below
     * covers that 50 ms easily. */
    const uint16_t CHR_C1 = 6720;
    const uint16_t CHR_C2 = 9120;
    const uint16_t CHR_C3 = (uint16_t)(CHR_BYTES - CHR_C1 - CHR_C2);
    mg_chr_upload((uint16_t)(CHR_W + 0u),
                  chr + 0u, CHR_C1);
    mg_chr_upload((uint16_t)(CHR_W + CHR_C1 / 2u),
                  chr + CHR_C1, CHR_C2);
    mg_chr_upload((uint16_t)(CHR_W + (CHR_C1 + CHR_C2) / 2u),
                  chr + CHR_C1 + CHR_C2, CHR_C3);

    mg_frame_commit();
    dbg("fmv_still: commit fired; sleeping until START\r\n");

    /* Hold the frame until user presses START. Poll pads every 100 ms. */
    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) break;
        sys_sleep_ticks(100);
    }
    dbg("fmv_still: exit\r\n");
    sys_exit(0);
}
