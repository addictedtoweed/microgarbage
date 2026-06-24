/* ============================================================
 *  audio_init.c — bring up AudioService inside mgapi.
 *
 *  Owns three pieces of state:
 *
 *    g_channel   — the request/response transport between VM ecall
 *                  handlers and the audio service.
 *
 *    g_service   — the AudioService instance, configured against
 *                  mgapi's 4 MB PSRAM audio slice. Drained + rendered
 *                  by mgapi_audio_pump on the mgapi worker thread.
 *
 *    g_ring      — small stereo int16 SPSC ring between
 *                  mgapi_audio_pump (producer, mgapi worker thread)
 *                  and mgapi_audio_drain (consumer, bsnes-plus
 *                  thread via mgapi_audio_pull). Cursors are atomic
 *                  with acquire/release ordering — single-producer,
 *                  single-consumer, no mutex.
 *
 *  v1.69: audio worker thread retired. The mgapi worker (see
 *  worker.c) calls mgapi_audio_pump once per emulated vblank,
 *  which both drains the channel and refills the ring.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "audio_init.h"

#include "audio/audio_service.h"
#include "audio/audio_sink.h"
#include "vm/service_channel.h"
#include "vm/vm_host_audio.h"

#if defined(_WIN32)
#include "audio_device_win32.h"
#endif

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
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

/* Shared staging buffer for SYS_AUDIO_LOAD_SAMPLE / LOAD_STAGED. 64KB
 * matches what examples/05_shell/host_audio.c uses; plenty for the
 * small SFX our demos load. Streaming WAVs don't use staging -- they
 * read incrementally via the AudioFileReader callbacks. */
/* v2.00 reverted v1.99's 512 KB bump now that SFX stage at NATIVE
 * rate (mixer interpolates per-channel). 64 KB matches 05_shell and
 * fits ~1.5 sec mono 22050 SFX comfortably; longer assets stream. */
#define MGAPI_AUDIO_STAGING_BYTES (64u * 1024u)
static uint8_t g_staging[MGAPI_AUDIO_STAGING_BYTES];

/* ----------------------------------------------------------------
 *  SPSC ring buffer between mgapi worker (producer) and bsnes-plus
 *  thread (consumer via mgapi_audio_pull).
 *
 *  Cursors are atomic with acquire/release semantics — the consumer
 *  loads `w` with acquire to synchronize with the producer's release
 *  store, then reads from g_ring[]. The producer mirrors the pattern
 *  for `r`. No mutex; the SPSC invariant + atomic ordering is enough.
 *
 *  Stereo int16, size = power of two so cursor arithmetic stays
 *  cheap. Plain unsigned cursors that overflow naturally —
 *  (w - r) gives the count via modular arithmetic.
 * ---------------------------------------------------------------- */
#define AUDIO_RING_FRAMES  16384u   /* ~371 ms @ 44.1 kHz, ~341 ms @ 48 kHz */
#define AUDIO_RING_MASK    (AUDIO_RING_FRAMES - 1u)
_Static_assert((AUDIO_RING_FRAMES & AUDIO_RING_MASK) == 0,
               "ring size must be a power of two");

static int16_t       g_ring[AUDIO_RING_FRAMES * 2];   /* L,R interleaved */
static atomic_uint   g_ring_w;    /* writes (frames; wraps at u32) */
static atomic_uint   g_ring_r;    /* reads  (frames; wraps at u32) */

/* Producer-side: load consumer's r with acquire, the rest from local
 * knowledge. Consumer mirrors: load producer's w with acquire. */
static inline uint32_t ring_used_from_consumer(void) {
    uint32_t w = atomic_load_explicit(&g_ring_w, memory_order_acquire);
    uint32_t r = atomic_load_explicit(&g_ring_r, memory_order_relaxed);
    return w - r;
}
static inline uint32_t ring_used_from_producer(void) {
    uint32_t w = atomic_load_explicit(&g_ring_w, memory_order_relaxed);
    uint32_t r = atomic_load_explicit(&g_ring_r, memory_order_acquire);
    return w - r;
}
static inline uint32_t ring_free_from_producer(void) {
    return AUDIO_RING_FRAMES - ring_used_from_producer();
}

