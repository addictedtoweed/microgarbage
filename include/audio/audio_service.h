/* ============================================================
 *  audio_service.h — the audio co-processor service
 *
 *  This is the thing that runs on the M4 (or the desktop worker
 *  thread): it owns the audio subsystem and processes REQ_AUDIO_*
 *  messages drained from the service channel, posting responses.
 *  It ties together every prior audio step:
 *
 *      channel  (step 1+2)  — how requests/responses travel
 *      pool     (step 3)    — sample data at rest (object handles)
 *      adapter  (step 4)    — pool -> music player stream source
 *      arbiter  (step 5)    — object handles -> voice handles, tracks
 *      mixer/music_player   — the existing DSP core
 *
 *  The VM side never calls this directly; it posts REQ_AUDIO_*
 *  messages over the channel (the VM ecall handlers in
 *  vm_host_audio.c do that). The service is the channel's PROVIDER
 *  endpoint. On the desktop a worker thread runs
 *  audio_service_run(); on the H745 the M4 runs the same loop.
 *
 *  Single-context by construction: only the service thread/core
 *  touches the pool/arbiter/mixer, mutating them as it drains
 *  serialized requests. No locks here — the channel is the
 *  synchronization boundary.
 *
 *  Output: the service renders the mixed audio into a contiguous
 *  output ring. WHO DRAINS THAT RING to a real device is platform
 *  specific and NOT part of this file:
 *      desktop: an OS audio backend (WASAPI/ALSA/CoreAudio) — to be
 *               written and verified on real hardware; this sandbox
 *               has no sound device.
 *      H745:    SAI + DMA -> PCM5102A DAC.
 *  The service exposes audio_service_render() so that backend (or a
 *  test) can pull mixed frames.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include "vm/service_channel.h"
#include "audio/audio_pool.h"
#include "audio/audio_arbiter.h"
#include "audio/audio_pool_stream.h"
#include "audio/audio_mixer.h"
#include "audio/music_player.h"

#include <stdint.h>
#include <stdbool.h>

/* How many concurrent music players the service maintains (one per
 * music-capable track). SFX go straight to mixer channels; music
 * voices use a music_player instance. */
#ifndef AUDIO_SERVICE_MAX_MUSIC
#define AUDIO_SERVICE_MAX_MUSIC  3   /* the 3 SD/stream channels */
#endif

typedef struct AudioService AudioService;

/* Configuration: the caller supplies the audio-memory region (for
 * the pool), the channel (already initialized with a transport), and
 * basic mixer parameters. The service builds the pool, arbiter, and
 * mixer internally. */
typedef struct {
    ServiceChannel *channel;       /* the provider endpoint           */
    void           *pool_region;   /* audio memory for the block pool */
    size_t          pool_region_size;
    uint32_t        sample_rate;   /* e.g. 44100                      */
    uint32_t        track_count;   /* arbiter tracks (<= 16)          */
    /* Shared staging buffer for loading PCM into pool objects. The
     * requester (VM-side ecall handler) copies guest PCM here, then
     * posts REQ_AUDIO_LOAD_STAGED; the service copies staging->pool.
     * Must outlive the service and be reachable by both endpoints
     * (shared SRAM on the H745, shared heap on the desktop). If NULL,
     * staged loads are rejected. */
    void           *staging_buffer;
    size_t          staging_capacity;
} AudioServiceConfig;

/* Create / destroy. create returns NULL on bad config or alloc
 * failure. */
AudioService *audio_service_create(const AudioServiceConfig *cfg);
void          audio_service_destroy(AudioService *svc);

/* Process up to `max` pending channel requests (non-blocking). Each
 * REQ_AUDIO_* is handled — mutating pool/arbiter/mixer — and a
 * response is posted if the request expected one. Returns the number
 * of requests processed. The service loop calls this repeatedly,
 * interleaved with render. */
uint32_t audio_service_process(AudioService *svc, uint32_t max);

/* Render `frames` of mixed stereo output into `out` (interleaved
 * int16 L,R). This is what the platform output backend (or a test)
 * pulls. Safe to interleave with process() on the same thread (the
 * service is single-context). */
void audio_service_render(AudioService *svc, int16_t *out, uint32_t frames);

/* Run the blocking service loop until stopped: drain requests, then
 * (a real deployment would also fill the output ring on the audio
 * clock here). Provided for the desktop worker thread. `should_stop`
 * is polled; pass an atomic flag's address via the closure. */
typedef bool (*audio_service_stop_fn)(void *user);
void audio_service_run(AudioService *svc,
                       audio_service_stop_fn should_stop, void *user);

/* ---- introspection (for tests / meters) ---- */

/* Drop all objects + voices owned by a dying VM. */
void audio_service_sweep_vm(AudioService *svc, uint16_t vm_id);

AudioPool    *audio_service_pool(AudioService *svc);
AudioArbiter *audio_service_arbiter(AudioService *svc);
AudioMixer   *audio_service_mixer(AudioService *svc);

#endif /* AUDIO_SERVICE_H */
