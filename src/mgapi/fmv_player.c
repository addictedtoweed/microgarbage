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