/* Render batch — small enough to fit a scratch buffer on the worker
 * thread's stack without thinking, large enough that the inner copy
 * loop amortizes nicely. */
#define AUDIO_RENDER_QUANTUM 512u  /* matches examples/05_shell — single
                                      sink_write per loop iteration, ~11.6 ms
                                      of audio at 44.1 kHz */

/* v1.89: direct Win32 audio output path.
 *
 * The bsnes-plus embedder's audio pipeline (Stream::sample → Audio::flush
 * → system.interface->audio_sample) turned out to have several layers
 * of subtle DSP-gating, sample-rate mismatch, and Qt audio-driver
 * issues that we couldn't reliably untangle even with the v1.88
 * Audio::flush fix and the Makefile-dep correction. Rather than keep
 * fighting bsnes-plus's pipeline, mgapi now opens its own waveOut
 * device and writes mixed audio directly to it. The bsnes-plus side
 * still calls mgapi_audio_pull, but we return silence — bsnes is no
 * longer the audio output path, just an open ABI hook we don't use.
 *
 * This is closer to the MCU twin model anyway: on the H745 mgapi
 * will drive an I2S codec directly, not pipe samples through the
 * SNES bus. Doing the same on Windows keeps the host/embedder seam
 * thin (cart-bus + video + input) and makes audio fully mgapi's
 * responsibility, where the mixer + FFT + ring already live. */
static AudioSink g_sink;
static bool      g_sink_open;
static uint32_t  g_effective_rate_hz;   /* sink rate (device-paced) */

/* v1.96 ring-to-sink resampler state.
 *
 * The mixer renders at MIXER_RATE_HZ (44100). The sink wants
 * g_effective_rate_hz frames per second. We use a tiny linear
 * resampler in the playback thread: a q32.32 phase accumulator
 * advances by step = MIXER_RATE_HZ / sink_rate per output frame;
 * when the integer part rolls over, we consume one input frame.
 * For sink_rate == MIXER_RATE_HZ the resampler degenerates to a
 * straight copy (fast-path-checked in playback_body).
 *
 * State is per-stream — we have one stream (the ring), so static
 * globals are fine. */
#define MIXER_RATE_HZ 44100u
static uint64_t g_rsmp_phase;       /* q32.32, [0, 1<<32) */
static int16_t  g_rsmp_prev_l;      /* last-consumed input frame (L) */
static int16_t  g_rsmp_prev_r;      /* last-consumed input frame (R) */
static int16_t  g_rsmp_curr_l;      /* current input frame (L)       */
static int16_t  g_rsmp_curr_r;      /* current input frame (R)       */
static bool     g_rsmp_primed;      /* has curr been loaded?         */

/* ----------------------------------------------------------------
 *  Audio worker thread (v1.81 restoration)
 *
 *  v1.69 folded audio_service_process into mgapi_audio_pump on the
 *  main mgapi worker thread. That created a deadlock for any guest
 *  ecall that posts to the audio channel and then blocks waiting
 *  for the response: the VM runs on the same worker thread that
 *  was supposed to drain the channel, so the response never
 *  arrives and channel_request_call times out at 1000 ms each.
 *  audio_get_levels-per-frame turns 60 fps into 1 fps; the demo's
 *  black-screen-until-button symptom was the early frames hitting
 *  the timeout.
 *
 *  Fix: a dedicated thread that runs audio_service_process +
 *  audio_service_render in a loop, blocking on the channel's
 *  condition variable when idle (via channel_provider_wait — sub-
 *  millisecond response, sidesteps the Sleep(5)-on-Windows-is-15ms
 *  problem that bit the earlier worker design). The main mgapi
 *  worker thread no longer touches the audio service.
 * ---------------------------------------------------------------- */
