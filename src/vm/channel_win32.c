/* ============================================================
 *  channel_win32.c — native Windows transport backend
 *
 *  The native-Windows twin of channel_thread.c. Implements the SAME
 *  ChannelTransport notify/wait/destroy contract and the SAME
 *  generation-counter logic, but with Win32 primitives instead of
 *  pthreads, so audio works in a standalone mingw .exe with no
 *  pthread/Cygwin dependency:
 *
 *    pthread_mutex_t      -> CRITICAL_SECTION
 *    pthread_cond_t       -> CONDITION_VARIABLE   (Vista+)
 *    pthread_cond_*wait   -> SleepConditionVariableCS
 *    pthread_cond_broadcast-> WakeAllConditionVariable
 *
 *  Exposes channel_thread_transport_make() — the SAME name as the
 *  pthread backend — so callers don't branch. The build links EITHER
 *  channel_thread.c (POSIX/Cygwin) OR this file (native Windows),
 *  never both.
 *
 *  IMPORTANT: this uses Win32 threads directly (no pthread shim), so
 *  it does NOT pull in libwinpthread — keeping a -static single-file
 *  .exe clean for distribution.
 *
 *  HONESTY: like the waveOut backend, this ships compile-verified
 *  (under the mingw cross) but NOT execution-verified here — the
 *  sandbox can't run a Windows binary. The LOGIC being ported is the
 *  generation-counter wait/signal already proven (TSan-clean) in
 *  channel_thread.c; only the OS primitives differ. Runtime behaviour
 *  (no deadlock, correct wakeups, clean shutdown) is verified on
 *  Windows by you.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/service_channel.h"

#if defined(_WIN32)

#include <windows.h>
#include <stdlib.h>

typedef struct {
    CRITICAL_SECTION   cs;
    CONDITION_VARIABLE cv;
    volatile ULONGLONG generation;   /* bumped on each notify */
} Win32Transport;

static void wt_notify(void *ctx) {
    Win32Transport *t = (Win32Transport *)ctx;
    EnterCriticalSection(&t->cs);
    t->generation++;
    WakeAllConditionVariable(&t->cv);   /* wake both endpoints */
    LeaveCriticalSection(&t->cs);
}

static bool wt_wait(void *ctx, uint32_t timeout_ms) {
    Win32Transport *t = (Win32Transport *)ctx;
    bool woken = false;
    EnterCriticalSection(&t->cs);

    ULONGLONG start_gen = t->generation;

    if (timeout_ms == 0) {
        /* poll-style: don't block (mirror the pthread backend, which
         * returns false here — the rings are the source of truth). */
        LeaveCriticalSection(&t->cs);
        return false;
    }

    /* SleepConditionVariableCS releases cs while waiting and
     * re-acquires it on wake. It can wake spuriously, so loop on the
     * generation just like the pthread backend. INFINITE maps from
     * UINT32_MAX. */
    DWORD remaining = (timeout_ms == UINT32_MAX) ? INFINITE : (DWORD)timeout_ms;

    /* For a finite timeout we track elapsed time so spurious wakeups
     * don't reset the full timeout each iteration. */
    ULONGLONG deadline = 0;
    if (remaining != INFINITE)
        deadline = GetTickCount64() + (ULONGLONG)timeout_ms;

    while (t->generation == start_gen) {
        DWORD this_wait;
        if (remaining == INFINITE) {
            this_wait = INFINITE;
        } else {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;            /* timed out */
            this_wait = (DWORD)(deadline - now);
        }
        BOOL ok = SleepConditionVariableCS(&t->cv, &t->cs, this_wait);
        if (!ok) {
            /* WAIT_TIMEOUT or error: stop waiting (re-check below). */
            break;
        }
        /* woke (possibly spurious) — loop re-tests the generation */
    }
    woken = (t->generation != start_gen);

    LeaveCriticalSection(&t->cs);
    return woken;
}

static void wt_destroy(void *ctx) {
    Win32Transport *t = (Win32Transport *)ctx;
    if (!t) return;
    DeleteCriticalSection(&t->cs);
    /* CONDITION_VARIABLE needs no explicit destroy on Win32. */
    free(t);
}

bool channel_thread_transport_make(ChannelTransport *out) {
    if (!out) return false;
    Win32Transport *t = calloc(1, sizeof(*t));
    if (!t) return false;
    InitializeCriticalSection(&t->cs);
    InitializeConditionVariable(&t->cv);
    t->generation = 0;
    out->notify  = wt_notify;
    out->wait    = wt_wait;
    out->destroy = wt_destroy;
    out->ctx     = t;
    return true;
}

#endif /* _WIN32 */
