/* ============================================================
 *  vm_runtime.h — guest-side runtime (internal)
 *
 *  This header is NOT meant to be included by guest application
 *  code. Guests should just #include the standard headers
 *  (<stdio.h>, <stdlib.h>, <string.h>, <time.h>) which live in
 *  the same lib/include/ directory.
 *
 *  This header declares the syscall numbers and a few internal
 *  helpers used by vm_runtime.c. It's a single place to keep
 *  the syscall ABI numbers in sync with the host's vm_ecall.h.
 *
 *  ---------------------------------------------------------------
 *  Linker-script requirements
 *  ---------------------------------------------------------------
 *
 *  vm_runtime.c's _start needs two symbols defined by the linker
 *  script to zero BSS at startup:
 *
 *      __bss_start
 *      __bss_end
 *
 *  examples/common/guest.ld provides these. If you build a guest
 *  with a different script, define them yourself or stub out
 *  the BSS-zero loop in _start.
 *
 *  ---------------------------------------------------------------
 *  Weak _start
 *  ---------------------------------------------------------------
 *
 *  The runtime's _start is declared __attribute__((weak)) so that
 *  a legacy guest that supplies its own _start (and doesn't have
 *  main()) still links cleanly. The linker drops the runtime's
 *  weak _start when a strong one exists; --gc-sections then
 *  removes the unreferenced extern reference to main() that
 *  would otherwise be undefined.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_VM_RUNTIME_H
#define MICROGARBAGE_VM_RUNTIME_H

#include <stdint.h>
#include <stddef.h>

/* ---------- Syscall numbers (must match host vm_ecall.h) ---------- */

#define SYS_OPENAT                    56
#define SYS_CLOSE                     57
#define SYS_LSEEK                     62
#define SYS_READ                      63
#define SYS_WRITE                     64
#define SYS_FFLUSH                    82
#define SYS_EXIT                      93
#define SYS_READDIR                  120
#define SYS_MKDIRAT                   34
#define SYS_UNLINKAT                  35

#define SYS_SELF                    1024
#define SYS_TICKS_NOW               1043
#define SYS_TICK_HZ                 1044
#define SYS_SLEEP_TICKS             1045
#define SYS_SLEEP_UNTIL             1046

#define SYS_ALLOC                   1056
#define SYS_FREE                    1057

#define SYS_MEMCPY                  1088
#define SYS_MEMSET                  1089
#define SYS_MEMMOVE                 1090
#define SYS_MEMCMP                  1091
#define SYS_STRLEN                  1092
#define SYS_STRCMP                  1093
#define SYS_STRCHR                  1094

#define SYS_SPAWN_AND_WAIT          1104
#define SYS_TTY_SET_RAW             1105

/* T.1 platform services */
#define SYS_FORMAT_AND_WRITE        1108
#define SYS_FORMAT_TO_BUF           1109
#define SYS_REALTIME_NOW            1110
#define SYS_ALLOC_SIZE              1111
#define SYS_RAND                    1112
#define SYS_TIMING_DEADLINE_REMAINING 1113

/* Audio (must match host vm_ecall.h 1160..1167) */
#define SYS_AUDIO_LOAD_SAMPLE       1160
#define SYS_AUDIO_LOAD_MUSIC        1161
#define SYS_AUDIO_FREE              1162
#define SYS_AUDIO_TRIGGER_SFX       1163
#define SYS_AUDIO_PLAY_MUSIC        1164
#define SYS_AUDIO_STOP             1165
#define SYS_AUDIO_SET_GAIN          1166
#define SYS_AUDIO_GET_LEVELS        1167
#define SYS_AUDIO_FFT_ENABLE        1168
#define SYS_AUDIO_LOAD_WAV          1169
#define SYS_AUDIO_STREAM_WAV        1170

/* ---------- Inline syscall helpers ----------
 * Six variants by arity. All return a0 unchanged from the syscall.
 * Clobber a0 (return), preserve a1..a6 (they're used as inputs).
 */
static inline uint32_t _vm_sys0(uint32_t n) {
    register uint32_t a0 asm("a0");
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys1(uint32_t n, uint32_t x0) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys2(uint32_t n, uint32_t x0, uint32_t x1) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys3(uint32_t n, uint32_t x0, uint32_t x1, uint32_t x2) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a2 asm("a2") = x2;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys4(uint32_t n, uint32_t x0, uint32_t x1,
                                 uint32_t x2, uint32_t x3) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a2 asm("a2") = x2;
    register uint32_t a3 asm("a3") = x3;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}
static inline uint32_t _vm_sys5(uint32_t n, uint32_t x0, uint32_t x1,
                                 uint32_t x2, uint32_t x3, uint32_t x4) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a2 asm("a2") = x2;
    register uint32_t a3 asm("a3") = x3;
    register uint32_t a4 asm("a4") = x4;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0)
                  : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7)
                  : "memory");
    return a0;
}

#endif /* MICROGARBAGE_VM_RUNTIME_H */