static atomic_int g_audio_stop;
#if defined(_WIN32)
  #include <windows.h>
  static HANDLE g_audio_thread;
#else
  #include <pthread.h>
  static pthread_t g_audio_thread;
  static int g_audio_thread_started;
#endif

/* v1.98: single-thread "05_shell model".
 *
 * History: v1.81 (the dual-thread design) split process+render from
 * sink_write into separate threads because a guest ecall blocking on
 * the audio channel could deadlock if the same thread that should
 * drain the channel was stuck inside sink_write. That was correct
 * once but I overcorrected — what's needed is for the audio thread
 * to be distinct from the bsnes-plus thread AND the VM thread, NOT
 * a second internal split between worker and playback.
 *
 * examples/05_shell/host_audio.c proves it: one audio thread doing
 * process(64) → render(QUANTUM) → sink_write(QUANTUM) in a tight
 * loop. sink_write blocks at the device's drain rate, which IS the
 * pacing — no ring, no resampler, no playback thread. The mixer
 * runs at the rate we open the sink at; if the device's native rate
 * differs, the WASAPI sink's AUTOCONVERTPCM flag lets Audio Engine
 * handle SRC for us.
 *
 * v1.91-1.97 ate the 76% deficit because the dual-thread + ring +
 * partial-quantum + resampler interaction was producing artifacts
 * that mimicked a rate mismatch — not because waveOut/WASAPI were
 * actually bad. The simple loop here is what every working audio
 * thread in this codebase does (audio_stress.c, host_audio.c, the
 * mgapi tests). */
