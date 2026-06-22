/* ============================================================
 *  fmv_player.c — FMV playback FSM + cart-window consumer. See header.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "fmv_player.h"

#include "fmv_video_stream.h"     /* fmv_video_stream_open, FMV_VIDEO_BLOCK_BYTES */
#include "cart_frame_types.h"     /* MgCompleteFrame */
#include "copro_mg_state.h"       /* write_subframe_to_cart_window, MG_ADVANCE_* */
#include "cart_window.h"          /* cart_window_load_blob / _set_ppu_batch / _set_frame_ready */
#include "dma_engine.h"
#include "io/stream_arbiter.h"
#include "vm/vm_host_fs.h"        /* vm_host_fs_route_read / _close */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FMV_RING_DEPTH 4u   /* up to 3 frames of lookahead */

/* ---- worker-owned (set at start, used by the producer) ---- */
static int            s_fd      = -1;
static StreamHandle   s_video_h = STREAM_HANDLE_INVALID;
static MgDmaEngine   *s_dma;
static MgDmaOps       s_dma_ops;
static uint32_t       s_nframes;
static uint32_t       s_abytes;
static bool           s_stop_pending;
static uint8_t        s_siphon_htime = 155;   /* H-counter fire pos (validated in siphon_dbuf_test); $env:MG_SIPHON_HTIME */

/* ---- bsnes-owned (kickoff + consumer) ---- */
static MgCompleteFrame s_cur;       /* the active, on-screen frame */
static unsigned        s_sf_idx;    /* sub-frame walk cursor */
static bool            s_kicked;    /* first frame loaded */

/* ---- cross-thread ---- */
static atomic_bool s_active;            /* gates the cart_window dispatch */
static atomic_int  s_state = FMV_STATUS_IDLE;

static uint32_t rd_u32le(const uint8_t *p) {
    return (uint32_t)(p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24));
}

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

/* Publish the per-scanline siphon config for sub-frame `sf_idx`. Each sub-frame
 * siphons its own 1/3 of the tail, so src/vram advance by sf_idx × (lines×bytes)
 * from the frame's base (src in bytes, vram in words = bytes/2). 6 bytes:
 * bytes/line, src(16), vram-word(16), line-count. bytes=0 disables it. The
 * kernel caches this at its state-0 handshake (after the prior FRAME_DONE,
 * before the next State-A burst), so the cached config matches the sub-frame
 * being bursted/siphoned this frame. */
static void publish_siphon_config(unsigned sf_idx) {
    uint32_t step_b = (uint32_t)s_cur.siphon_lines * s_cur.siphon_bytes;  /* 1600 */
    uint16_t src    = (uint16_t)(s_cur.siphon_src  + sf_idx * step_b);
    uint16_t vram   = (uint16_t)(s_cur.siphon_vram + sf_idx * (step_b / 2u));

    uint8_t sip[6];
    sip[0] = s_cur.siphon_bytes;
    sip[1] = (uint8_t)(src  & 0xFFu);
    sip[2] = (uint8_t)(src  >> 8);
    sip[3] = (uint8_t)(vram & 0xFFu);
    sip[4] = (uint8_t)(vram >> 8);
    sip[5] = s_cur.siphon_lines;
    cart_window_load_blob(CW_OFF_SIPHON_CONFIG, sip, sizeof sip);
    cart_window_load_blob(CW_OFF_SIPHON_HTIME, &s_siphon_htime, 1u);
}

/* Load the current frame into the cart window and arm sub-frame 0. */
static void promote_current(void) {
    cart_window_load_blob(0u, s_cur.payload, s_cur.payload_used);
    cart_window_set_ppu_batch(&s_cur.ppu_batch);

    s_sf_idx = 0;
    publish_siphon_config(0u);
    write_subframe_to_cart_window(&s_cur.subframes[0]);
}

static void fmv_player_finalize(void) {
    /* Disable the siphon in the cart window so the next (non-FMV) demo's
     * kernel doesn't cache a stale config and fire per-scanline DMAs. */
    {
        uint8_t off[6] = {0};
        cart_window_load_blob(CW_OFF_SIPHON_CONFIG, off, sizeof off);
    }
    if (s_video_h != STREAM_HANDLE_INVALID) {
        stream_arbiter_unregister(s_video_h);
        s_video_h = STREAM_HANDLE_INVALID;
    }
    if (s_dma) { mg_dma_destroy(s_dma); s_dma = NULL; }
    if (s_fd >= 0) { vm_host_fs_route_close(s_fd); s_fd = -1; }
    s_kicked = false;
    s_sf_idx = 0;
    atomic_store(&s_state, FMV_STATUS_IDLE);
}

