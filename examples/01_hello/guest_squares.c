/* 01_hello/guest_squares.c — yet another variant.
 *
 * Computes sum of squares 1..10 = 385 in a real loop (volatile
 * prevents constant-folding), then exits with the result.
 * Exercises RV32M's mul, branches, sp-relative load/store,
 * compressed jumps.
 *
 * Used by the test suite to verify the M extension and the
 * tighter end-to-end of compute-heavy guests. The host doesn't
 * use this guest directly. */

#define SYS_EXIT 93

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

void _start(void) {
    volatile int n = 10;
    int sum = 0;
    for (int i = 1; i <= n; i++) {
        sum += i * i;
    }
    sys_exit(sum);
}
