/* ============================================================
 *  vm_host_platform.h — small platform services
 *
 *  A grab-bag of libc-shaped primitives that live in the host
 *  so guests don't have to carry implementations. The aim is
 *  to keep each guest small without committing the host to
 *  general-purpose libc emulation.
 *
 *  What's here:
 *    - SYS_FORMAT_AND_WRITE   printf-style formatter to an fd
 *    - SYS_FORMAT_TO_BUF      snprintf-style formatter to a buffer
 *    - SYS_REALTIME_NOW       wall-clock time (optional)
 *    - SYS_ALLOC_SIZE         introspect a SYS_ALLOC block's size
 *    - SYS_RAND               host-managed PRNG
 *    - SYS_TIMING_DEADLINE_REMAINING  frame-budget helper
 *
 *  Each service has a clean "this isn't supported" path: a
 *  syscall registered but with no backing source returns
 *  -ENOSYS. That's how the realtime service stays optional —
 *  the host can install the syscalls without providing a
 *  realtime source, and guests get a clean answer.
 *
 *  ---------------------------------------------------------------
 *  Installation
 *  ---------------------------------------------------------------
 *
 *      VmSystem sys;
 *      vm_system_init(&sys, &cfg);
 *      vm_host_install_stdio(&sys);
 *      vm_host_install_fs(&sys);          // optional
 *      vm_host_install_platform(&sys);    // adds the syscalls here
 *
 *  The order between platform and the other installs doesn't
 *  matter. Platform doesn't hook into stdio or fs; it stands
 *  alone.
 *
 *  ---------------------------------------------------------------
 *  Realtime configuration
 *  ---------------------------------------------------------------
 *
 *  SYS_REALTIME_NOW is gated by a function-pointer in
 *  VmHostPlatformConfig. If realtime_source is NULL, the syscall
 *  returns -ENOSYS. The shell host wires this to
 *  clock_gettime(CLOCK_REALTIME); embedded hosts without an RTC
 *  leave it NULL.
 *
 *  ---------------------------------------------------------------
 *  PRNG configuration
 *  ---------------------------------------------------------------
 *
 *  SYS_RAND uses a SplitMix64 chain. The seed comes from
 *  VmHostPlatformConfig.rand_seed (a u64). If left zero, the
 *  host uses a default deterministic seed — which is fine for
 *  games but bad for anything security-sensitive. The shell
 *  host seeds from clock_gettime nanoseconds.
 *
 *  ---------------------------------------------------------------
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_VM_HOST_PLATFORM_H
#define MICROGARBAGE_VM_HOST_PLATFORM_H

#include "vm/vm_system.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  Configuration
 * ============================================================ */

/* Optional wall-clock source. Called by the SYS_REALTIME_NOW
 * handler. Fills *seconds_out and *nanos_out with the current
 * time relative to the Unix epoch. Return true on success,
 * false to indicate "not available right now" (the guest sees
 * -ENOSYS). If the host doesn't provide this at all, set
 * realtime_source to NULL and SYS_REALTIME_NOW always returns
 * -ENOSYS. */
typedef bool (*VmRealtimeSource)(void *userdata,
                                  uint32_t *seconds_out,
                                  uint32_t *nanos_out);

typedef struct {
    /* Wall-clock source. NULL means SYS_REALTIME_NOW returns
     * -ENOSYS. */
    VmRealtimeSource realtime_source;
    void            *realtime_userdata;

    /* Seed for the SYS_RAND PRNG. If zero, the host uses a
     * default deterministic seed (good for repeatable tests,
     * bad for security). */
    uint64_t         rand_seed;
} VmHostPlatformConfig;

/* ============================================================
 *  Installation
 * ============================================================ */

/* Install the platform syscall handlers on `sys`. The config
 * may be NULL — in that case realtime_source defaults to NULL
 * (SYS_REALTIME_NOW returns -ENOSYS) and rand_seed defaults to
 * a hardcoded value. Returns false if any registration fails. */
bool vm_host_install_platform(VmSystem *sys,
                               const VmHostPlatformConfig *cfg);

/* ============================================================
 *  Wire formats (for handler authors and guest libraries)
 * ============================================================ */

/* VmRealtimeRecord: written to guest memory by SYS_REALTIME_NOW.
 * 12 bytes total. seconds is the Unix epoch second; nanos is
 * 0..999_999_999. */
typedef struct {
    uint32_t version;          /* layout version; currently 1 */
    uint32_t seconds;          /* Unix epoch (low 32 bits) */
    uint32_t nanos;            /* 0..999_999_999 */
} VmRealtimeRecord;

/* SYS_FORMAT_AND_WRITE argument layout:
 *
 *   a0 = fd                (where to write)
 *   a1 = guest pointer to format string (null-terminated)
 *   a2 = guest pointer to args array (array of u32)
 *   a3 = number of args in the array (0..VM_FORMAT_MAX_ARGS)
 *
 *   → a0 = bytes written, or -errno
 *
 * SYS_FORMAT_TO_BUF argument layout:
 *
 *   a0 = guest pointer to output buffer
 *   a1 = buffer capacity in bytes (excluding null)
 *   a2 = guest pointer to format string
 *   a3 = guest pointer to args array
 *   a4 = number of args
 *
 *   → a0 = bytes written (excluding null), or -errno. The
 *          formatter always null-terminates the output if cap > 0.
 *
 * Supported conversions:
 *     %s          string (guest pointer in args)
 *     %d, %i      signed decimal
 *     %u          unsigned decimal
 *     %x, %X      hex (lower/upper)
 *     %o          octal
 *     %c          single character
 *     %p          pointer (8 hex chars, no 0x prefix)
 *     %%          literal '%'
 *
 * Supported flags: '-' (left-align), '0' (zero-pad), '+' (force sign),
 *                  ' ' (space-for-positive), '#' (alt form for x/X/o)
 * Supported width: decimal number, or '*' to consume an arg
 * Supported precision: '.<num>' or '.*'
 * Length modifiers: ignored. No %f support (intentional — soft float
 * isn't free, and RV32IMC has no FPU). */

#define VM_FORMAT_MAX_ARGS      16
#define VM_FORMAT_MAX_FMT_LEN   1024
#define VM_FORMAT_MAX_STR_ARG   512

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_VM_HOST_PLATFORM_H */
