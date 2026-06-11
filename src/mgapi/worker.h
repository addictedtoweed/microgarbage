/* ============================================================
 *  worker.h — mgapi's main worker thread.
 *
 *  The bsnes-plus thread must never block on the guest VM, audio
 *  mixing, file I/O, or TCP polling. This module owns a dedicated
 *  worker thread that runs the per-frame step body, woken by the
 *  embedder's vblank signal.
 *
 *  Sync model: a Win32 auto-reset event + an atomic pending-vblank
 *  counter. The embedder calls mgapi_worker_signal once per
 *  emulated vblank; the worker thread wakes, drains the counter,
 *  and runs the registered tick callback once per pending vblank.
 *  If the worker falls behind, it catches up on its own — bsnes
 *  doesn't stall.
 *
 *  Stage 1 (v1.68): just stands the thread up and forwards what
 *  used to run inside mgapi_step. No audio/cart-window behaviour
 *  changes yet; that lands in v1.69+.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_WORKER_H
#define MGAPI_WORKER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Callback shape: invoked once per drained vblank on the worker
 * thread. `elapsed_ns` is whatever the embedder passed to the
 * most recent mgapi_worker_signal, or 0 if the embedder never
 * tracked elapsed time. */
typedef void (*MgapiWorkerTick)(uint64_t elapsed_ns);

/* Start the worker thread. Must be called AFTER mgapi_init has
 * brought up the audio service, VM, and TCP listener — the tick
 * callback assumes those are live. Returns 0 / -errno. */
int  mgapi_worker_start(MgapiWorkerTick tick);

/* Stop the worker. Idempotent. Must be called BEFORE mgapi_shutdown
 * tears down the VM/audio so the tick callback never sees a half-
 * torn-down runtime. */
void mgapi_worker_stop(void);

/* Embedder-facing kick: increments the pending-vblank counter and
 * signals the worker. Returns immediately (sub-microsecond). Safe
 * to call before mgapi_worker_start (becomes a no-op then). */
void mgapi_worker_signal(uint64_t elapsed_ns);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_WORKER_H */
