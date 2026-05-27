/* 02_counter/guest.c — incrementing counter that yields between
 * iterations.
 *
 * Demonstrates:
 *   - SYS_YIELD: voluntarily giving up the rest of the current
 *     quantum, letting the scheduler do its job
 *   - A real (if simple) main loop in the guest
 *   - Number-to-decimal-string formatting without libc
 *
 * Compared to 01_hello, this guest never exits on its own — it
 * loops forever. The host bounds runtime via a cycle cap.
 *
 * Public domain (CC0). No warranty.
 */

#define SYS_WRITE 64
#define SYS_YIELD 1040
#define SYS_EXIT  93

static inline int sys_write(int fd, const void *buf, unsigned n) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)buf;
    register unsigned a2 asm("a2") = n;
    register int      a7 asm("a7") = SYS_WRITE;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline void sys_yield(void) {
    register int a7 asm("a7") = SYS_YIELD;
    asm volatile ("ecall" : : "r"(a7) : "memory");
}

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* Format an unsigned integer into the END of buf as decimal,
 * returning a pointer to the first character of the resulting
 * string. (Format-from-the-end is the standard idiom for this
 * since we generate the digits least-significant first.) */
static char *format_u32(unsigned v, char *buf_end) {
    *--buf_end = '\0';
    if (v == 0) { *--buf_end = '0'; return buf_end; }
    while (v > 0) {
        *--buf_end = (char)('0' + (v % 10));
        v /= 10;
    }
    return buf_end;
}

void _start(void) {
    unsigned counter = 0;
    char line[32];
    char numbuf[16];

    /* Suppress unused warning if the guest is later changed to
     * not call sys_exit. */
    (void)sys_exit;

    for (;;) {
        /* Build "counter: N\n" in a stack-local buffer. */
        const char prefix[] = "counter: ";
        const unsigned prefix_len = sizeof(prefix) - 1;
        unsigned i;
        for (i = 0; i < prefix_len; i++) line[i] = prefix[i];

        char *num = format_u32(counter, numbuf + sizeof(numbuf));
        unsigned num_len = 0;
        while (num[num_len]) num_len++;
        for (i = 0; i < num_len; i++) line[prefix_len + i] = num[i];
        line[prefix_len + num_len] = '\n';

        sys_write(1, line, prefix_len + num_len + 1);

        counter++;
        sys_yield();   /* Let the scheduler tick over. */
    }
}
