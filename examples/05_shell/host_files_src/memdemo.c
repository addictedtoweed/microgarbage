/* host_files_src/memdemo.c — slab allocator demo guest.
 *
 * Built into host_files/memdemo.elf by the shell's build.sh.
 * Run from inside the shell with:
 *
 *     run /host/memdemo.elf
 *
 * What it does:
 *   1. Allocates three blocks via SYS_ALLOC (sizes 100, 200, 500 B)
 *   2. Frees the middle one explicitly
 *   3. Prints what it allocated and what it leaked
 *   4. Exits without freeing the other two blocks
 *
 * The point: when the guest exits, the host's vm_system_unload_vm
 * walks this VM's per-VM SYS_ALLOC tracking and reclaims the
 * leaked blocks. The user can verify by running 'meminfo' before
 * and after:
 *
 *     [/]$ meminfo
 *     ...shared:    0 /  14K ... allocs    0  frees    0
 *
 *     [/]$ run /host/memdemo.elf
 *     memdemo: alloc 100 -> 0xC0000008
 *     memdemo: alloc 200 -> 0xC0000088
 *     memdemo: alloc 500 -> 0xC0000208
 *     memdemo: freed middle block
 *     memdemo: leaking 2 blocks on exit (auto-cleanup will reclaim)
 *
 *     [/]$ meminfo
 *     ...shared:    0 /  14K ... allocs    3  frees    3
 *
 * The 'allocs' and 'frees' counts both ticked up to 3: the guest
 * called SYS_ALLOC three times, freed once explicitly, and the
 * host freed the other two implicitly during unload.
 */

#define SYS_WRITE 64
#define SYS_EXIT  93
#define SYS_ALLOC 1056
#define SYS_FREE  1057

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

static inline unsigned sys_alloc(unsigned size) {
    register unsigned a0 asm("a0") = size;
    register int      a7 asm("a7") = SYS_ALLOC;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int sys_free(unsigned ptr) {
    register unsigned a0 asm("a0") = ptr;
    register int      a7 asm("a7") = SYS_FREE;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return (int)a0;
}

static unsigned slen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

static void puts_(const char *s) {
    sys_write(1, s, slen(s));
}

static char *fmt_hex(unsigned v, char *buf_end) {
    *--buf_end = '\0';
    if (v == 0) { *--buf_end = '0'; return buf_end; }
    while (v) {
        unsigned d = v & 0xFu;
        *--buf_end = (char)(d < 10 ? '0' + d : 'A' + d - 10);
        v >>= 4;
    }
    *--buf_end = 'x';
    *--buf_end = '0';
    return buf_end;
}

static char *fmt_u(unsigned v, char *buf_end) {
    *--buf_end = '\0';
    if (v == 0) { *--buf_end = '0'; return buf_end; }
    while (v) { *--buf_end = (char)('0' + v % 10); v /= 10; }
    return buf_end;
}

static void report(const char *prefix, unsigned size, unsigned ptr) {
    char buf[24];
    puts_(prefix);
    puts_(" ");
    puts_(fmt_u(size, buf + sizeof(buf)));
    puts_(" -> ");
    puts_(fmt_hex(ptr, buf + sizeof(buf)));
    puts_("\n");
}

void _start(void) {
    puts_("memdemo: SYS_ALLOC tracking + auto-cleanup demo\n");

    unsigned a = sys_alloc(100);
    report("memdemo: alloc", 100, a);

    unsigned b = sys_alloc(200);
    report("memdemo: alloc", 200, b);

    unsigned c = sys_alloc(500);
    report("memdemo: alloc", 500, c);

    /* Free the middle one. */
    int r = sys_free(b);
    if (r == 0) {
        puts_("memdemo: freed middle block (200 B)\n");
    } else {
        puts_("memdemo: SYS_FREE failed\n");
    }

    /* Don't bother free()ing a and c — exit and let the host
     * auto-cleanup take care of them. */
    puts_("memdemo: leaking 2 blocks on exit (host auto-cleanup will reclaim)\n");
    sys_exit(0);
}
