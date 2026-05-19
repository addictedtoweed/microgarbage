/* 01_hello/guest_factorial.c — slightly more interesting hello.
 *
 * Calls factorial(5) and exits with the result (= 120). The
 * compiler's optimizer constant-folds factorial(5) but still
 * emits the call/return sequence and stack frame, so this
 * exercises ra (return address), sp (stack pointer), jal, and
 * function epilogues — more than guest.c does.
 *
 * Used by the test suite to verify the dispatcher handles call
 * conventions correctly. Build via build.sh; the host doesn't
 * use this guest directly. */

#define SYS_EXIT 93

__attribute__((noinline))
static int factorial(int n) {
    if (n <= 1) return 1;
    return n * factorial(n - 1);
}

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

void _start(void) {
    sys_exit(factorial(5));
}
