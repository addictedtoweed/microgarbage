/* host_files_src/count.c — sample spawnable guest.
 *
 * Built into host_files/count.elf by the shell's build.sh.
 * Run from inside the shell with:
 *
 *     run /host/count.elf
 *
 * Counts from 1 to 10, printing each number on its own line,
 * then exits with status 0. Useful for verifying that the
 * spawned VM has its own stack and can do real work, not just
 * print a static string.
 */

#define SYS_WRITE 64
#define SYS_EXIT  93

static inline int sys_write(int fd, const void *buf, unsigned n) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)buf;
    register unsigned a2 asm("a2") = n;
    register int      a7 asm("a7") = SYS_WRITE;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* Tiny "%d\n" formatter — emits the integer, then a newline. */
static void putn(unsigned n) {
    char buf[16];
    char *p = buf + sizeof(buf);
    *--p = '\n';
    if (n == 0) {
        *--p = '0';
    } else {
        while (n) {
            *--p = (char)('0' + n % 10);
            n /= 10;
        }
    }
    sys_write(1, p, (unsigned)((buf + sizeof(buf)) - p));
}

void _start(void) {
    for (unsigned i = 1; i <= 10; i++) putn(i);
    sys_exit(0);
}
