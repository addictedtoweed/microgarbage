/* Computes the sum of squares of 1..10 using RV32M's mul.
 * Volatile counter prevents constant-folding. */

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
