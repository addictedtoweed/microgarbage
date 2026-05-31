/* ============================================================
 *  host_audio.c — AudioService bring-up + worker thread.
 *
 *  See host_audio.h for the contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "host_audio.h"

#include "audio/audio_service.h"
#include "audio/audio_sink.h"
#include "vm/vm_host_audio.h"
#include "vm/service_channel.h"
#include "vm/channel_thread.h"
#include "storage/trashfs.h"
#include "vm/vm_system.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <stdatomic.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <process.h>
   typedef HANDLE   audio_thread_t;
#else
#  include <pthread.h>
   typedef pthread_t audio_thread_t;
#endif

/* ----------------------------------------------------------------
 *  Sizing
 *
 *  Pool: 1 MB stand-in for the H745's PSRAM audio region. Staging:
 *  64 KB scratch the audio handlers borrow to marshal samples between
 *  the VM thread and the audio worker. Channel: 32 ChannelMsg slots
 *  in each direction (request/response) — single-digit messages in
 *  flight at peak, 32 leaves margin.
 * ---------------------------------------------------------------- */
#define AUDIO_POOL_REGION_BYTES (1024 * 1024)
#define AUDIO_STAGING_BYTES     (64 * 1024)
#define AUDIO_CHANNEL_SLOTS     32

static uint8_t          g_audio_pool_region[AUDIO_POOL_REGION_BYTES];
static uint8_t          g_audio_staging[AUDIO_STAGING_BYTES];
static ChannelMsg       g_audio_req_ring[AUDIO_CHANNEL_SLOTS];
static ChannelMsg       g_audio_resp_ring[AUDIO_CHANNEL_SLOTS];
static ServiceChannel   g_audio_channel;
static AudioService    *g_audio_service;
static audio_thread_t   g_audio_thread;
static _Atomic int      g_audio_stop;
static bool             g_audio_running;

static bool audio_should_stop(void *u) {
    (void)u;
    return atomic_load_explicit(&g_audio_stop, memory_order_acquire) != 0;
}

/* ----------------------------------------------------------------
 *  Worker thread
 *
 *  Desktop stand-in for the M4's SAI+DMA loop: continuously renders
 *  the mixed output to a live sound device and, between renders,
 *  drains channel requests + pumps music. The sink's write() is
 *  device-paced (it blocks until the device wants more), so this
 *  loop runs at the audio clock — no separate timer needed.
 *
 *  On Windows the sink is waveOut. If that can't be opened (no
 *  device, or a platform without it), we fall back to the request-
 *  only loop (audio still works logically, just silent) so the
 *  shell never hangs waiting on a device that isn't there.
 * ---------------------------------------------------------------- */
#define AUDIO_RENDER_QUANTUM 512u

static void audio_worker_body(void) {
    AudioSink sink;
    bool have_sink = false;
#if defined(_WIN32)
    have_sink = audio_sink_open(&sink, "waveout", NULL, 44100);
#endif

    if (!have_sink) {
        /* No live device: just service requests (silent). */
        audio_service_run(g_audio_service, audio_should_stop, NULL);
        return;
    }

    int16_t buf[AUDIO_RENDER_QUANTUM * 2];
    while (!audio_should_stop(NULL)) {
        audio_service_process(g_audio_service, 64);
        audio_service_render(g_audio_service, buf, AUDIO_RENDER_QUANTUM);
        if (audio_sink_write(&sink, buf, AUDIO_RENDER_QUANTUM) < 0) break;
    }
    audio_sink_close(&sink);
}

/* Platform thread entry — adapt the shared body to each OS's
 * thread-function signature. */
#if defined(_WIN32)
static DWORD WINAPI audio_worker(LPVOID u) {
    (void)u;
    audio_worker_body();
    return 0;
}
#else
static void *audio_worker(void *u) {
    (void)u;
    audio_worker_body();
    return NULL;
}
#endif

/* Platform thread start/join. Return 0 on success (start) like
 * pthread_create. */
static int audio_thread_start(void) {
#if defined(_WIN32)
    g_audio_thread = CreateThread(NULL, 0, audio_worker, NULL, 0, NULL);
    return g_audio_thread ? 0 : -1;
#else
    return pthread_create(&g_audio_thread, NULL, audio_worker, NULL);
#endif
}
static void audio_thread_join(void) {
#if defined(_WIN32)
    WaitForSingleObject(g_audio_thread, INFINITE);
    CloseHandle(g_audio_thread);
#else
    pthread_join(g_audio_thread, NULL);
#endif
}

/* ----------------------------------------------------------------
 *  Streaming WAV file reader (the service's AudioFileReader)
 *
 *  Composite reader bound into the service: a native path of the
 *  form "td0:/x" goes to the trashfs RAM disk (the SD card on the
 *  H745, via the same incremental open/read/seek), anything else
 *  goes to stdio (a /host file). The audio service stays
 *  filesystem-agnostic; it just calls open/read/seek/close.
 *
 *  Reads happen on the audio worker thread (the M4 in the
 *  deployment), which is why this file I/O lives here and not on
 *  the VM side. The TrashfsVolume pointer travels through the
 *  AudioFileReader.ctx field — set by host_audio_start.
 *
 *  NOTE: trashfs_read is read-only w.r.t. the volume's metadata
 *  but shares the volume with the VM thread; don't rewrite /td0
 *  while a /td0 WAV is streaming (long music streams from /host,
 *  not the small RAM disk, so this rarely bites in practice).
 * ---------------------------------------------------------------- */
