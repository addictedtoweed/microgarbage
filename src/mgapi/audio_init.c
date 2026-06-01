/* ============================================================
 *  audio_init.c — bring up AudioService inside mgapi.
 *
 *  Owns three pieces of state:
 *
 *    g_channel   — the request/response transport between the
 *                  future VM ecall handlers and the service. In
 *                  stage 2b1 nothing posts to it; the wiring is
 *                  here so stage 3's VM lands on a working seam.
 *
 *    g_service   — the AudioService instance, configured against
 *                  mgapi's 4 MB PSRAM audio slice. Runs synchronously
 *                  in the embedder's thread (no worker thread).
 *
 *    g_ring      — small stereo int16 ring between mgapi_audio_pump
 *                  (writer, from mgapi_step) and mgapi_audio_drain
 *                  (reader, from mgapi_audio_pull). Single-threaded,
 *                  so plain cursors — no atomics needed.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "audio_init.h"

#include "audio/audio_service.h"
#include "vm/service_channel.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------------------------------------------
 *  Channel storage
 *
 *  Same storage shape as examples/05_shell/host.c uses: two
 *  power-of-two ChannelMsg rings (request + response). 32 slots is
 *  plenty — stage 3's VM will only queue a handful at a time. */
#define MGAPI_AUDIO_CHANNEL_SLOTS  32

extern bool channel_thread_transport_make(ChannelTransport *out);
/* `channel_thread_transport_make` is declared in include/vm/channel_thread.h
 * but we don't include that here so this module can compile in either the
 * pthread (Cygwin) or Win32 (mingw) backend — the symbol is provided by
 * exactly one of channel_thread.c / channel_win32.c at link time. */

static ChannelMsg     g_req_ring[MGAPI_AUDIO_CHANNEL_SLOTS];
static ChannelMsg     g_resp_ring[MGAPI_AUDIO_CHANNEL_SLOTS];
static ServiceChannel g_channel;

/* g_service is the alive marker: init is atomic from the caller's
 * perspective (either everything succeeds and g_service is non-NULL,
 * or any step fails and we roll back everything we acquired). No
 * separate "channel alive" flag is needed because if g_service is
 * NULL, either we never started init or we failed mid-init and
 * already cleaned the channel up. */
static AudioService  *g_service;

/* ----------------------------------------------------------------
 *  Ring buffer between pump and drain
 *
 *  Stereo int16, size = power of two so cursor arithmetic stays
 *  cheap. Plain unsigned cursors that overflow naturally —
 *  (write - read) gives the count via modular arithmetic. */

#define AUDIO_RING_FRAMES  16384u   /* ~371 ms at 44.1 kHz */
#define AUDIO_RING_MASK    (AUDIO_RING_FRAMES - 1u)
_Static_assert((AUDIO_RING_FRAMES & AUDIO_RING_MASK) == 0,
               "ring size must be a power of two");

static int16_t  g_ring[AUDIO_RING_FRAMES * 2];   /* L,R interleaved */
static uint32_t g_ring_w;    /* writes (frames; unmasked, wraps at u32) */
static uint32_t g_ring_r;    /* reads  (frames; unmasked, wraps at u32) */

static inline uint32_t ring_used(void) { return g_ring_w - g_ring_r; }
static inline uint32_t ring_free(void) { return AUDIO_RING_FRAMES - ring_used(); }

/* ----------------------------------------------------------------
 *  Init / shutdown
 * ---------------------------------------------------------------- */

int mgapi_audio_init(void *pool_region, size_t pool_region_size) {
    if (g_service) return -EALREADY;
    if (!pool_region || pool_region_size == 0) return -EINVAL;

    ChannelTransport tr;
    if (!channel_thread_transport_make(&tr)) return -ENOMEM;

    if (!service_channel_init(&g_channel,
                              g_req_ring, g_resp_ring,
                              MGAPI_AUDIO_CHANNEL_SLOTS, &tr)) {
        tr.destroy(tr.ctx);
        return -ENOMEM;
    }

    /* No staging buffer (no LOAD_STAGED yet) and no file reader
     * (no STREAM_WAV yet). Both null -> the service rejects those
     * requests cleanly, which matches our "no clients" state.
     * track_count=16 mirrors the shell host's choice. */
    AudioServiceConfig cfg = {
        .channel          = &g_channel,
        .pool_region      = pool_region,
        .pool_region_size = pool_region_size,
        .sample_rate      = 44100,
        .track_count      = 16,
        .staging_buffer   = NULL,
        .staging_capacity = 0,
        .file_reader      = { NULL, NULL, NULL, NULL, NULL },
    };

    g_service = audio_service_create(&cfg);
    if (!g_service) {
        service_channel_destroy(&g_channel);
        return -ENOMEM;
    }

    g_ring_w = 0;
    g_ring_r = 0;
    return 0;
}

void mgapi_audio_shutdown(void) {
    if (!g_service) return;
    audio_service_destroy(g_service);
    service_channel_destroy(&g_channel);
    g_service = NULL;
    g_ring_w = 0;
    g_ring_r = 0;
}

/* ----------------------------------------------------------------
 *  Pump (writer) and drain (reader)
 * ---------------------------------------------------------------- */

void mgapi_audio_pump(uint32_t frames) {
    if (!g_service) return;

    /* Drain any pending channel requests first. Stage 2b1: no
     * requests in flight; this is here so stage 3's VM lands on a
     * working seam without us editing the pump function. */
    audio_service_process(g_service, 64);

    /* Clamp to ring capacity. If the embedder is slow to drain we
     * silently drop the excess — better than overwriting unread
     * frames. The mixer continues; only the unrendered chunk is
     * lost, which the embedder hears as a missed sample window. */
    uint32_t avail = ring_free();
    if (frames > avail) frames = avail;
    if (frames == 0) return;

    /* Render in chunks. 256-frame scratch keeps stack usage tiny
     * (1 KB) and matches the mixer's typical batch granularity. */
    int16_t scratch[256 * 2];
    while (frames > 0) {
        uint32_t chunk = frames > 256u ? 256u : frames;
        audio_service_render(g_service, scratch, chunk);
        for (uint32_t i = 0; i < chunk; i++) {
            uint32_t pos = (g_ring_w + i) & AUDIO_RING_MASK;
            g_ring[pos * 2 + 0] = scratch[i * 2 + 0];
            g_ring[pos * 2 + 1] = scratch[i * 2 + 1];
        }
        g_ring_w += chunk;
        frames -= chunk;
    }
}

uint32_t mgapi_audio_drain(int16_t *dst_stereo, uint32_t frames) {
    if (!g_service || !dst_stereo) return 0;
    uint32_t have = ring_used();
    if (frames > have) frames = have;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t pos = (g_ring_r + i) & AUDIO_RING_MASK;
        dst_stereo[i * 2 + 0] = g_ring[pos * 2 + 0];
        dst_stereo[i * 2 + 1] = g_ring[pos * 2 + 1];
    }
    g_ring_r += frames;
    return frames;
}

uint32_t mgapi_audio_ring_used(void)     { return ring_used(); }
uint32_t mgapi_audio_ring_capacity(void) { return AUDIO_RING_FRAMES; }
