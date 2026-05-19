/* 04_keydump/guest.c — diagnostic guest that prints each input
 * byte as a bracketed hex value.
 *
 * Demonstrates:
 *   - SYS_READ for non-blocking host stdin input
 *   - Polling pattern: read → yield → read, never blocking
 *   - Handling -EIO (stdin closed) cleanly
 *
 * Useful for figuring out what your terminal sends for arrow
 * keys, function keys, modifier combinations, mouse clicks (with
 * mouse reporting enabled), bracketed paste boundaries, etc.
 *
 * Run with the host in raw-terminal mode (which the host does by
 * default for this example), then press keys:
 *
 *   $ ./build/host
 *   keydump: press 'q' to quit
 *   [up arrow][down arrow][q]
 *   [1B][5B][41][1B][5B][42][71]
 *   keydump: 'q' pressed, exiting
 *
 * Reading the bytes: 0x1B is ESC, 0x5B is '[', 0x41 is 'A' — so
 * up arrow is the 3-byte sequence ESC [ A, matching xterm
 * convention. PuTTY uses the same sequences.
 */

#define SYS_READ   63
#define SYS_WRITE  64
#define SYS_YIELD  1040
#define SYS_EXIT   93

static inline int sys_read(int fd, void *buf, unsigned n) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)buf;
    register unsigned a2 asm("a2") = n;
    register int      a7 asm("a7") = SYS_READ;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

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

/* Format one byte as "[XX]" into out (4 chars). */
static unsigned format_hex_byte(unsigned char b, char *out) {
    static const char hex[] = "0123456789ABCDEF";
    out[0] = '[';
    out[1] = hex[(b >> 4) & 0xF];
    out[2] = hex[b & 0xF];
    out[3] = ']';
    return 4;
}

void _start(void) {
    const char banner[] = "keydump: press 'q' to quit\r\n";
    sys_write(1, banner, sizeof(banner) - 1);

    char inbuf[32];
    char outbuf[256];

    for (;;) {
        int r = sys_read(0, inbuf, sizeof(inbuf));

        if (r < 0) {
            /* -EIO: stdin closed. Exit cleanly. */
            const char msg[] = "\r\nkeydump: stdin closed\r\n";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_exit(0);
        }
        if (r == 0) {
            /* No bytes ready right now. Yield and try again.
             * Without this yield we'd burn the whole quantum
             * spinning on read. */
            sys_yield();
            continue;
        }

        /* Format each byte as [XX] and emit. Deliberately no
         * spaces or newlines between bytes so a multi-byte
         * sequence like an arrow key appears as a single visual
         * unit like "[1B][5B][41]". */
        unsigned out_len = 0;
        unsigned char quit = 0;
        for (int i = 0; i < r; i++) {
            unsigned char b = (unsigned char)inbuf[i];
            out_len += format_hex_byte(b, outbuf + out_len);
            if (b == 'q') quit = 1;
        }
        sys_write(1, outbuf, out_len);

        if (quit) {
            const char msg[] = "\r\nkeydump: 'q' pressed, exiting\r\n";
            sys_write(1, msg, sizeof(msg) - 1);
            sys_exit(0);
        }
    }
}