typedef struct {
    bool         is_trash;
    FILE        *fp;       /* stdio backing (is_trash == false)  */
    TrashfsFile  tf;       /* trashfs backing (is_trash == true) */
} StreamFile;

static void *afr_open(void *ctx, const char *path) {
    TrashfsVolume *trash = (TrashfsVolume *)ctx;
    if (!path) return NULL;
    StreamFile *sf = calloc(1, sizeof *sf);
    if (!sf) return NULL;
    if (strncmp(path, "td0:/", 5) == 0) {
        if (!trash) { free(sf); return NULL; }
        sf->is_trash = true;
        /* "td0:/x" -> trashfs path "/x" (skip the "td0:" prefix). */
        if (trashfs_open(trash, path + 4, TRASHFS_O_RDONLY, &sf->tf)
                != TRASHFS_OK) { free(sf); return NULL; }
    } else {
        sf->fp = fopen(path, "rb");
        if (!sf->fp) { free(sf); return NULL; }
    }
    return sf;
}
static uint32_t afr_read(void *ctx, void *fh, void *dst, uint32_t bytes) {
    (void)ctx;
    StreamFile *sf = (StreamFile *)fh;
    if (!sf) return 0;
    if (sf->is_trash) {
        uint32_t got = 0;
        if (trashfs_read(&sf->tf, dst, bytes, &got) != TRASHFS_OK) return 0;
        return got;
    }
    return (uint32_t)fread(dst, 1, bytes, sf->fp);
}
static bool afr_seek(void *ctx, void *fh, uint32_t off) {
    (void)ctx;
    StreamFile *sf = (StreamFile *)fh;
    if (!sf) return false;
    if (sf->is_trash) {
        uint32_t newpos = 0;
        return trashfs_lseek(&sf->tf, (int32_t)off, TRASHFS_SEEK_SET,
                             &newpos) == TRASHFS_OK;
    }
    return fseek(sf->fp, (long)off, SEEK_SET) == 0;
}
static void afr_close(void *ctx, void *fh) {
    (void)ctx;
    StreamFile *sf = (StreamFile *)fh;
    if (!sf) return;
    if (sf->is_trash) trashfs_close(&sf->tf);
    else if (sf->fp) fclose(sf->fp);
    free(sf);
}

/* ----------------------------------------------------------------
 *  Lifecycle
 * ---------------------------------------------------------------- */

bool host_audio_start(VmSystem *sys,
                      const char *host_fs_root,
                      TrashfsVolume *trash_vol) {
    ChannelTransport tr;
    if (!channel_thread_transport_make(&tr)) return false;
    if (!service_channel_init(&g_audio_channel, g_audio_req_ring,
                              g_audio_resp_ring, AUDIO_CHANNEL_SLOTS, &tr)) {
        tr.destroy(tr.ctx);
        return false;
    }
    AudioServiceConfig acfg = {
        .channel          = &g_audio_channel,
        .pool_region      = g_audio_pool_region,
        .pool_region_size = sizeof(g_audio_pool_region),
        .sample_rate      = 44100,
        .track_count      = 16,
        .staging_buffer   = g_audio_staging,
        .staging_capacity = sizeof(g_audio_staging),
        .file_reader      = { afr_open, afr_read, afr_seek, afr_close,
                              trash_vol },
    };
    g_audio_service = audio_service_create(&acfg);
    if (!g_audio_service) {
        service_channel_destroy(&g_audio_channel);
        return false;
    }
    atomic_store(&g_audio_stop, 0);
    if (audio_thread_start() != 0) {
        audio_service_destroy(g_audio_service);
        g_audio_service = NULL;
        service_channel_destroy(&g_audio_channel);
        return false;
    }
    g_audio_running = true;

    VmHostAudioConfig hcfg = {
        .channel          = &g_audio_channel,
        .staging_buffer   = g_audio_staging,
        .staging_capacity = sizeof(g_audio_staging),
        .call_timeout_ms  = 1000,
        .host_fs_root     = host_fs_root,
    };
    if (!vm_host_install_audio(sys, &hcfg)) {
        /* handlers not installed; stop the worker we started */
        atomic_store_explicit(&g_audio_stop, 1, memory_order_release);
        audio_thread_join();
        audio_service_destroy(g_audio_service);
        g_audio_service = NULL;
        service_channel_destroy(&g_audio_channel);
        g_audio_running = false;
        return false;
    }
    return true;
}

void host_audio_stop(void) {
    if (!g_audio_running) return;
    atomic_store_explicit(&g_audio_stop, 1, memory_order_release);
    audio_thread_join();
    audio_service_destroy(g_audio_service);
    g_audio_service = NULL;
    service_channel_destroy(&g_audio_channel);
    g_audio_running = false;
}
