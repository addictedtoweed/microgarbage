/* ============================================================
 *  worker.c — mgapi's main worker thread (Win32 + pthreads).
 *
 *  See worker.h for the contract. Implementation notes:
 *
 *  - Auto-reset event + atomic pending-vblank counter. The signal
 *    path bumps the counter then SetEvent's; the worker loop reads
 *    the counter and processes one tick per pending vblank. This
 *    keeps cadence under transient load: if the worker misses a
 *    vblank (one tick took >16.7 ms), the next wakeup catches up
 *    by running multiple ticks back-to-back instead of dropping.
 *
 *  - Stop path: atomic_store(g_stop, 1) + SetEvent, then join.
 *    The worker checks g_stop after every wait and after every
 *    tick so it exits within at most one tick of the stop call.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "worker.h"

#include <stdatomic.h>
#include <stdio.h>

#if defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#else
  #include <errno.h>
  #include <pthread.h>
  #include <semaphore.h>
#endif

static MgapiWorkerTick g_tick;

static atomic_int       g_stop;
static atomic_uint      g_pending_vblanks;
static atomic_uint_least64_t g_last_elapsed_ns;

#if defined(_WIN32)
static HANDLE  g_thread;
static HANDLE  g_evt;     /* auto-reset; SetEvent on each signal */
#else
static pthread_t g_thread;
static int       g_thread_started;
static sem_t     g_sem;   /* counting semaphore — same role as evt+counter */
#endif

static int g_started;

static void worker_drain(void) {
    /* Drain every pending vblank into the tick callback. Worker
     * runs as fast as the callback can finish; if multiple vblanks
     * piled up while we were busy we catch them all here. */
    for (;;) {
        unsigned pending = atomic_load_explicit(&g_pending_vblanks,
                                                memory_order_acquire);
        if (pending == 0) return;
        atomic_fetch_sub_explicit(&g_pending_vblanks, 1u,
                                  memory_order_acq_rel);
        uint64_t dt = atomic_load_explicit(&g_last_elapsed_ns,
                                           memory_order_acquire);
        if (g_tick) g_tick(dt);
        if (atomic_load_explicit(&g_stop, memory_order_acquire)) return;
    }
}

#if defined(_WIN32)
static DWORD WINAPI worker_entry(LPVOID u) {
    (void)u;
    fprintf(stderr, "mgapi worker: thread entered\n");
    fflush(stderr);
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        DWORD r = WaitForSingleObject(g_evt, INFINITE);
        if (r != WAIT_OBJECT_0) break;
        if (atomic_load_explicit(&g_stop, memory_order_acquire)) break;
        worker_drain();
    }
    fprintf(stderr, "mgapi worker: thread exiting\n");
    fflush(stderr);
    return 0;
}
#else
static void *worker_entry(void *u) {
    (void)u;
    fprintf(stderr, "mgapi worker: thread entered\n");
    fflush(stderr);
    while (!atomic_load_explicit(&g_stop, memory_order_acquire)) {
        while (sem_wait(&g_sem) != 0) {
            if (errno != EINTR) break;
        }
        if (atomic_load_explicit(&g_stop, memory_order_acquire)) break;
        worker_drain();
    }
    fprintf(stderr, "mgapi worker: thread exiting\n");
    fflush(stderr);
    return NULL;
}
#endif

int mgapi_worker_start(MgapiWorkerTick tick) {
    if (g_started) return 0;
    g_tick = tick;
    atomic_store(&g_stop, 0);
    atomic_store(&g_pending_vblanks, 0u);
    atomic_store(&g_last_elapsed_ns, 0ull);

#if defined(_WIN32)
    g_evt = CreateEvent(NULL, /*manualReset=*/FALSE, /*initial=*/FALSE, NULL);
    if (!g_evt) {
        fprintf(stderr, "mgapi worker: CreateEvent FAILED (err=%lu)\n",
                (unsigned long)GetLastError());
        fflush(stderr);
        return -1;
    }
    g_thread = CreateThread(NULL, 0, worker_entry, NULL, 0, NULL);
    if (!g_thread) {
        fprintf(stderr, "mgapi worker: CreateThread FAILED (err=%lu)\n",
                (unsigned long)GetLastError());
        fflush(stderr);
        CloseHandle(g_evt);
        g_evt = NULL;
        return -1;
    }
#else
    if (sem_init(&g_sem, 0, 0) != 0) {
        fprintf(stderr, "mgapi worker: sem_init FAILED\n");
        fflush(stderr);
        return -1;
    }
    if (pthread_create(&g_thread, NULL, worker_entry, NULL) != 0) {
        fprintf(stderr, "mgapi worker: pthread_create FAILED\n");
        fflush(stderr);
        sem_destroy(&g_sem);
        return -1;
    }
    g_thread_started = 1;
#endif

    g_started = 1;
    fprintf(stderr, "mgapi worker: started\n");
    fflush(stderr);
    return 0;
}

void mgapi_worker_stop(void) {
    if (!g_started) return;
    atomic_store_explicit(&g_stop, 1, memory_order_release);

#if defined(_WIN32)
    if (g_evt) SetEvent(g_evt);
    if (g_thread) {
        WaitForSingleObject(g_thread, INFINITE);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_evt) {
        CloseHandle(g_evt);
        g_evt = NULL;
    }
#else
    sem_post(&g_sem);
    if (g_thread_started) {
        pthread_join(g_thread, NULL);
        g_thread_started = 0;
    }
    sem_destroy(&g_sem);
#endif

    g_started = 0;
    g_tick = NULL;
    fprintf(stderr, "mgapi worker: stopped\n");
    fflush(stderr);
}

void mgapi_worker_signal(uint64_t elapsed_ns) {
    if (!g_started) return;
    atomic_store_explicit(&g_last_elapsed_ns,
                          (uint_least64_t)elapsed_ns,
                          memory_order_release);
    atomic_fetch_add_explicit(&g_pending_vblanks, 1u,
                              memory_order_acq_rel);
#if defined(_WIN32)
    SetEvent(g_evt);
#else
    sem_post(&g_sem);
#endif
}

/* v2.35: wake the worker to run a tick WITHOUT advancing the embedder's
 * vblank clock. Used by the frame-consumed hook so a guest blocked in
 * mg_wait_frame resumes the instant the SNES acks the frame, instead of
 * waiting up to one mgapi_step period (~23 ms at the emulator's ~43 Hz
 * step cadence) for the next periodic signal — that latency was capping
 * FMV at ~11 fps. Bumps pending so worker_drain runs the tick, but
 * leaves g_last_elapsed_ns alone (the tick reuses the last real dt; the
 * audio pump is a no-op so dt is immaterial here). Safe from any
 * thread. */
void mgapi_worker_wake(void) {
    if (!g_started) return;
    atomic_fetch_add_explicit(&g_pending_vblanks, 1u,
                              memory_order_acq_rel);
#if defined(_WIN32)
    SetEvent(g_evt);
#else
    sem_post(&g_sem);
#endif
}
