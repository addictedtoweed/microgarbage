/* ============================================================
 *  platform_posix.c — host_platform.h for Linux and Cygwin
 *
 *  POSIX implementation of the host platform layer: clock_gettime
 *  for time, nanosleep for sleep, sigaction for the stop hook.
 *
 *  This file is the relocation of code that previously lived inline
 *  in examples/05_shell/host.c. Behavior is intended to be
 *  identical; only the location changed.
 *
 *  Built on: any non-Windows target, AND Cygwin (which is POSIX —
 *  __CYGWIN__ is defined but _WIN32 is not... except Cygwin DOES
 *  define _WIN32 in some header configurations, so the build system
 *  selects this file explicitly rather than relying on #ifdef here).
 *  To keep it self-contained and avoid surprises, the whole file is
 *  guarded so that an accidental compile on native Windows is a
 *  no-op rather than a double-definition.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#if !defined(_WIN32) || defined(__CYGWIN__)

/* clock_gettime / CLOCK_MONOTONIC need POSIX feature macros on some
 * libcs. Define before any include. */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include "vm/host_platform.h"

#include <time.h>
#include <signal.h>
#include <string.h>

/* ------------------------------------------------------------
 *  Time
 * ------------------------------------------------------------ */
static struct timespec g_t0;
static int             g_t0_set = 0;

uint32_t host_platform_monotonic_ms(void *userdata) {
    (void)userdata;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!g_t0_set) {
        g_t0 = t;
        g_t0_set = 1;
    }
    /* Difference in milliseconds. Borrow from seconds if nsec went
     * backwards relative to the anchor — without that, a signed
     * subtraction cast to uint64 blows up to a near-2^64 value and
     * the millisecond math skews once per second of real time. */
    long sec_delta  = (long)(t.tv_sec  - g_t0.tv_sec);
    long nsec_delta = (long)(t.tv_nsec - g_t0.tv_nsec);
    if (nsec_delta < 0) {
        sec_delta  -= 1;
        nsec_delta += 1000000000L;
    }
    uint64_t ms = (uint64_t)sec_delta * 1000ULL
                + (uint64_t)nsec_delta / 1000000ULL;
    /* Truncate to 49.7-day wraparound — documented intended
     * behavior. */
    return (uint32_t)ms;
}

bool host_platform_realtime(void *userdata,
                            uint32_t *seconds_out,
                            uint32_t *nanos_out) {
    (void)userdata;
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0) return false;
    /* Truncate to 32-bit Unix epoch seconds (wraps in 2106). */
    *seconds_out = (uint32_t)t.tv_sec;
    *nanos_out   = (uint32_t)t.tv_nsec;
    return true;
}

/* ------------------------------------------------------------
 *  Sleep
 * ------------------------------------------------------------ */
void host_platform_sleep_ms(unsigned ms) {
    struct timespec ts;
    ts.tv_sec  = (time_t)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000ul);
    nanosleep(&ts, NULL);
}

/* ------------------------------------------------------------
 *  Stop / interrupt hook
 * ------------------------------------------------------------ */
static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo) { (void)signo; g_stop = 1; }

void host_platform_install_stop_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
}

bool host_platform_stop_requested(void) {
    return g_stop != 0;
}

void host_platform_request_stop(void) {
    g_stop = 1;
}

#endif /* !_WIN32 || __CYGWIN__ */
