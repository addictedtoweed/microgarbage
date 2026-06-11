/* demo_fmv.c — FMV2 player guest ELF.
 *
 * Streams /host/movie.fmv (FMV2 = 32-byte header + per-frame
 * [audio | video] units) through the mgapi runtime. Audio chunk
 * → PCM streaming voice; video chunk → CGRAM + tilemap + CHR
 * uploaded over the 3 SNES vblanks the FMV frame occupies (20 fps
 * × 3 = 60 Hz vblank cadence).
 *
 * START exits cleanly; file-end auto-exits.
 *
 * Phase 2: dry player (no overlay, no SFX). Phase 3 will layer a
 * crosshair sprite + button-triggered gunshot SFX on top.
 *
 * Public domain (CC0). No warranty.
 */
#include "mg_input.h"
#include "mg_frame.h"
#include "mg_panic.h"
#include "mg_audio.h"
#include "mg_bg.h"
#include "mg_gfx.h"
#include "mg_stream.h"
#include "fs.h"
#include "vm_runtime.h"

#include <stdint.h>

/* Guest runtime doesn't ship libc string fns; tiny private copies. */
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

/* Direct stdio + sleep ecalls, bypassing mg_* wrappers we don't need. */
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
static uint32_t sys_ticks_now(void) {
    register uint32_t a0 asm("a0");
    register int      a7 asm("a7") = SYS_TICKS_NOW;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static unsigned my_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static void dbg(const char *s) { sys_write(1, s, my_strlen(s)); }
static void dbg_u32(uint32_t v) {
    char buf[12]; int n = 0;
    if (v == 0) { sys_write(1, "0", 1); return; }
    char tmp[12]; int t = 0;
    while (v) { tmp[t++] = (char)('0' + v % 10); v /= 10; }
    while (t > 0) buf[n++] = tmp[--t];
    sys_write(1, buf, n);
}

/* FMV layout constants (must match tools/fmv_encode.c). */
#define VW   240
#define VH   208
#define TW   (VW / 8)            /* 30 tiles wide   */
#define TH   (VH / 8)            /* 26 tiles tall   */
#define NTILES (TW * TH)         /* 780 = exactly fills the 4bpp tile budget */
#define BLANK_TILE NTILES        /* index 780; CHR for it is left zeroed by
                                    clean_slate so it renders as backdrop */

/* Per-frame block sizes inside the .fmv. */
#define CGRAM_BYTES  (8 * 16 * 2)         /* 256 — 128 BGR555 entries        */
#define TILEMAP_BYTES (NTILES * 2)        /* 1560 — raw 16-bit cells         */
#define CHR_BYTES    (NTILES * 32)        /* 24960 — 4bpp CHR for 780 tiles  */
#define VIDEO_BLOCK_BYTES (CGRAM_BYTES + TILEMAP_BYTES + CHR_BYTES)  /* 26776 */

/* v2.06: double-buffered VRAM layout. Two CHR banks (A,B) and two
 * tilemaps so we can upload to the back buffer over 3 sub-frames
 * while BG1 displays the fully-rendered front buffer — no
 * partial-update tearing. BG12NBA's CHR base is 4K-word-aligned;
 * BG1SC's tilemap base is 1K-word-aligned.
 *   $0000-$30BF  CHR_A     (12480 words = 780 tiles × 16 words/tile)
 *   $4000-$70BF  CHR_B     (same)
 *   $7800-$7BFF  TM_B      (1024 words = 32×32 cells)
 *   $7C00-$7FFF  TM_A
 * Total: ~52 KB of VRAM used (out of 64 KB available). */
#define CHR_A_WORD   0x0000
#define CHR_B_WORD   0x4000
#define TM_A_WORD    0x7C00
#define TM_B_WORD    0x7800

/* Read u16 / u32 LE out of a byte buffer (the .fmv header). */
static uint16_t rd_u16le(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}
static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

/* Read exactly `n` bytes from fd into buf (loop over short reads). */
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

/* Per-frame staging. The stream arbiter delivers one A/V chunk
 * (audio + video, encoder-interleaved) per consume call directly
 * into s_frame[]; we split it in-place via pointer math. With the
 * arbiter pre-reading 3 chunks ahead, a sudden 50+ ms SD hiccup
 * doesn't starve playback. */
/* v2.31: size MAX_AUDIO_BYTES for 15 fps (67 ms/frame = 11760 B audio).
 * Old 20 fps was 8820 B; new buffer fits both. The actual abytes is read
 * from the FMV header — this constant is just the upper-bound cap. */
#define MAX_AUDIO_BYTES   11760                       /* 67 ms @ 44.1 kHz stereo */
#define MAX_FRAME_BYTES   (MAX_AUDIO_BYTES + VIDEO_BLOCK_BYTES)   /* 38536 */
static uint8_t  s_frame[MAX_FRAME_BYTES];
static MgBgTile s_tilemap[32 * 32];      /* 32×32 with BLANK_TILE margins */
/* v2.06: previous frame's CGRAM cached so the palette upload in the
 * current iteration pairs with the FRONT buffer (which holds last
 * iteration's data). Without this, palette would flash one frame
 * ahead of the buffer. */
static uint8_t  s_prev_palette[CGRAM_BYTES];

static void init_tilemap_margins(void) {
    /* Fill the entire 32×32 with the blank tile; the active per-frame
     * upload overwrites the 30×26 center, leaving a 1-tile margin of
     * blank around it (260 active px height in a 224-line frame =
     * 16 lines of dead space split top/bottom, plus 16 px left/right
     * margin in a 256 px frame). */
    MgBgTile blank;
    blank.word = (uint16_t)BLANK_TILE;
    for (uint32_t i = 0; i < 32 * 32; i++) s_tilemap[i] = blank;
}

/* Copy the 30×26 FMV tilemap into the centered region of s_tilemap.
 * Encoder writes cells as raw u16 little-endian (matches MgBgTile.word
 * on RV32 LE), so a per-row memcpy is correct. */
static void splat_fmv_tilemap(const uint8_t *tm_bytes) {
    for (uint32_t r = 0; r < TH; r++) {
        const uint8_t *src = tm_bytes + r * (TW * 2);
        MgBgTile *dst = &s_tilemap[(r + 1) * 32 + 1];
        my_memcpy(dst, src, TW * 2);
    }
}

void _start(void) {
    dbg("\r\nfmv: _start\r\n");

    /* --- video setup: Mode 1, BG1 starts pointing at buffer B (empty,
     * shows black). First iteration uploads to buffer A; iteration 2
     * swaps BG1 to A (then-fully-uploaded) and starts uploading B.
     * From there, BG1 always shows the buffer that was fully
     * uploaded last iteration — clean, no tearing. */
    mg_ppu_clean_slate();
    dbg("fmv: after clean_slate\r\n");
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, TM_B_WORD, MG_BG_SIZE_32x32, CHR_B_WORD);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);

    /* v2.31: mg_force_blank(8, 0) — TOP-only HDMA.
     *
     * At 15 fps the FMV is split into 4 sub-frame DMAs (one per NMI)
     * of ~7000 B each. Per-NMI budget = (38 vblank + ~3 lines of DMA
     * overrun) × ~165 = ~6700 B safe; each chunk fits with some
     * give. The 8 lines of HDMA-written $80 are mostly decorative —
     * with this small a per-NMI DMA, the kernel's @done writes $0F
     * at around line 3-4 of next frame and FMV pixels start at line
     * 8 (BLANK_TILE row 0 in the tilemap), so no clipping. Full
     * 240×208 FMV visible, 8 lines of backdrop margin top + bottom. */
    mg_force_blank(8, 0);

    init_tilemap_margins();
    dbg("fmv: opening file\r\n");

    /* --- open the clip --- */
    int fd = fs_open("/host/movie.fmv", O_RDONLY);
    if (fd < 0) {
        dbg("fmv: open failed\r\n");
        sys_exit(1);
    }
    dbg("fmv: opened\r\n");

    /* --- parse FMV2 header (32 B) --- */
    uint8_t hdr[32];
    if (read_full(fd, hdr, 32) != 32 || my_memcmp(hdr, "FMV2", 4) != 0) {
        mg_panic("not an FMV2 file");
        fs_close(fd);
        sys_exit(1);
    }
    /* FMV2 header offsets (matches tools/fmv_encode.c hdr()):
     *   +0  "FMV2"
     *   +4  w (u16)
     *   +6  h (u16)
     *   +8  fps (u16)          ← v2.31: now read this; was hardcoded
     *   +10 audio_channels (u16)
     *   +12 nframes (u32)
     *   +16 audio_rate (u32)
     *   +20 audio_bits + _ (u32)
     *   +24 audio_bytes_per_frame (u32)
     *   +28 _ (u32)                                                   */
    uint32_t fps      = (uint32_t)rd_u16le(hdr + 8);
    uint32_t nframes  = rd_u32le(hdr + 12);
    uint32_t arate    = rd_u32le(hdr + 16);
    uint32_t abytes   = rd_u32le(hdr + 24);
    dbg("fmv: fps="); dbg_u32(fps);
    dbg(" nframes="); dbg_u32(nframes);
    dbg(" rate="); dbg_u32(arate);
    dbg(" abytes="); dbg_u32(abytes); dbg("\r\n");
    /* abytes is bytes/audio-chunk; sanity-cap so we never overrun s_frame. */
    if (abytes == 0 || abytes > MAX_AUDIO_BYTES) {
        dbg("fmv: bad abytes\r\n");
        fs_close(fd);
        sys_exit(1);
    }
    if (fps == 0 || fps > 60) fps = 20;   /* fallback for old/synth clips */
    uint32_t ms_per_frame = 1000u / fps;

    /* v2.10: hand the fd to the stream arbiter so it pre-reads
     * ahead of our 50 ms consumption. chunk_bytes = abytes + video
     * (the encoder pairs them in lockstep per frame). depth=4
     * gives 3 chunks of look-ahead = ~150 ms cushion against SD
     * latency hiccups on the MCU (Windows reads are <1 ms so the
     * ring stays full trivially). */
    const uint32_t chunk_bytes = abytes + VIDEO_BLOCK_BYTES;
    MgStream stream = mg_stream_open_fd(fd, chunk_bytes, 4u);
    if (stream == MG_STREAM_INVALID) {
        dbg("fmv: stream register failed\r\n");
        fs_close(fd);
        sys_exit(1);
    }

    /* --- open the streaming audio voice --- */
    MgVoice voice = mg_audio_pcm_stream_open(arate);
    if (voice == MG_VOICE_REJECTED) {
        dbg("fmv: pcm_stream_open REJECTED\r\n");
        mg_stream_close(stream);
        fs_close(fd);
        sys_exit(1);
    }
    dbg("fmv: voice opened, entering loop\r\n");

    /* v2.06: prime the mixer ring with silence so the back-pressure
     * feed loop has something to push against from iter 0 — same idea
     * as music_player priming intro/loop heads before play. Without
     * this, the first ~15 iterations burst-feed an empty ring; the
     * back-pressure won't engage until the ring is near full, which
     * also means iter 0-15 commit-fire faster than the kernel can
     * consume (staged > consumed gate drops the inner ones, losing
     * 14-ish FMV frames at startup). Silence avoids any audio
     * duplication: iter 0 feeds frame 0 audio on top, which plays
     * after the 50 ms primed silence drains. Net: ~50 ms cushion of
     * audio "lead time" against scheduler hiccups, no video frames
     * dropped, ~50 ms of audio-lags-video at the very start. */
    {
        static const int16_t s_prime_silence[2205 * 2] = {0};
        (void)mg_audio_pcm_stream_feed(voice, s_prime_silence, 2205);
        dbg("fmv: primed 50ms silence\r\n");
    }

    /* --- per-FMV-frame loop ---
     * 1 video frame is shown across 3 SNES vblanks. We do all uploads
     * (CGRAM / tilemap / CHR) + audio feed up front, then drain 3
     * vblanks (which is what paces playback to 20 fps regardless of
     * how fast the upload itself was). */
    uint32_t frame = 0;
    /* Pacing baseline. Each iter advances target_ms by 50 and sleeps
     * until then; total drift over the FMV is zero. */
    uint32_t target_ms = sys_ticks_now();
    /* Demo profiling — same tick source mgapi feeds into the
     * scheduler (host_platform_monotonic_ms on Windows; SysTick or
     * a hardware free-running counter on the MCU port). Captured at
     * the START of the playback loop and END to validate that wall-
     * clock elapsed matches FMV duration (600 frames × 50 ms = 30 s). */
    uint32_t demo_start_ms = sys_ticks_now();
    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) break;

        if (frame >= nframes) break;

        /* Pull one chunk = [audio | video] from the arbiter. Blocks
         * with short sleep-retries if the ring is briefly empty;
         * returns false on true EOF. */
        if (!mg_stream_consume(stream, s_frame, chunk_bytes)) break;

        /* split chunk: audio | CGRAM | tilemap | CHR */
        const uint8_t *audio = s_frame;
        const uint8_t *cg    = audio + abytes;
        const uint8_t *tm    = cg + CGRAM_BYTES;
        const uint8_t *chr   = tm + TILEMAP_BYTES;

        /* Determine BACK buffer (alternates A,B,A,B...). BG1 currently
         * displays the OPPOSITE buffer — uploaded fully in the
         * previous iteration. */
        bool back_is_a = (frame % 2u) == 0u;
        uint16_t back_chr = back_is_a ? CHR_A_WORD : CHR_B_WORD;
        uint16_t back_tm  = back_is_a ? TM_A_WORD  : TM_B_WORD;

        /* Upload PREVIOUS frame's palette now — it pairs with the
         * FRONT buffer we're about to display. Skip on the very first
         * iteration (front is empty/black anyway). */
        if (frame > 0u) {
            mg_palette_load(0, (const uint16_t *)s_prev_palette, 128);
        }

        /* Tilemap: build the 32×32 with margins + FMV centered, then
         * upload as raw 2048 B to the BACK tilemap address. Bypasses
         * mg_bg_blit (which targets BG1's current tilemap_word, which
         * is FRONT for us). The runtime treats this as a transient
         * VRAM-write DMA. */
        splat_fmv_tilemap(tm);
        mg_chr_upload_transient(back_tm, s_tilemap, TILEMAP_BYTES);

        /* v2.31: CHR split across 4 sub-frame-sized chunks (was 3).
         * At 15 fps the kernel processes 4 NMI sub-frames per FMV
         * frame, so we get 4× the per-NMI DMA budget windows. Per-NMI
         * budget at (38+8) lines × ~165 B/line ≈ 7590 B; sizes below
         * fit with margin. Mid-tile splits land contiguously in VRAM
         * after all 4 chunks complete (each frame's BG1 buffer is the
         * one fully uploaded the PREVIOUS iteration, double-buffered). */
        const uint16_t CHR_C1 = 4160;
        const uint16_t CHR_C2 = 6900;
        const uint16_t CHR_C3 = 6900;
        const uint16_t CHR_C4 = (uint16_t)(CHR_BYTES - CHR_C1 - CHR_C2 - CHR_C3);
        mg_chr_upload_transient(back_chr + 0u,
                                 chr + 0u, CHR_C1);
        mg_chr_upload_transient((uint16_t)(back_chr + CHR_C1 / 2u),
                                 chr + CHR_C1, CHR_C2);
        mg_chr_upload_transient((uint16_t)(back_chr + (CHR_C1 + CHR_C2) / 2u),
                                 chr + CHR_C1 + CHR_C2, CHR_C3);
        mg_chr_upload_transient((uint16_t)(back_chr + (CHR_C1 + CHR_C2 + CHR_C3) / 2u),
                                 chr + CHR_C1 + CHR_C2 + CHR_C3, CHR_C4);

        /* Feed audio (may partially fail if ring is near full; the
         * absolute-time pacing below blocks 50 ms per iter so the
         * mixer drains enough that the FOLLOWING iter's feed will
         * succeed fully). */
        uint32_t aframes = abytes / 4u;
        (void)mg_audio_pcm_stream_feed(voice,
                                        (const int16_t *)audio,
                                        aframes);

        /* Commit. Sub-frame chaining DMAs the back buffer across 3
         * NMIs. BG1 stays on the previous-back-now-front during this
         * window, displaying clean. */
        mg_frame_commit();

        /* v2.31: pace by waiting for the kernel to consume the
         * committed frame (= all sub-frames processed). At 15 fps
         * with 4 sub-frames, that's exactly 4 NMIs = 4/60 s =
         * 66.67 ms — which doesn't divide evenly into integer ms,
         * so the old time-based sleep at 66 ms woke up 0.67 ms
         * early and h_frame_commit silently dropped commits whose
         * predecessors hadn't been fully consumed yet. mg_wait_frame
         * gives exact kernel-cadence pacing with no drift. The
         * target_ms accounting stays for the elapsed-time profile
         * print at exit. */
        target_ms += ms_per_frame;
        mg_wait_frame();

        /* Stage BG1 swap: after this iteration, BG1 should display
         * what we just uploaded (back becomes new front in next iter
         * when the PPU batch with these values is applied). */
        mg_bg_setup(MG_BG_LAYER_1, back_tm, MG_BG_SIZE_32x32, back_chr);

        /* Save this frame's palette to apply when this buffer goes
         * front (NEXT iteration's CGRAM upload). */
        my_memcpy(s_prev_palette, cg, CGRAM_BYTES);

        if ((frame & 31) == 0) {
            dbg("fmv: frame "); dbg_u32(frame); dbg("\r\n");
        }
        frame++;
    }
    /* Elapsed-time profile. Same SYS_TICKS_NOW read at start +
     * finish; difference is wall-clock ms. Prints sec.ms + a frame
     * rate so we can eyeball whether the 50-ms-per-iter pacing
     * actually held. For movie.fmv (600 frames @ 20 fps target):
     * expected 30.000 s and ~20.0 fps. */
    uint32_t demo_end_ms = sys_ticks_now();
    uint32_t elapsed_ms  = demo_end_ms - demo_start_ms;
    uint32_t sec         = elapsed_ms / 1000u;
    uint32_t ms          = elapsed_ms % 1000u;
    dbg("fmv: loop exit at frame "); dbg_u32(frame);
    dbg(" — elapsed "); dbg_u32(sec); dbg(".");
    if (ms < 100u) dbg("0");
    if (ms < 10u)  dbg("0");
    dbg_u32(ms); dbg(" s");
    if (elapsed_ms > 0u && frame > 0u) {
        /* fps × 10 so we get one decimal place without floats. */
        uint32_t fps10 = (frame * 10000u) / elapsed_ms;
        dbg(" (~"); dbg_u32(fps10 / 10u); dbg(".");
        dbg_u32(fps10 % 10u); dbg(" fps)");
    }
    dbg("\r\n");

    /* --- clean up --- */
    mg_audio_pcm_stream_close(voice);
    mg_stream_close(stream);
    fs_close(fd);
    sys_exit(0);
}