static void audio_worker_body(void) {
    int16_t mix_buf[AUDIO_RENDER_QUANTUM * 2];

    /* Resample setup: mixer at MIXER_RATE_HZ → sink at g_effective_rate_hz.
     * For MCU (effective == mixer) the loop short-circuits to a single
     * sink_write of mix_buf — no resampler cost. For Windows where the
     * device wants 48 kHz, sink_buf is sized to hold the upsampled
     * output. The +8 slop handles any rounding in the per-iteration
     * output count. */
    const uint32_t mixer_rate = MIXER_RATE_HZ;
    const uint32_t sink_rate  = g_effective_rate_hz
                                  ? g_effective_rate_hz : MIXER_RATE_HZ;
    const bool passthrough    = (sink_rate == mixer_rate);
    /* Worst-case output frames per QUANTUM = ceil(QUANTUM * sink/mixer)
     * + a few for rounding. Stack-sized; ~3 KB at 48k. */
    const uint32_t sink_buf_cap =
        ((uint64_t)AUDIO_RENDER_QUANTUM * (uint64_t)sink_rate
         + (mixer_rate - 1u)) / (uint64_t)mixer_rate + 8u;
    int16_t *sink_buf = malloc(sink_buf_cap * 2 * sizeof(int16_t));
    if (!sink_buf) {
        fprintf(stderr, "mgapi audio: worker malloc failed (%u frames)\n",
                (unsigned)sink_buf_cap);
        fflush(stderr);
        return;
    }

    /* Resampler state — persists across iterations for continuity. */
    uint64_t step  = ((uint64_t)mixer_rate << 32) / (uint64_t)sink_rate;
    uint64_t phase = 0;
    int16_t  prev_l = 0, prev_r = 0;   /* last consumed input frame */

    /* Production-rate sanity check — one line at 2 sec. */
    uint64_t frames_total = 0;
    LARGE_INTEGER freq, t0, tnow;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    bool rate_logged = false;

    while (!atomic_load_explicit(&g_audio_stop, memory_order_acquire)) {
        if (!g_service) break;

        audio_service_process(g_service, 64);
        audio_service_render(g_service, mix_buf, AUDIO_RENDER_QUANTUM);

        if (!g_sink_open) {
            (void)channel_provider_wait(&g_channel, 1);
            continue;
        }

        if (passthrough) {
            /* Mixer rate == sink rate: write straight through. */
            if (audio_sink_write(&g_sink, mix_buf,
                                  AUDIO_RENDER_QUANTUM) < 0) break;
            frames_total += AUDIO_RENDER_QUANTUM;
        } else {
            /* Resample mixer→sink with linear interpolation. Each
             * output frame advances phase by `step`; when phase rolls
             * over an integer boundary we consume one input frame. */
            uint32_t in_idx = 0;
            uint32_t out_n  = 0;
            while (in_idx < AUDIO_RENDER_QUANTUM && out_n < sink_buf_cap) {
                while (phase >= ((uint64_t)1 << 32)) {
                    prev_l = mix_buf[in_idx * 2 + 0];
                    prev_r = mix_buf[in_idx * 2 + 1];
                    in_idx++;
                    phase -= (uint64_t)1 << 32;
                    if (in_idx >= AUDIO_RENDER_QUANTUM) goto flush;
                }
                int32_t curr_l = mix_buf[in_idx * 2 + 0];
                int32_t curr_r = mix_buf[in_idx * 2 + 1];
                uint32_t frac = (uint32_t)phase;
                int32_t ol = (int32_t)prev_l
                           + (int32_t)(((int64_t)(curr_l - (int32_t)prev_l)
                                        * (int64_t)frac) >> 32);
                int32_t or_ = (int32_t)prev_r
                           + (int32_t)(((int64_t)(curr_r - (int32_t)prev_r)
                                        * (int64_t)frac) >> 32);
                sink_buf[out_n * 2 + 0] = (int16_t)ol;
                sink_buf[out_n * 2 + 1] = (int16_t)or_;
                out_n++;
                phase += step;
            }
flush:
            if (out_n > 0) {
                if (audio_sink_write(&g_sink, sink_buf, out_n) < 0) break;
                frames_total += out_n;
            }
        }

        if (!rate_logged) {
            QueryPerformanceCounter(&tnow);
            double elapsed = (double)(tnow.QuadPart - t0.QuadPart)
                             / (double)freq.QuadPart;
            if (elapsed >= 2.0) {
                rate_logged = true;
                fprintf(stderr,
                        "mgapi audio: worker rate=%.0f fps "
                        "(mixer %u → sink %u)\n",
                        (double)frames_total / elapsed,
                        (unsigned)mixer_rate, (unsigned)sink_rate);
                fflush(stderr);
            }
        }
    }
    free(sink_buf);
}

/* v1.98: the playback thread, ring-buffer drain, and linear resampler
 * that lived here through v1.97 are GONE. The audio worker now calls
 * audio_sink_write inline — the sink IS the clock, the same architecture
 * as examples/05_shell/host_audio.c. The ring (g_ring) and resampler
 * state above are kept compiled because mgapi_audio_drain still references
 * them when bsnes-plus's stream path asks for samples (it gets nothing
 * because g_sink_open is true), but no producer feeds them. They could
 * be deleted in a follow-up cleanup. */