/* ----------------------------------------------------------------
 *  Sprite overlay (M1): a cursor + bullethole pool drawn over the FMV.
 *
 *  Writes the static cursor/hole CHR, OBJ palettes, and an initial OAM
 *  (sprite 0 = cursor centered, 1-127 hidden) into the dedicated clean
 *  cart-window regions (CW_OFF_SPR_*). The FMV producer adds DMA slots
 *  that push these to VRAM word 28864 / CGADD 128 / OAM every frame, so
 *  they render on top of BG1. M2/M3 will rewrite the OAM live from input.
 * ---------------------------------------------------------------- */
static uint16_t spr_bgr555(int r, int g, int b) {
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
}

/* Build one 8x8 4bpp SNES tile from an 8-row 1-bit mask: set bit -> color
 * index 1, clear bit -> 0 (transparent). Plane 0 carries the mask; 1-3 = 0. */
static void spr_build_tile_idx1(uint8_t tile[32], const uint8_t mask[8]) {
    memset(tile, 0, 32);
    for (int y = 0; y < 8; y++) tile[2 * y] = mask[y];   /* plane 0 = mask row */
}

/* Overlay state: cursor = sprite 0; bulletholes = a FIFO pool in sprite slots
 * 1..31. Stamping past 31 recycles the oldest (it "jumps" to the new spot) —
 * the 31-sprite-pool demo. */
#define OV_POOL_FIRST  1u
#define OV_POOL_LAST   31u
static struct {
    int      cx, cy;          /* cursor position (screen px) */
    uint8_t  prev_left;       /* left-button edge detect */
    uint8_t  cursor_hidden;   /* hide cursor while right button held */
    unsigned next_hole;       /* FIFO write cursor (1..31) */
    uint32_t rng;             /* LCG for palette variety */
    struct { uint8_t x, y, pal, active; } hole[32];   /* [1..31] used */
} s_ov;

/* Build the full 544 B OAM from the overlay state and push it to the live CW
 * region (the producer's SF1/2/3 OAM slots DMA it to PPU OAM). */
static void fmv_overlay_write_oam(void) {
    uint8_t oam[CW_SPR_OAM_BYTES];
    memset(oam, 0, sizeof oam);
    for (int i = 0; i < 128; i++) oam[i * 4 + 1] = 240u;   /* hide all */

    if (!s_ov.cursor_hidden) {                              /* sprite 0: cursor */
        oam[0] = (uint8_t)s_ov.cx;
        oam[1] = (uint8_t)s_ov.cy;
        oam[2] = (uint8_t)(CW_SPR_TILE_CURSOR & 0xFFu);
        oam[3] = (uint8_t)((3u << 4) | (0u << 1) | ((CW_SPR_TILE_CURSOR >> 8) & 1u)); /* prio3,pal0 */
    }

    for (unsigned i = OV_POOL_FIRST; i <= OV_POOL_LAST; i++) {   /* sprites 1..31: holes */
        if (!s_ov.hole[i].active) continue;
        oam[i * 4 + 0] = s_ov.hole[i].x;
        oam[i * 4 + 1] = s_ov.hole[i].y;
        oam[i * 4 + 2] = (uint8_t)(CW_SPR_TILE_HOLE & 0xFFu);
        oam[i * 4 + 3] = (uint8_t)((2u << 4) | ((s_ov.hole[i].pal & 7u) << 1) | ((CW_SPR_TILE_HOLE >> 8) & 1u));
    }
    cart_window_load_blob(CW_OFF_SPR_OAM, oam, sizeof oam);
}

static void fmv_overlay_setup(void) {
    /* CHR: cursor crosshair (tile 269) + bullethole splat (tile 270). */
    static const uint8_t cursor[8] = { 0x18,0x18,0x18,0xFF,0xFF,0x18,0x18,0x18 };
    static const uint8_t hole[8]   = { 0x3C,0x7E,0xFF,0xFF,0xFF,0xFF,0x7E,0x3C };
    uint8_t chr[CW_SPR_CHR_BYTES];
    spr_build_tile_idx1(chr,      cursor);
    spr_build_tile_idx1(chr + 32, hole);
    cart_window_load_blob(CW_OFF_SPR_CHR, chr, sizeof chr);

    /* OBJ CGRAM (CGADD 128): pal 0 cursor green; pal 1-3 hole colors. OBJ color
     * index 0 is always transparent, so only [pal*16+1] matters here. */
    uint16_t cg[CW_SPR_CGRAM_BYTES / 2];   /* 64 entries = OBJ pal 0-3 */
    memset(cg, 0, sizeof cg);
    cg[0 * 16 + 1] = spr_bgr555( 40, 255,  80);   /* pal 0: bright green cursor */
    cg[1 * 16 + 1] = spr_bgr555(170,  30,  30);   /* pal 1: dark red hole  */
    cg[2 * 16 + 1] = spr_bgr555(110, 110, 110);   /* pal 2: gray hole      */
    cg[3 * 16 + 1] = spr_bgr555(120,  70,  30);   /* pal 3: brown hole     */
    cart_window_load_blob(CW_OFF_SPR_CGRAM, cg, sizeof cg);

    /* Init overlay state + write the initial OAM (cursor centered, no holes). */
    memset(&s_ov, 0, sizeof s_ov);
    s_ov.cx = 124; s_ov.cy = 100;
    s_ov.next_hole = OV_POOL_FIRST;
    s_ov.rng = 0x1234567u;
    fmv_overlay_write_oam();
}

