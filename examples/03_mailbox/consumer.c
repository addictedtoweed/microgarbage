/* 03_mailbox/consumer.c — receives messages from the producer and
 * prints them to host stdout, then exits.
 *
 * Demonstrates:
 *   - SYS_WHITELIST_ADD to allow a specific sender
 *   - SYS_RECV to wait for a message (blocking with a timeout)
 *   - The consumer half of a producer/consumer pair
 *
 * The consumer must whitelist the producer's vm_id before any
 * sends will succeed. The producer handles the race window where
 * it tries to send before the whitelist is set up (it gets -EPERM,
 * yields, and retries).
 */

#define SYS_WHITELIST_ADD  1075
#define SYS_RECV           1073
#define SYS_WRITE          64
#define SYS_YIELD          1040
#define SYS_EXIT           93

#define PRODUCER_VM_ID     0
#define MESSAGES_TO_RECEIVE  10

static inline int sys_whitelist_add(unsigned sender_id) {
    register int a0 asm("a0") = (int)sender_id;
    register int a7 asm("a7") = SYS_WHITELIST_ADD;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int sys_recv(void *dest, unsigned timeout) {
    register int      a0 asm("a0") = (int)(unsigned long)dest;
    register unsigned a1 asm("a1") = timeout;
    register int      a7 asm("a7") = SYS_RECV;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
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

/* Format an unsigned decimal at the end of buf. Returns pointer
 * to the start of the formatted string. */
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
    /* Allow the producer to send to us. Without this, the first
     * SYS_SEND from the producer would return -EPERM. */
    sys_whitelist_add(PRODUCER_VM_ID);

    unsigned char payload[32];
    char line[64];
    char numbuf[16];

    for (unsigned received = 0; received < MESSAGES_TO_RECEIVE; received++) {
        /* Block with a generous timeout — 100k step-quanta is
         * plenty for the producer to send to us. 0 would be
         * non-blocking poll; UINT32_MAX would be wait forever.
         * In between, the scheduler unblocks us as soon as a
         * message is delivered (synchronous-delivery path in
         * the SYS_SEND handler). */
        int sender = sys_recv(payload, 100000);

        if (sender < 0) {
            /* -ETIMEDOUT or other error. Bail. */
            const char *msg = "consumer: recv error, exiting\n";
            unsigned len = 0; while (msg[len]) len++;
            sys_write(1, msg, len);
            sys_exit(1);
        }

        /* Decode the LE uint32 in the first 4 bytes. */
        unsigned counter = (unsigned)payload[0]
                          | ((unsigned)payload[1] << 8)
                          | ((unsigned)payload[2] << 16)
                          | ((unsigned)payload[3] << 24);

        /* Build "consumer got: N from VM K\n". */
        const char p1[] = "consumer got: ";
        const char p2[] = " from VM ";
        const unsigned p1_len = sizeof(p1) - 1;
        const unsigned p2_len = sizeof(p2) - 1;
        unsigned out = 0, i;
        for (i = 0; i < p1_len; i++) line[out++] = p1[i];

        char *num = format_u32(counter, numbuf + sizeof(numbuf));
        while (*num) line[out++] = *num++;

        for (i = 0; i < p2_len; i++) line[out++] = p2[i];

        num = format_u32((unsigned)sender, numbuf + sizeof(numbuf));
        while (*num) line[out++] = *num++;

        line[out++] = '\n';
        sys_write(1, line, out);
    }

    const char *msg = "consumer: done\n";
    unsigned len = 0; while (msg[len]) len++;
    sys_write(1, msg, len);
    sys_exit(0);
}