#if defined(_WIN32)
static DWORD WINAPI audio_worker_entry(LPVOID u) {
    (void)u; audio_worker_body(); return 0;
}
static int audio_worker_start(void) {
    atomic_store(&g_audio_stop, 0);
    g_audio_thread = CreateThread(NULL, 0, audio_worker_entry, NULL, 0, NULL);
    if (g_audio_thread) {
        /* v1.86: bump the audio worker above NORMAL so routine background
         * work (filesystem indexers, Windows scheduled tasks, even the
         * GUI thread doing layout) can't preempt us long enough to drain
         * the SPSC ring below bsnes-plus's pull cadence. ABOVE_NORMAL is
         * the standard Windows audio-thread treatment: enough to keep
         * the producer running predictably without starving foreground
         * work the way TIME_CRITICAL would. */
        SetThreadPriority(g_audio_thread, THREAD_PRIORITY_ABOVE_NORMAL);
    }
    return g_audio_thread ? 0 : -1;
}
static void audio_worker_stop(void) {
    atomic_store_explicit(&g_audio_stop, 1, memory_order_release);
    /* Kick the CV so the worker wakes even mid-wait. */
    if (g_channel.transport.notify) {
        g_channel.transport.notify(g_channel.transport.ctx);
    }
    if (g_audio_thread) {
        WaitForSingleObject(g_audio_thread, INFINITE);
        CloseHandle(g_audio_thread);
        g_audio_thread = NULL;
    }
}
#else
static void *audio_worker_entry(void *u) {
    (void)u; audio_worker_body(); return NULL;
}
static int audio_worker_start(void) {
    atomic_store(&g_audio_stop, 0);
    if (pthread_create(&g_audio_thread, NULL, audio_worker_entry, NULL) != 0) {
        return -1;
    }
    g_audio_thread_started = 1;
    return 0;
}
static void audio_worker_stop(void) {
    atomic_store_explicit(&g_audio_stop, 1, memory_order_release);
    if (g_channel.transport.notify) {
        g_channel.transport.notify(g_channel.transport.ctx);
    }
    if (g_audio_thread_started) {
        pthread_join(g_audio_thread, NULL);
        g_audio_thread_started = 0;
    }
}
#endif

/* AudioFileReader implementation: bare stdio fopen/fread/fseek/fclose.
 * The audio service's request handler builds the absolute host path
 * by joining host_fs_root (e.g. "./host") with the guest's "/host/foo
 * .wav" remainder, so by the time afr_open sees it the path is just
 * a regular file the OS can open. */
typedef struct { FILE *fp; } AfrFile;

static void *afr_open(void *ctx, const char *path) {
    (void)ctx;
    if (!path) return NULL;
    AfrFile *f = (AfrFile *)calloc(1, sizeof(*f));
    if (!f) return NULL;
    f->fp = fopen(path, "rb");
    if (!f->fp) { free(f); return NULL; }
    return f;
}
static uint32_t afr_read(void *ctx, void *fh, void *dst, uint32_t bytes) {
    (void)ctx;
    AfrFile *f = (AfrFile *)fh;
    if (!f || !f->fp) return 0;
    return (uint32_t)fread(dst, 1, bytes, f->fp);
}
static bool afr_seek(void *ctx, void *fh, uint32_t off) {
    (void)ctx;
    AfrFile *f = (AfrFile *)fh;
    if (!f || !f->fp) return false;
    return fseek(f->fp, (long)off, SEEK_SET) == 0;
}
static void afr_close(void *ctx, void *fh) {
    (void)ctx;
    AfrFile *f = (AfrFile *)fh;
    if (!f) return;
    if (f->fp) fclose(f->fp);
    free(f);
}

/* Ring buffer + helpers live further up so the worker body can see
 * them (the worker is the producer now; previously the pump on the
 * embedder thread was). */

/* ----------------------------------------------------------------
 *  Init / shutdown
 * ---------------------------------------------------------------- */

