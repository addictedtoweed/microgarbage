/* ============================================================
 *  platform_win.c — host_platform.h for native Windows (mingw/MSVC)
 *
 *  Native Windows implementation of the host platform layer.
 *
 *  Relocation of code that previously lived inline in
 *  examples/05_shell/host.c. Behavior is intended to be identical.
 *
 *  Notes:
 *    - Time uses native Win32 APIs (GetTickCount64 /
 *      GetSystemTimeAsFileTime) rather than mingw's clock_gettime
 *      shim, so the native build doesn't pull in winpthreads. This
 *      is a behavior-preserving choice: same millisecond monotonic
 *      tick and same Unix-epoch wall clock the host had before, just
 *      sourced from the platform-appropriate API.
 *    - The stop hook installs BOTH signal(SIGINT) and a console
 *      control handler. The console handler is what actually catches
 *      Ctrl-C in a real console (cmd/PowerShell); signal() covers
 *      kill -INT and CRT emulation. This is why Ctrl-C works in a
 *      real console — see the long bug hunt that established it.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#if defined(_WIN32)

#include "vm/host_platform.h"

#include <windows.h>
#include <signal.h>

/* ------------------------------------------------------------
 *  Time
 *
 *  Uses native Win32 APIs rather than mingw's clock_gettime shim:
 *  GetTickCount64 (monotonic, ms resolution, no winpthreads
 *  dependency) and GetSystemTimeAsFileTime (wall clock). This keeps
 *  the native build self-contained — clock_gettime on mingw pulls in
 *  the winpthreads runtime, which we'd otherwise have to link.
 * ------------------------------------------------------------ */
static uint64_t g_t0_ms = 0;
static int      g_t0_set = 0;

uint32_t host_platform_monotonic_ms(void *userdata) {
    (void)userdata;
    uint64_t now = (uint64_t)GetTickCount64();   /* ms since boot */
    if (!g_t0_set) {
        g_t0_ms = now;
        g_t0_set = 1;
    }
    /* Anchor at first call so the tick starts near 0; truncate to
     * 32 bits for the documented 49.7-day wraparound. */
    return (uint32_t)(now - g_t0_ms);
}

bool host_platform_realtime(void *userdata,
                            uint32_t *seconds_out,
                            uint32_t *nanos_out) {
    (void)userdata;
    /* FILETIME is 100-ns ticks since 1601-01-01. Convert to Unix
     * epoch (seconds since 1970) by subtracting the well-known
     * offset, then split into seconds + nanoseconds. */
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t ticks = ((uint64_t)ft.dwHighDateTime << 32)
                   | (uint64_t)ft.dwLowDateTime;          /* 100-ns */
    /* 11644473600 = seconds between 1601 and 1970. */
    const uint64_t EPOCH_DIFF_100NS = 116444736000000000ULL;
    if (ticks < EPOCH_DIFF_100NS) return false;
    uint64_t unix100ns = ticks - EPOCH_DIFF_100NS;
    *seconds_out = (uint32_t)(unix100ns / 10000000ULL);
    *nanos_out   = (uint32_t)((unix100ns % 10000000ULL) * 100ULL);
    return true;
}

/* ------------------------------------------------------------
 *  Sleep
 * ------------------------------------------------------------ */
void host_platform_sleep_ms(unsigned ms) {
    Sleep(ms);
}

/* ------------------------------------------------------------
 *  Stop / interrupt hook
 * ------------------------------------------------------------ */
static volatile sig_atomic_t g_stop = 0;

static void on_sigint(int signo) { (void)signo; g_stop = 1; }

static BOOL WINAPI on_console_ctrl(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            g_stop = 1;
            /* Brief notice so a clean Ctrl-C shutdown is visibly
             * distinct from a hard kill. WriteFile (not fprintf):
             * this handler runs on a separate OS thread, so we avoid
             * the C stdio lock the main thread may hold. */
            {
                static const char msg[] = "\nhost: stopping...\n";
                DWORD wrote = 0;
                WriteFile(GetStdHandle(STD_ERROR_HANDLE),
                          msg, (DWORD)(sizeof(msg) - 1), &wrote, NULL);
            }
            return TRUE;   /* handled */
        default:
            return FALSE;
    }
}

void host_platform_install_stop_handler(void) {
    /* mingw lacks struct sigaction; the ANSI signal() API is enough
     * for SIGINT here (covers kill -INT and CRT emulation). */
    signal(SIGINT, on_sigint);
    /* The mechanism that actually catches a real-console Ctrl-C. */
    SetConsoleCtrlHandler(on_console_ctrl, TRUE);
}

bool host_platform_stop_requested(void) {
    return g_stop != 0;
}

void host_platform_request_stop(void) {
    g_stop = 1;
}

#endif /* _WIN32 */