/* Per-SNES-frame (called from on_frame_done): the port-2 SNES Mouse moves the
 * cursor; LEFT click stamps a bullethole (random palette) into the FIFO pool;
 * RIGHT click clears the whole pool and hides the cursor while held. Then
 * rewrite the live OAM. Mouse deltas/buttons come from the embedder via
 * mgapi_post_mouse → cart_window_consume_mouse (bit0=left, bit1=right). */
static void fmv_overlay_tick(void) {
    int dx = 0, dy = 0;
    uint8_t mb = 0;
    cart_window_consume_mouse(&dx, &dy, &mb);
    s_ov.cx += dx;
    s_ov.cy += dy;
    if (s_ov.cx < 8)   s_ov.cx = 8;
    if (s_ov.cx > 240) s_ov.cx = 240;
    if (s_ov.cy < 8)   s_ov.cy = 8;
    if (s_ov.cy > 200) s_ov.cy = 200;

    uint8_t left  = mb & 1u;
    uint8_t right = (mb >> 1) & 1u;

    if (right) {                             /* clear pool + hide cursor while held */
        for (unsigned i = OV_POOL_FIRST; i <= OV_POOL_LAST; i++) s_ov.hole[i].active = 0u;
        s_ov.next_hole = OV_POOL_FIRST;
    }
    s_ov.cursor_hidden = right;

    if (left && !s_ov.prev_left) {           /* left press edge -> stamp a hole */
        s_ov.rng = s_ov.rng * 1664525u + 1013904223u;
        uint8_t pal = (uint8_t)(1u + ((s_ov.rng >> 28) % 3u));   /* pal 1..3 */
        unsigned h = s_ov.next_hole;
        s_ov.hole[h].x = (uint8_t)s_ov.cx;
        s_ov.hole[h].y = (uint8_t)s_ov.cy;
        s_ov.hole[h].pal = pal;
        s_ov.hole[h].active = 1u;
        s_ov.next_hole = (h >= OV_POOL_LAST) ? OV_POOL_FIRST : (h + 1u);
    }
    s_ov.prev_left = left;

    fmv_overlay_write_oam();
}

bool fmv_player_start(int fd) {
    if (fd < 0) return false;
    /* Self-heal: a prior FMV demo that didn't tear down cleanly (e.g. it hung
     * before mg_fmv_stop) would leave s_active set + a stale siphon config in
     * the cart window. Rather than fail and let that poison this run, force a
     * clean teardown first (clears the siphon config, unregisters, frees). */
    if (atomic_load(&s_active)) {
        atomic_store(&s_active, false);
        fmv_player_finalize();
    }

    uint8_t hdr[32];
    if (read_full(fd, hdr, 32) != 32 || memcmp(hdr, "FMV2", 4) != 0) return false;
    uint32_t nframes = rd_u32le(hdr + 12);
    uint32_t abytes  = rd_u32le(hdr + 24);
    if (nframes == 0 || abytes == 0) return false;

    mg_dma_ops_memcpy(&s_dma_ops);
    MgDmaEngine *dma = mg_dma_create(&s_dma_ops);
    if (!dma) return false;

    StreamHandle h = fmv_video_stream_open(fd, abytes, nframes, dma, FMV_RING_DEPTH);
    if (h == STREAM_HANDLE_INVALID) { mg_dma_destroy(dma); return false; }

    s_fd      = fd;
    s_dma     = dma;
    s_video_h = h;
    s_nframes = nframes;
    s_abytes  = abytes;

    /* Live HTIME tuning knob — sweep to land the per-line force-blank in the
     * right pillar / H-blank. Read once per playback. */
    {
        const char *e = getenv("MG_SIPHON_HTIME");
        if (e) { int v = atoi(e); if (v > 0 && v < 256) s_siphon_htime = (uint8_t)v; }
    }

    s_kicked  = false;
    s_sf_idx  = 0;
    s_stop_pending = false;

    /* M1: stage the static cursor/hole CHR + palettes + OAM before releasing
     * to the bsnes thread, so the very first burst already has them resident. */
    fmv_overlay_setup();

    atomic_store(&s_state, FMV_STATUS_PLAYING);
    atomic_store(&s_active, true);    /* release: hands off to the bsnes thread */
    return true;
}