int mgapi_audio_init(void *pool_region, size_t pool_region_size,
                     uint32_t requested_rate_hz) {
    if (g_service) return -EALREADY;
    if (!pool_region || pool_region_size == 0) return -EINVAL;

    /* v1.94: pick the effective sample rate. requested_rate_hz == 0
     * means "ask the host" — on Windows we query the WASAPI default
     * render endpoint for its mix-format rate (what Audio Engine
     * actually drives the device at after shared-mode mixing). On
     * other platforms (MCU build) there is no device to ask, so a
     * caller passing 0 there is a configuration bug — we'd fall back
     * to a compile-time default that may not match the I2S clock.
     * The audio service mixer and the waveOut sink BOTH open at this
     * rate, so source streams resample once on the way in (where the
     * music_player already does it cheaply) instead of relying on the
     * OS to resample on the way out. */
    uint32_t effective_rate_hz = requested_rate_hz;
    const char *rate_source = "caller";
    if (effective_rate_hz == 0) {
#if defined(_WIN32)
        uint32_t device_rate = 0;
        if (mgapi_query_default_audio_rate(&device_rate) && device_rate >= 8000) {
            effective_rate_hz = device_rate;
            rate_source = "WASAPI default endpoint";
        } else {
            effective_rate_hz = 44100;
            rate_source = "fallback (WASAPI query failed)";
        }
#else
        effective_rate_hz = 44100;
        rate_source = "fallback (no device query on this platform)";
#endif
    }
    fprintf(stderr,
            "mgapi audio: effective rate = %u Hz (source: %s)\n",
            (unsigned)effective_rate_hz, rate_source);
    fflush(stderr);
    g_effective_rate_hz = effective_rate_hz;

    ChannelTransport tr;
    if (!channel_thread_transport_make(&tr)) return -ENOMEM;

    if (!service_channel_init(&g_channel,
                              g_req_ring, g_resp_ring,
                              MGAPI_AUDIO_CHANNEL_SLOTS, &tr)) {
        tr.destroy(tr.ctx);
        return -ENOMEM;
    }

    /* Wire the staging buffer and file_reader so the service can
     * service SYS_AUDIO_LOAD_SAMPLE and SYS_AUDIO_STREAM_WAV requests
     * the VM ecall handlers post. The file reader is stdio-based;
     * vm_host_install_audio's SYS_AUDIO_LOAD_WAV handler resolves
     * guest "/host/foo.wav" to a host path before calling afr_open.
     * track_count=16 mirrors the shell host's choice. */
    AudioServiceConfig cfg = {
        .channel          = &g_channel,
        .pool_region      = pool_region,
        .pool_region_size = pool_region_size,
        /* v1.96: pin the MIXER at 44100, independent of the device rate.
         *
         * Why not just use effective_rate_hz here? The audio_service
         * mixer creates its channels with source_sample_rate = 0
         * (calloc default), so the mixer treats each channel's source
         * as if it already runs at the mixer's output rate — no per-
         * channel resample. That worked while output rate == source
         * rate == 44100, but the moment we asked the mixer to run at
         * 48000 (to match a 48k device), SFX at 22050 played at 2.18×
         * speed and music at 44100 played at 1.09× — both audibly
         * wrong. Rather than thread a source-rate update through every
         * SFX/music play path (touches arbiter, music_player, etc.),
         * we keep the mixer at 44100 and resample 44100 → device rate
         * in the playback thread instead. One resample point, no
         * mixer surgery, all source rates work as before. */
        .sample_rate      = 44100,
        .track_count      = 16,
        .staging_buffer   = g_staging,
        .staging_capacity = sizeof(g_staging),
        .file_reader      = { afr_open, afr_read, afr_seek, afr_close, NULL },
    };

    g_service = audio_service_create(&cfg);
    if (!g_service) {
        service_channel_destroy(&g_channel);
        return -ENOMEM;
    }

    /* DIAGNOSTIC v1.64: worker thread DISABLED while isolating the
     * 1-fps emulation problem. Without the worker, VM-side audio
     * ecalls (mg_sfx_load, mg_stream_play, audio_get_levels) would
     * block on their channel response and timeout after 1 second --
     * but demo_audio_mixer's v1.63 build makes ZERO audio ecalls, so
     * that's fine. If THIS build runs at 60 fps with visible text,
     * the worker thread (or its CPU competition with bsnes-plus) was
     * the culprit. */
    atomic_store(&g_ring_w, 0u);
    atomic_store(&g_ring_r, 0u);

    /* v1.89: open the direct Win32 waveOut sink. See the comment at
     * the AudioSink declaration for why we bypass bsnes-plus's audio
     * pipeline entirely. Failure here is non-fatal — the service still
     * runs (so FFT works, mixing works), the ring still fills for any
     * embedder that calls mgapi_audio_pull, just nothing comes out the
     * speakers. We log loudly because that's the case we'd never want
     * to silently ship. */
    /* v1.99: open WASAPI at the DEVICE'S preferred rate (typically
     * 48 kHz on Win11), NOT the mixer rate. v1.98 tried opening at
     * 44100 with AUTOCONVERTPCM expecting Audio Engine to resample,
     * but on the user's chipset it played 44100 buffers at the device's
     * 48000 rate — music came out 1.088× too fast. Opening at the
     * device rate ensures the engine accepts our buffer 1-for-1, and
     * the audio worker does the 44100 → device rate resample inline
     * before sink_write. The MCU twin still passes 44100 explicitly
     * and the resampler short-circuits to a memcpy in that case. */
    uint32_t sink_rate_hz = effective_rate_hz;
    if (audio_sink_open(&g_sink, "wasapi", NULL, sink_rate_hz)) {
        g_sink_open = true;
        fprintf(stderr,
                "mgapi audio: WASAPI sink opened (%u Hz s16 stereo)\n",
                (unsigned)sink_rate_hz);
        fflush(stderr);
#if defined(_WIN32)
        /* Higher Windows timer resolution helps Audio Engine pacing
         * even though we're event-driven inside the WASAPI sink — it
         * also helps anything else our threads might Sleep on. */
        timeBeginPeriod(1);
#endif
    } else {
        g_sink_open = false;
        fprintf(stderr,
                "mgapi audio: WARNING — sink failed to open; "
                "audio will be silent unless the embedder pulls the ring\n");
        fflush(stderr);
    }

    /* v1.81: bring up the dedicated audio worker thread. Without it
     * any VM ecall that channel_request_call's into the service
     * deadlocks (the only thread that could produce the response IS
     * the thread waiting for it). */
    if (audio_worker_start() != 0) {
        fprintf(stderr, "mgapi audio: worker thread FAILED to start\n");
        fflush(stderr);
        audio_service_destroy(g_service);
        service_channel_destroy(&g_channel);
        g_service = NULL;
        return -ENOMEM;
    }
    fprintf(stderr, "mgapi audio: service up + worker thread running\n");
    fflush(stderr);
    return 0;
}

