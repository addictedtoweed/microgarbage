/* 03_mailbox/producer.c — sends a counter to the consumer via
 * mailbox messages, then exits.
 *
 * Demonstrates:
 *   - SYS_SEND for inter-VM message passing
 *   - Handling -EPERM when the recipient hasn't whitelisted us
 *     yet (because we got scheduled first); retry with backoff
 *   - The producer half of a producer/consumer pair
 *
 * Pairs with consumer.c — the host loads both, and they cooperate
 * via the per-VM mailbox provided by vm_system_load_vm.
 *
 * Wire layout: payload is just a 32-byte buffer where the first 4
 * bytes hold a little-endian uint32 counter. The remaining 28
 * bytes are unused (the mailbox slot size is fixed at 32 bytes by
 * default). A real protocol would have a header, type tag, etc.,
 * but this is the simplest thing that works.
 *
 * Public domain (CC0). No warranty.
 */

#define SYS_SEND  1072
#define SYS_YIELD 1040
#define SYS_EXIT  93

#define VM_EPERM    1
#define VM_EAGAIN  11
#define VM_ENOENT   2

#define CONSUMER_VM_ID  1
#define MESSAGES_TO_SEND  10

/* Inline-asm syscall wrappers. */

static inline int sys_send(unsigned target_id, const void *payload, unsigned size) {
    register int      a0 asm("a0") = (int)target_id;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)payload;
    register unsigned a2 asm("a2") = size;
    register int      a7 asm("a7") = SYS_SEND;
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

void _start(void) {
    /* 32-byte payload — first 4 bytes hold the counter. */
    unsigned char payload[32];
    for (unsigned i = 0; i < sizeof(payload); i++) payload[i] = 0;

    for (unsigned counter = 0; counter < MESSAGES_TO_SEND; ) {
        /* Stuff the current counter into the payload as a
         * little-endian uint32. RV32 is LE so this is just the
         * native byte order. */
        payload[0] = (unsigned char)(counter & 0xFF);
        payload[1] = (unsigned char)((counter >> 8) & 0xFF);
        payload[2] = (unsigned char)((counter >> 16) & 0xFF);
        payload[3] = (unsigned char)((counter >> 24) & 0xFF);

        int r = sys_send(CONSUMER_VM_ID, payload, sizeof(payload));

        if (r == 0) {
            /* Delivered. Advance the counter. */
            counter++;
            sys_yield();
        } else if (r == -VM_EPERM) {
            /* Consumer hasn't whitelisted us yet — yield and retry.
             * This commonly happens during the very first scheduling
             * round when the producer runs before the consumer has
             * had a chance to call SYS_WHITELIST_ADD. */
            sys_yield();
        } else if (r == -VM_EAGAIN) {
            /* Consumer's mailbox is full. Yield and retry — the
             * scheduler will run the consumer, which will drain
             * a slot. */
            sys_yield();
        } else {
            /* -ENOENT (consumer gone) or some other error.
             * Either way, time to bail. */
            sys_exit(1);
        }
    }

    sys_exit(0);
}
