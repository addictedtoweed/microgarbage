/* ============================================================
 *  vm/host_compat.h — POSIX shims for native-Windows builds
 *
 *  Cygwin and Linux see this header and #define nothing. On
 *  mingw (where _WIN32 is defined but POSIX headers like
 *  termios.h / time.h's clock_gettime / unistd.h's nanosleep
 *  aren't fully available), this header supplies inline shims
 *  built on top of the Win32 API.
 *
 *  Why a separate header: keeping platform conditionals in
 *  one place beats sprinkling #ifdef _WIN32 throughout each
 *  caller. Anywhere the host needs clock_gettime / nanosleep /
 *  similar, it just includes this and uses the POSIX name.
 *
 *  This header is for HOST code (the application built for
 *  the developer's machine). Guests are RV32IMC and use their
 *  own libc bridge in lib/vm_runtime.c — completely separate
 *  layer.
 *
 *  Public domain (CC0).
 * ============================================================ */

#ifndef MICROGARBAGE_VM_HOST_COMPAT_H
#define MICROGARBAGE_VM_HOST_COMPAT_H

#ifndef _WIN32
/* On Linux/macOS/Cygwin, ensure POSIX prototypes for
 * nanosleep, clock_gettime, etc. are visible. Without this
 * the default <time.h> may only expose ANSI C symbols. Must
 * be defined before any system header is pulled in by the
 * including translation unit. */
#  ifndef _POSIX_C_SOURCE
#    define _POSIX_C_SOURCE 200809L
#  endif
#  include <time.h>
#endif

#ifdef _WIN32

#include <time.h>      /* struct timespec lives here on mingw too */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* CLOCK_REALTIME / CLOCK_MONOTONIC — POSIX defines these as
 * clockid_t enums. mingw doesn't ship a clock_gettime, so we
 * supply our own values. */
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME  0
#endif
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

/* Note: mingw's <time.h> may declare clock_gettime as a normal
 * function prototype (without providing the body — the symbol is
 * undefined at link time). Defining our own static inline would
 * clash with that declaration.
 *
 * To avoid the clash we macro-replace the calls with private
 * names. Callers writing clock_gettime(...) get our impl; the
 * mingw declaration is left alone. */
#define clock_gettime vmh_clock_gettime
#define nanosleep     vmh_nanosleep

/* clock_gettime on Windows:
 *   CLOCK_REALTIME  → GetSystemTimePreciseAsFileTime, converted
 *                     to seconds + nanoseconds since Unix epoch
 *   CLOCK_MONOTONIC → QueryPerformanceCounter, scaled to ns
 *
 * The FILETIME epoch is 1601-01-01, Unix is 1970-01-01. The
 * delta is 11644473600 seconds, or 116444736000000000 in
 * 100-nanosecond ticks. */
static inline int vmh_clock_gettime(int clk_id, struct timespec *tp) {
    if (!tp) return -1;

    if (clk_id == CLOCK_REALTIME) {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        unsigned long long t = ((unsigned long long)ft.dwHighDateTime << 32) |
                                (unsigned long long)ft.dwLowDateTime;
        t -= 116444736000000000ULL;   /* to Unix epoch, in 100-ns */
        tp->tv_sec  = (time_t)(t / 10000000ULL);
        tp->tv_nsec = (long)((t % 10000000ULL) * 100);
        return 0;
    }
    if (clk_id == CLOCK_MONOTONIC) {
        LARGE_INTEGER freq, count;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&count);
        tp->tv_sec  = (time_t)(count.QuadPart / freq.QuadPart);
        long long remainder = count.QuadPart % freq.QuadPart;
        tp->tv_nsec = (long)((remainder * 1000000000LL) / freq.QuadPart);
        return 0;
    }
    return -1;
}

/* nanosleep on Windows: Sleep takes milliseconds. We round
 * nanoseconds up to the nearest millisecond to satisfy the
 * "at least this long" contract. For sub-millisecond sleeps
 * this is wildly imprecise; the host doesn't currently need
 * that granularity. */
static inline int vmh_nanosleep(const struct timespec *req, struct timespec *rem) {
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    if (!req) return -1;
    unsigned long long ms = (unsigned long long)req->tv_sec * 1000ULL +
                            ((unsigned long long)req->tv_nsec + 999999ULL) / 1000000ULL;
    if (ms == 0 && (req->tv_sec > 0 || req->tv_nsec > 0)) ms = 1;
    Sleep((DWORD)ms);
    return 0;
}

#endif /* _WIN32 */

#endif /* MICROGARBAGE_VM_HOST_COMPAT_H */