bool mgapi_audio_install_ecalls(VmSystem *sys, const char *host_fs_root) {
    if (!g_service || !sys) {
        fprintf(stderr, "mgapi audio: install_ecalls precondition fail "
                        "(g_service=%p sys=%p)\n",
                (void *)g_service, (void *)sys);
        fflush(stderr);
        return false;
    }
    VmHostAudioConfig hcfg = {
        .channel          = &g_channel,
        .staging_buffer   = g_staging,
        .staging_capacity = sizeof(g_staging),
        .call_timeout_ms  = 1000,
        .host_fs_root     = host_fs_root,
    };
    bool ok = vm_host_install_audio(sys, &hcfg);
    fprintf(stderr,
            "mgapi audio: vm_host_install_audio -> %s (host_fs_root=%s)\n",
            ok ? "OK" : "FAILED", host_fs_root ? host_fs_root : "(null)");
    fflush(stderr);
    return ok;
}

void mgapi_audio_shutdown(void) {
    if (!g_service) return;
    /* v1.98: single-thread teardown. Worker thread owns sink_write
     * inline; stop it first so any blocked write returns, then close
     * the sink, then destroy the service. */
    audio_worker_stop();
    if (g_sink_open) {
        audio_sink_close(&g_sink);
        g_sink_open = false;
#if defined(_WIN32)
        /* Pair with timeBeginPeriod(1) at init. */
        timeEndPeriod(1);
#endif
    }
    audio_service_destroy(g_service);
    service_channel_destroy(&g_channel);
    g_service = NULL;
    atomic_store(&g_ring_w, 0u);
    atomic_store(&g_ring_r, 0u);
    /* Reset resampler state — a fresh init starts clean. */
    g_rsmp_phase  = 0;
    g_rsmp_primed = false;
    g_rsmp_prev_l = g_rsmp_prev_r = 0;
    g_rsmp_curr_l = g_rsmp_curr_r = 0;
}