void fmv_player_stop(void) {
    if (!atomic_load(&s_active)) return;
    atomic_store(&s_active, false);    /* bsnes stops dispatching to the FMV */
    cart_window_set_frame_ready(0);    /* kernel stops bursting within a frame */
    s_stop_pending = true;             /* finalize on the next worker tick */
}

void fmv_player_tick(void) {
    if (s_stop_pending) {
        /* The kernel has had a tick with frame_ready=0, so no FRAME_DONE
         * consumer can be in flight — safe to free. */
        fmv_player_finalize();
        s_stop_pending = false;
    }
}

void fmv_player_shutdown(void) {
    atomic_store(&s_active, false);
    s_stop_pending = false;
    fmv_player_finalize();
}

int  fmv_player_status(void) { return atomic_load(&s_state); }
bool fmv_player_active(void) { return atomic_load(&s_active); }

void fmv_player_set_htime(uint8_t htime) {
    if (htime == 0) htime = 1;
    s_siphon_htime = htime;
    /* Push it live so the kernel's @loop caches the new value next frame. */
    cart_window_load_blob(CW_OFF_SIPHON_HTIME, &s_siphon_htime, 1u);
    fprintf(stderr, "fmv: siphon HTIME=%u\n", (unsigned)htime);
    fflush(stderr);
}

static bool fmv_player_try_kickoff(void) {
    if (s_kicked) return true;
    /* Pre-roll: wait until the producer has at least 2 frames buffered so a
     * single SD/file hiccup at startup doesn't immediately starve the SNES.
     * Exception: a short clip whose producer already hit EOF kicks with
     * whatever it managed to buffer (≥1) rather than waiting forever. */
    uint32_t avail = stream_arbiter_chunks_available(s_video_h);
    if (avail < 2u && !stream_arbiter_is_eof(s_video_h)) return false;
    if (avail == 0u) return false;
    if (!stream_arbiter_consume(s_video_h, &s_cur)) return false;
    promote_current();
    s_kicked = true;
    return true;
}

uint8_t fmv_player_frame_ready_byte(void) {
    /* EOF: keep the kernel idle (and don't re-arm a stale frame_ready). */
    if (atomic_load(&s_state) == FMV_STATUS_EOF) return 0u;
    /* Pre-roll: not yet loaded → try to kick; 0 until a frame is resident. */
    if (!s_kicked) return fmv_player_try_kickoff() ? 1u : 0u;
    return 1u;
}

int fmv_player_on_frame_done(void) {
    /* Guard against a stray FRAME_DONE from a pre-FMV (clean_slate) frame
     * arriving before the first kickoff — close it out cleanly. */
    if (!s_kicked) return MG_ADVANCE_EMPTY;

    /* Sprite overlay: update cursor + bullethole pool from the pad and rewrite
     * the live OAM once per SNES frame (this runs every FRAME_DONE). */
    fmv_overlay_tick();

    /* More sub-frames of the active frame? Load the next one + publish its
     * siphon third (src/vram advanced by the new sub-frame index). */
    if (s_sf_idx + 1u < s_cur.subframe_count) {
        s_sf_idx++;
        publish_siphon_config(0u);   /* DIAG: whole-tail single-config (no split) */
        write_subframe_to_cart_window(&s_cur.subframes[s_sf_idx]);
        return MG_ADVANCE_MORE;
    }

    /* Active frame fully bursted — promote the next frame from the ring. */
    if (stream_arbiter_consume(s_video_h, &s_cur)) {
        promote_current();
        return MG_ADVANCE_PROMOTED;
    }

    /* Ring empty. EOF → end playback (clears frame_ready). */
    if (stream_arbiter_is_eof(s_video_h)) {
        atomic_store(&s_state, FMV_STATUS_EOF);
        return MG_ADVANCE_EMPTY;
    }

    /* Producer momentarily behind — re-promote the current frame (a 1-frame
     * stutter, never a tear). With depth-4 lookahead this is rare. */
    promote_current();
    return MG_ADVANCE_PROMOTED;
}
