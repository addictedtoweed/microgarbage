/* 01_hello/guest.c — minimal "hello world" guest.
 *
 * Demonstrates:
 *   - The bare-minimum guest program structure (no libc, no
 *     startup code; _start is the entry point)
 *   - SYS_WRITE for console output via the host
 *   - SYS_EXIT for clean termination
 *
 * Build with the toolchain via the shared linker script:
 *   riscv64-unknown-elf-gcc -march=rv32imc -mabi=ilp32 \
 *       -nostdlib -nostartfiles -ffreestanding -O2 \
 *       -Wl,-T,../common/guest.ld -o build/guest.elf guest.c
 *
 * The build.sh in this directory does this for you.
 */

/* Syscall numbers. See include/vm/vm_ecall.h for the full ABI. */
#define SYS_WRITE 64
#define SYS_EXIT  93

/* Inline-asm wrappers. The asm constraints pin specific registers
 * (a0..a2 for args, a7 for syscall number) and the "memory"
 * clobber tells the compiler that the call may read/write
 * arbitrary memory through the pointers we passed. */

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

/* String length helper. We can't use libc <string.h> because
 * we're freestanding. */
static unsigned guest_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

void _start(void) {
    const char *msg = "Hello from inside the VM!\n";
    sys_write(1, msg, guest_strlen(msg));
    sys_exit(0);
}
