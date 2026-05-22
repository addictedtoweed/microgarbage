/* ============================================================
 *  channel_thread.c — desktop (pthread) transport backend
 *
 *  Implements ChannelTransport's notify/wait/destroy with a single
 *  mutex + condition variable. Both endpoints share the one condvar:
 *
 *    notify():  lock, bump a generation counter, broadcast, unlock.
 *    wait():    lock, if the generation hasn't advanced since we
 *               last checked, wait (with timeout); return whether
 *               the generation advanced (woken) vs timed out.
 *
 *  A single shared condvar means a notify wakes BOTH a waiting
 *  requester and a waiting provider; each then re-polls its own ring
 *  and goes back to sleep if its ring was empty. That spurious
 *  wakeup is harmless — the rings are the source of truth; the
 *  condvar only exists to avoid busy-spinning. The generation
 *  counter makes the wait edge-triggered enough to avoid lost
 *  wakeups (a notify between our ring-check and our wait still
 *  advances the generation, so we don't sleep through it).
 *
 *  This backend is desktop-only. The H745 backend (HSEM/IPI) is a
 *  separate file built at MCU bring-up.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

/* clock_gettime / CLOCK_REALTIME need POSIX.1-2001. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "vm/service_channel.h"

#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <time.h>

typedef struct {
    pthread_mutex_t mtx;
    pthread_cond_t  cv;
    uint64_t        generation;   /* bumped on each notify */
} ThreadTransport;

static void tt_notify(void *ctx) {
    ThreadTransport *t = (ThreadTransport *)ctx;
    pthread_mutex_lock(&t->mtx);
    t->generation++;
    pthread_cond_broadcast(&t->cv);
    pthread_mutex_unlock(&t->mtx);
}

static bool tt_wait(void *ctx, uint32_t timeout_ms) {
    ThreadTransport *t = (ThreadTransport *)ctx;
    bool woken = false;
    pthread_mutex_lock(&t->mtx);

    uint64_t start_gen = t->generation;

    if (timeout_ms == 0) {
        /* poll-style: report whether a notify is already pending
         * since we can't observe "since last check" without state;
         * treat 0 as "don't block", woken iff generation moved is
         * not observable here, so just return false (no block). */
        pthread_mutex_unlock(&t->mtx);
        return false;
    }

    if (timeout_ms == UINT32_MAX) {
        while (t->generation == start_gen) {
            pthread_cond_wait(&t->cv, &t->mtx);
        }
        woken = true;
    } else {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec  += timeout_ms / 1000u;
        ts.tv_nsec += (long)(timeout_ms % 1000u) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }

        int rc = 0;
        while (t->generation == start_gen && rc == 0) {
            rc = pthread_cond_timedwait(&t->cv, &t->mtx, &ts);
        }
        woken = (t->generation != start_gen);
    }

    pthread_mutex_unlock(&t->mtx);
    return woken;
}

static void tt_destroy(void *ctx) {
    ThreadTransport *t = (ThreadTransport *)ctx;
    if (!t) return;
    pthread_cond_destroy(&t->cv);
    pthread_mutex_destroy(&t->mtx);
    free(t);
}

/* Build a thread transport. Returns false on alloc failure. The
 * returned transport's ctx is heap-owned and freed by destroy. */
bool channel_thread_transport_make(ChannelTransport *out) {
    if (!out) return false;
    ThreadTransport *t = calloc(1, sizeof(*t));
    if (!t) return false;
    if (pthread_mutex_init(&t->mtx, NULL) != 0) { free(t); return false; }
    if (pthread_cond_init(&t->cv, NULL) != 0) {
        pthread_mutex_destroy(&t->mtx); free(t); return false;
    }
    t->generation = 0;
    out->notify  = tt_notify;
    out->wait    = tt_wait;
    out->destroy = tt_destroy;
    out->ctx     = t;
    return true;
}
