/* host_files_src/hello.c — sample spawnable guest.
 *
 * Built into host_files/hello.elf by the shell's build.sh.
 * Run from inside the shell with:
 *
 *     run /host/hello.elf
 *
 * Demonstrates that the shell can load and execute an ELF
 * file from the host's real filesystem (via the /host mount)
 * as a child VM.
 *
 * The child shares the shell's stdio, so this output appears
 * on the user's terminal interleaved with the shell's prompts.
 *
 * Public domain (CC0). No warranty.
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

static unsigned slen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

void _start(void) {
    const char *msg = "hi from spawned VM\n";
    sys_write(1, msg, slen(msg));
    sys_exit(0);
}