/* ----------------------------------------------------------------
 *  Pump (writer) and drain (reader)
 * ---------------------------------------------------------------- */

/* v1.81: no-op. Service processing + ring rendering happen on the
 * dedicated audio worker thread (see audio_worker_body above). The
 * mgapi main worker thread no longer touches the audio service or
 * ring, so the per-vblank pump call is harmless dead code; kept
 * exported only because mgapi_step_body still calls it during the
 * v1.68 worker tick. Future cleanup: remove the call from
 * mgapi_step_body and drop this export entirely. */
void mgapi_audio_pump(uint32_t frames) {
    (void)frames;
}

/* Consumer side. Called from the bsnes-plus thread via
 * mgapi_audio_pull. SPSC-paired with the audio worker above.
 *
 * v1.90: when the direct waveOut sink is active, the playback thread
 * already owns the SPSC ring as the sole consumer — we MUST NOT let
 * bsnes-plus drain from the same ring or both consumers fight over
 * sample reads (= sub-rate audio, glitches, and the symptom that
 * looked like "slow audio" in v1.89). Return zero frames so the
 * bsnes-plus stream path silently plays silence; the real audio path
 * runs through waveOut. */
uint32_t mgapi_audio_drain(int16_t *dst_stereo, uint32_t frames) {
    if (!g_service || !dst_stereo) return 0;
    if (g_sink_open) return 0;   /* direct sink owns the ring */

    uint32_t have = ring_used_from_consumer();
    if (frames > have) frames = have;
    if (frames == 0) return 0;

    uint32_t r = atomic_load_explicit(&g_ring_r, memory_order_relaxed);
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t pos = (r + i) & AUDIO_RING_MASK;
        dst_stereo[i * 2 + 0] = g_ring[pos * 2 + 0];
        dst_stereo[i * 2 + 1] = g_ring[pos * 2 + 1];
    }
    atomic_store_explicit(&g_ring_r, r + frames, memory_order_release);
    return frames;
}

uint32_t mgapi_audio_ring_used(void)     { return ring_used_from_consumer(); }
uint32_t mgapi_audio_ring_capacity(void) { return AUDIO_RING_FRAMES; }

/* #73: expose the audio service's FMV clip-audio ring (NULL if audio isn't up). */
AudioRingStream *mgapi_audio_fmv_ring(void) {
    return g_service ? audio_service_fmv_ring(g_service) : NULL;
}

/* #73: SNES master-clock feedback for FMV A/V drift sync. The embedder calls
 * this from its audio-output cadence (mgapi_audio_pull) — once per SNES output
 * sample. bsnes' Enter loop and the mgapi mixer are BOTH pinned at 44100, so
 * the cumulative pull count IS the SNES master clock in mixer-rate units (no
 * scaling). We publish it into the FMV ring; audio_service_render feeds it to
 * the mixer's drift PLL so the WASAPI-paced audio tracks the SNES clock. This
 * is the emulator twin of the H745's hardware SNES-master-clock feedback. */
static _Atomic uint64_t g_snes_clock;

void mgapi_audio_note_snes_clock(uint32_t frames) {
    if (!g_service || frames == 0u) return;
    uint64_t now = atomic_fetch_add_explicit(&g_snes_clock, frames,
                                             memory_order_relaxed) + frames;
    AudioRingStream *r = audio_service_fmv_ring(g_service);
    if (r) audio_ring_stream_set_external_clock(r, now);
}
