/* ============================================================
 *  host_platform.h — the host's platform abstraction layer (HAL)
 *
 *  This is the seam between the host *services* (filesystem, stdio,
 *  TUI, the run loop) and the *operating system* underneath them.
 *  Everything in here is "exactly one implementation per build,
 *  chosen at compile/link time" — as opposed to transports, which
 *  are runtime-selected via the VmHostTransport vtable.
 *
 *  Why this exists
 *  ---------------
 *  Before this layer, host code reached directly for clock_gettime,
 *  nanosleep / Sleep, sigaction / signal / SetConsoleCtrlHandler,
 *  etc., guarded by #ifdef _WIN32 scattered across files. That made
 *  the Windows/Linux/Cygwin differences hard to follow and hard to
 *  extend. Routing those calls through this small interface keeps
 *  the OS-specific code in one place per platform:
 *
 *      src/host/platform_posix.c   Linux + Cygwin (POSIX)
 *      src/host/platform_win.c     native Windows (mingw/MSVC)
 *      src/host/platform_stub.c    reference for a NEW target
 *                                  (e.g. STM32) — weak no-op bodies
 *                                  a porter overrides one at a time
 *
 *  Scope (intentionally small)
 *  ---------------------------
 *  Only the primitives that genuinely differ across targets and
 *  that the host needs every run: time, sleep, and the stop/
 *  interrupt hook. NOT here, on purpose:
 *    - Filesystem: already abstracted (trashfs + host passthrough).
 *    - Sockets:    contained in transport_tcp.c. MCU networking is
 *                  lwIP, a different shape; we'll design that seam
 *                  when it's real, not against desktop sockets now.
 *    - Console raw-mode / nonblocking console read: handled by the
 *                  stdio bridge (termios vs vm_host_stdio_win32);
 *                  see vm_host_stdio.h.
 *    - Spawn / scheduling / memory: pure VM-core logic, no OS
 *                  dependency, so nothing to abstract.
 *
 *  Adding a platform
 *  -----------------
 *  Copy platform_stub.c, implement the functions for your target,
 *  and build with it instead of platform_posix/win. See
 *  docs/host_platform.md.
 * ============================================================ */
#ifndef VM_HOST_PLATFORM_H
#define VM_HOST_PLATFORM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------
 *  Time
 * ------------------------------------------------------------ */

/* Monotonic millisecond counter. Starts from an arbitrary anchor
 * on first call (so the first call may return ~0) and never goes
 * backwards. Wraps at 2^32 ms (~49.7 days), which is the documented
 * tick-source contract the scheduler expects. Immune to wall-clock
 * adjustments (NTP, manual set).
 *
 * Signature matches VmSystemConfig.tick_source so it can be passed
 * straight through; the userdata argument is unused but kept for
 * that compatibility. */
uint32_t host_platform_monotonic_ms(void *userdata);

/* Wall-clock (real) time, for SYS_REALTIME_NOW. Writes Unix epoch
 * seconds and the nanosecond remainder. Returns false if the target
 * has no real-time clock (a bare MCU without an RTC), in which case
 * the syscall reports -ENOSYS to the guest.
 *
 * Signature matches VmSystemConfig.realtime_source. */
bool host_platform_realtime(void *userdata,
                            uint32_t *seconds_out,
                            uint32_t *nanos_out);

/* ------------------------------------------------------------
 *  Sleep
 * ------------------------------------------------------------ */

/* Sleep the calling (host) thread for approximately ms milliseconds.
 * Used by the run loop to yield the CPU when no VM is runnable, so
 * the host doesn't spin a core at 100%. Coarse precision is fine. */
void host_platform_sleep_ms(unsigned ms);

/* ------------------------------------------------------------
 *  Stop / interrupt hook
 *
 *  The host run loop polls host_platform_stop_requested() once per
 *  iteration and shuts down cleanly when it returns true. The flag
 *  is raised by an OS interrupt source wired up by
 *  host_platform_install_stop_handler():
 *    - POSIX:   SIGINT via sigaction.
 *    - Windows: SIGINT via signal() AND a console control handler
 *               (SetConsoleCtrlHandler) — the latter is what
 *               actually catches Ctrl-C in a real console; the
 *               former covers kill -INT and CRT emulation.
 *    - MCU:     however the target signals "stop" (or never; the
 *               stub leaves the flag clear and the host runs until
 *               power-off).
 * ------------------------------------------------------------ */

/* Install the platform's interrupt handler(s). Call once, early in
 * main(), before the run loop. Idempotent. */
void host_platform_install_stop_handler(void);

/* True once an interrupt (Ctrl-C / SIGINT / console close) has been
 * requested. The run loop polls this. */
bool host_platform_stop_requested(void);

/* Force the stop flag (e.g. a guest asked the host to exit, or a
 * fatal error wants a clean teardown). Lets non-signal code share
 * the same shutdown path. */
void host_platform_request_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* VM_HOST_PLATFORM_H */
