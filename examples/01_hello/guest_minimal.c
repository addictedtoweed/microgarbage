/* 01_hello/guest_minimal.c — three-instruction test fixture.
 *
 * The simplest possible RV32IMC program:
 *
 *   li   a0, 0       ; exit code
 *   li   a7, 93      ; SYS_EXIT
 *   ecall
 *
 * Total: ~10 bytes (3 instructions, two of them compressed).
 * The test suite uses this to verify the dispatcher does exactly
 * what's expected for a minimum-viable program: two ALU ops
 * retire, then ECALL traps, then the handler sets halted.
 *
 * Not loaded by host.c — that uses guest.c (the actual "hello
 * world" demo). */

#define SYS_EXIT 93

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

void _start(void) {
    sys_exit(0);
}
