/* ============================================================
 *  platform_stub.c — REFERENCE platform layer for a NEW target
 *
 *  Copy this file when porting microgarbage's host to a new
 *  platform (e.g. an STM32 bare-metal or RTOS target). It provides
 *  a complete, compilable, *non-functional* implementation of
 *  host_platform.h so the project links from day one; you then
 *  replace the bodies one at a time as you bring the target up.
 *
 *  Two ways to use it
 *  ------------------
 *  1. WEAK OVERRIDE (drop-in, recommended for incremental bring-up):
 *     Build this file into your target. Every function here is
 *     declared __attribute__((weak)). As soon as you provide a
 *     NON-weak function with the same name in another translation
 *     unit, the linker uses yours instead — no edits to this file,
 *     no registration. Implement them incrementally; the weak stub
 *     covers whatever you haven't done yet.
 *
 *     CAUTION with weak symbols:
 *       - The override must match the signature EXACTLY. A mismatch
 *         doesn't error — it just fails to override, and this stub
 *         silently runs instead. If an override "isn't taking,"
 *         check the prototype first.
 *       - Weak-symbol resolution can differ across toolchains and
 *         especially across static-link / LTO scenarios. GCC
 *         (including xPack riscv-none-elf-gcc) and Clang honor it;
 *         MSVC does not have a portable equivalent (use option 2).
 *
 *  2. EDIT-IN-PLACE: just fill in the bodies here and build only
 *     this file (drop the weak attribute or leave it; with no
 *     competing definition it's harmless). Simplest for MSVC or if
 *     you dislike weak symbols.
 *
 *  Each function documents what a real implementation must do. The
 *  stub behavior is chosen to be safe-but-obviously-inert:
 *    - time returns 0 / "no clock"
 *    - sleep busy-spins a tiny bounded amount (so a run loop that
 *      relies on sleep doesn't peg the CPU AND doesn't hang)
 *    - the stop hook never raises (host runs until power-off)
 * ============================================================ */

#include "vm/host_platform.h"

/* WEAK: provide your own same-named function to override any of
 * these. If your toolchain doesn't support weak symbols, delete the
 * macro definition (make it empty) and edit the bodies directly. */
#if defined(__GNUC__) || defined(__clang__)
#  define HP_WEAK __attribute__((weak))
#else
#  define HP_WEAK   /* no weak support (e.g. MSVC): edit bodies */
#endif

/* ------------------------------------------------------------
 *  Time
 *
 *  Real implementation: return a free-running millisecond counter.
 *  On STM32 this is typically the SysTick-driven HAL_GetTick(), or
 *  a TIM in up-counting mode. Must be monotonic and wrap cleanly at
 *  2^32. The scheduler uses this for sys_sleep_ticks / timeouts, so
 *  if it returns a constant, sleeping VMs never wake.
 * ------------------------------------------------------------ */
HP_WEAK uint32_t host_platform_monotonic_ms(void *userdata) {
    (void)userdata;
    /* TODO(port): return HAL_GetTick() or equivalent. */
    return 0;
}

/* Real implementation: fill seconds/nanos from an RTC. Return false
 * if the target has no real-time clock — SYS_REALTIME_NOW then
 * reports -ENOSYS, which is fine. */
HP_WEAK bool host_platform_realtime(void *userdata,
                                    uint32_t *seconds_out,
                                    uint32_t *nanos_out) {
    (void)userdata; (void)seconds_out; (void)nanos_out;
    /* TODO(port): read RTC, or leave returning false (no clock). */
    return false;
}

/* ------------------------------------------------------------
 *  Sleep
 *
 *  Real implementation: block/idle the CPU for ~ms. On an RTOS this
 *  is osDelay(ms) / vTaskDelay; bare-metal, a SysTick busy-wait or a
 *  WFI loop. The host run loop calls this when no VM is runnable, so
 *  a no-op turns the loop into a 100% busy-spin (works, but wastes
 *  power). The stub does a tiny bounded spin as a placeholder.
 * ------------------------------------------------------------ */
HP_WEAK void host_platform_sleep_ms(unsigned ms) {
    /* TODO(port): osDelay(ms) / vTaskDelay / WFI. Placeholder spin
     * is intentionally crude and bounded — replace it. */
    volatile unsigned long spin = (unsigned long)ms * 1000ul;
    while (spin--) { /* nothing */ }
}

/* ------------------------------------------------------------
 *  Stop / interrupt hook
 *
 *  On a desktop this wires Ctrl-C to a stop flag. On an MCU there
 *  may be no such concept — a button IRQ, a watchdog, or simply
 *  "runs forever." The stub never raises the flag, so the host run
 *  loop runs until reset/power-off, which is the usual MCU case.
 *  If you want a stop source (e.g. a GPIO button), set the flag from
 *  your ISR by calling host_platform_request_stop().
 * ------------------------------------------------------------ */
static volatile int g_stop = 0;

HP_WEAK void host_platform_install_stop_handler(void) {
    /* TODO(port): optional — register a button/UART-break IRQ that
     * calls host_platform_request_stop(). No-op is valid. */
}

HP_WEAK bool host_platform_stop_requested(void) {
    return g_stop != 0;
}

HP_WEAK void host_platform_request_stop(void) {
    g_stop = 1;
}
