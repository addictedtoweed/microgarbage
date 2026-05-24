/* Tests for vm_mailbox. */

#include "test_runner.h"
#include "vm/vm_mailbox.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* Helper: a 32-byte payload struct. Sized to match the typical
 * default slot size; tests can use this directly. */
typedef struct {
    uint32_t kind;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint8_t  data[16];
} TestMsg;

/* ============================================================
 *  Init and basic state
 * ============================================================ */

static void test_init_basic(void) {
    VmMailbox m;
    /* Storage for 4 slots of 32-byte payloads = (32 + 2) * 4 = 136 bytes */
    uint8_t storage[(32 + 2) * 4];
    VmMailboxResult r = vm_mailbox_init(&m, storage, 32, 4);
    ASSERT_EQ_INT(VM_MBOX_OK, r);
    ASSERT_EQ_INT(32, m.slot_size);
    ASSERT_EQ_INT(4, m.depth);
    ASSERT_EQ_INT(0, (int)m.sends_accepted);
    ASSERT_EQ_INT(0, (int)m.recvs);
    ASSERT(vm_mailbox_empty(&m));
    ASSERT(!vm_mailbox_full(&m));
}

static void test_init_rejects_null_handle(void) {
    uint8_t storage[64];
    ASSERT_EQ_INT(VM_MBOX_ERR_INVALID_ARG,
                  vm_mailbox_init(NULL, storage, 16, 4));
}

static void test_init_rejects_null_storage(void) {
    VmMailbox m;
    ASSERT_EQ_INT(VM_MBOX_ERR_INVALID_ARG,
                  vm_mailbox_init(&m, NULL, 16, 4));
}

static void test_init_rejects_zero_slot_size(void) {
    VmMailbox m;
    uint8_t storage[64];
    ASSERT_EQ_INT(VM_MBOX_ERR_INVALID_ARG,
                  vm_mailbox_init(&m, storage, 0, 4));
}

static void test_init_rejects_zero_depth(void) {
    VmMailbox m;
    uint8_t storage[64];
    ASSERT_EQ_INT(VM_MBOX_ERR_INVALID_ARG,
                  vm_mailbox_init(&m, storage, 16, 0));
}

static void test_init_rejects_oversized_slot(void) {
    VmMailbox m;
    uint8_t storage[64];
    ASSERT_EQ_INT(VM_MBOX_ERR_INVALID_ARG,
                  vm_mailbox_init(&m, storage,
                                  VM_MAILBOX_MAX_SLOT_SIZE + 1, 1));
}

static void test_required_storage_helper(void) {
    /* 16 bytes payload + 2 bytes overhead = 18 per slot, × 8 = 144 */
    ASSERT_EQ_INT(144, (int)vm_mailbox_required_storage_bytes(16, 8));

    /* Edge cases — zero anything returns zero. */
    ASSERT_EQ_INT(0, (int)vm_mailbox_required_storage_bytes(0, 8));
    ASSERT_EQ_INT(0, (int)vm_mailbox_required_storage_bytes(16, 0));

    /* Oversized — returns zero. */
    ASSERT_EQ_INT(0, (int)vm_mailbox_required_storage_bytes(
                         VM_MAILBOX_MAX_SLOT_SIZE + 1, 1));
}

/* ============================================================
 *  Whitelist
 * ============================================================ */

static void test_whitelist_default_empty(void) {
    VmMailbox m;
    uint8_t storage[64];
    vm_mailbox_init(&m, storage, 16, 2);

    /* No senders allowed by default. */
    ASSERT(!vm_mailbox_whitelist_check(&m, 0));
    ASSERT(!vm_mailbox_whitelist_check(&m, 1));
    ASSERT(!vm_mailbox_whitelist_check(&m, 31));
}

static void test_whitelist_set_and_check(void) {
    VmMailbox m;
    uint8_t storage[64];
    vm_mailbox_init(&m, storage, 16, 2);

    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_whitelist_set(&m, 3));
    ASSERT(vm_mailbox_whitelist_check(&m, 3));
    ASSERT(!vm_mailbox_whitelist_check(&m, 4));
    ASSERT(!vm_mailbox_whitelist_check(&m, 2));
}

static void test_whitelist_clear(void) {
    VmMailbox m;
    uint8_t storage[64];
    vm_mailbox_init(&m, storage, 16, 2);

    vm_mailbox_whitelist_set(&m, 5);
    ASSERT(vm_mailbox_whitelist_check(&m, 5));

    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_whitelist_clear(&m, 5));
    ASSERT(!vm_mailbox_whitelist_check(&m, 5));
}

static void test_whitelist_set_all_and_clear_all(void) {
    VmMailbox m;
    uint8_t storage[64];
    vm_mailbox_init(&m, storage, 16, 2);

    vm_mailbox_whitelist_set_all(&m);
    ASSERT(vm_mailbox_whitelist_check(&m, 0));
    ASSERT(vm_mailbox_whitelist_check(&m, 7));
    ASSERT(vm_mailbox_whitelist_check(&m, VM_MAILBOX_WHITELIST_BITS - 1));

    vm_mailbox_whitelist_clear_all(&m);
    ASSERT(!vm_mailbox_whitelist_check(&m, 0));
    ASSERT(!vm_mailbox_whitelist_check(&m, 7));
}

static void test_whitelist_rejects_out_of_range_id(void) {
    VmMailbox m;
    uint8_t storage[64];
    vm_mailbox_init(&m, storage, 16, 2);

    ASSERT_EQ_INT(VM_MBOX_ERR_BAD_VM_ID,
                  vm_mailbox_whitelist_set(&m, VM_MAILBOX_WHITELIST_BITS));
    ASSERT_EQ_INT(VM_MBOX_ERR_BAD_VM_ID,
                  vm_mailbox_whitelist_set(&m, 65535));
}

static void test_whitelist_self_allowed(void) {
    /* A VM can whitelist its own ID for self-messaging. */
    VmMailbox m;
    uint8_t storage[64];
    vm_mailbox_init(&m, storage, 16, 2);

    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_whitelist_set(&m, 7));
    ASSERT(vm_mailbox_whitelist_check(&m, 7));
}

/* ============================================================
 *  Send and receive — happy path
 * ============================================================ */

static void test_send_recv_round_trip(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(sizeof(TestMsg), 4)];
    vm_mailbox_init(&m, storage, sizeof(TestMsg), 4);
    vm_mailbox_whitelist_set(&m, 1);

    TestMsg sent = { .kind = 42, .arg0 = 100, .arg1 = 200, .arg2 = 300 };
    memcpy(sent.data, "hello, mailbox!", 16);

    ASSERT_EQ_INT(VM_MBOX_OK,
                  vm_mailbox_send(&m, 1, &sent, sizeof(TestMsg)));
    ASSERT_EQ_INT(1, (int)m.sends_accepted);
    ASSERT_EQ_INT(1, (int)vm_mailbox_count(&m));

    TestMsg got = {0};
    uint16_t sender = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &got, &sender));
    ASSERT_EQ_INT(1, sender);
    ASSERT_EQ_INT(42, (int)got.kind);
    ASSERT_EQ_INT(100, (int)got.arg0);
    ASSERT_EQ_INT(200, (int)got.arg1);
    ASSERT_EQ_INT(300, (int)got.arg2);
    ASSERT(memcmp(got.data, "hello, mailbox!", 16) == 0);
    ASSERT_EQ_INT(1, (int)m.recvs);
    ASSERT(vm_mailbox_empty(&m));
}

static void test_send_fifo_ordering(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 8)];
    vm_mailbox_init(&m, storage, 4, 8);
    vm_mailbox_whitelist_set_all(&m);

    /* Send three messages from three different senders. */
    uint32_t p;
    p = 0xAAAA1111; ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 1, &p, 4));
    p = 0xBBBB2222; ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 2, &p, 4));
    p = 0xCCCC3333; ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 3, &p, 4));

    /* Recv in send order. */
    uint32_t got = 0;
    uint16_t sender = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &got, &sender));
    ASSERT_EQ_INT(1, sender);
    ASSERT_EQ_INT((int)0xAAAA1111, (int)got);
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &got, &sender));
    ASSERT_EQ_INT(2, sender);
    ASSERT_EQ_INT((int)0xBBBB2222, (int)got);
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &got, &sender));
    ASSERT_EQ_INT(3, sender);
    ASSERT_EQ_INT((int)0xCCCC3333, (int)got);
}

static void test_recv_with_null_sender_ok(void) {
    /* Caller may pass NULL for out_sender if they don't care. */
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    vm_mailbox_whitelist_set(&m, 1);

    uint32_t sent = 0xDEADBEEF;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 1, &sent, 4));

    uint32_t got = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &got, NULL));
    ASSERT_EQ_INT((int)0xDEADBEEF, (int)got);
}

/* ============================================================
 *  Send — error paths
 * ============================================================ */

static void test_send_rejected_not_whitelisted(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    /* Whitelist VM 1 only. */
    vm_mailbox_whitelist_set(&m, 1);

    uint32_t p = 0xCAFEBABE;
    ASSERT_EQ_INT(VM_MBOX_ERR_NOT_WHITELISTED,
                  vm_mailbox_send(&m, 2, &p, 4));
    ASSERT_EQ_INT(1, (int)m.sends_rejected_perm);
    ASSERT_EQ_INT(0, (int)m.sends_accepted);
    ASSERT(vm_mailbox_empty(&m));
}

static void test_send_rejected_full(void) {
    VmMailbox m;
    /* Depth 2 — fill it. */
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    vm_mailbox_whitelist_set(&m, 1);

    uint32_t p = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 1, &p, 4));
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 1, &p, 4));
    ASSERT(vm_mailbox_full(&m));

    /* Third send rejected. */
    ASSERT_EQ_INT(VM_MBOX_ERR_FULL, vm_mailbox_send(&m, 1, &p, 4));
    ASSERT_EQ_INT(1, (int)m.sends_rejected_full);
    ASSERT_EQ_INT(2, (int)m.sends_accepted);
}

static void test_send_rejected_wrong_size(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(16, 2)];
    vm_mailbox_init(&m, storage, 16, 2);
    vm_mailbox_whitelist_set(&m, 1);

    uint8_t payload[16] = {0};
    /* Mismatched size: should reject with INVALID_ARG. */
    ASSERT_EQ_INT(VM_MBOX_ERR_INVALID_ARG,
                  vm_mailbox_send(&m, 1, payload, 8));
    ASSERT_EQ_INT(VM_MBOX_ERR_INVALID_ARG,
                  vm_mailbox_send(&m, 1, payload, 32));
}

static void test_send_rejected_bad_vm_id(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    vm_mailbox_whitelist_set_all(&m);

    uint32_t p = 0;
    ASSERT_EQ_INT(VM_MBOX_ERR_BAD_VM_ID,
                  vm_mailbox_send(&m, VM_MAILBOX_WHITELIST_BITS, &p, 4));
}

static void test_send_rejected_null_payload(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    vm_mailbox_whitelist_set(&m, 1);

    ASSERT_EQ_INT(VM_MBOX_ERR_INVALID_ARG,
                  vm_mailbox_send(&m, 1, NULL, 4));
}

/* ============================================================
 *  Recv — error paths
 * ============================================================ */

static void test_recv_empty(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);

    uint32_t got = 0;
    uint16_t sender = 0;
    ASSERT_EQ_INT(VM_MBOX_ERR_EMPTY, vm_mailbox_recv(&m, &got, &sender));
    ASSERT_EQ_INT(0, (int)m.recvs);
}

static void test_recv_rejects_null_payload(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    vm_mailbox_whitelist_set(&m, 1);

    uint32_t p = 1;
    vm_mailbox_send(&m, 1, &p, 4);

    uint16_t sender = 0;
    ASSERT_EQ_INT(VM_MBOX_ERR_INVALID_ARG,
                  vm_mailbox_recv(&m, NULL, &sender));
    /* Message was not consumed. */
    ASSERT_EQ_INT(1, (int)vm_mailbox_count(&m));
}

/* ============================================================
 *  Peek
 * ============================================================ */

static void test_peek_does_not_consume(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    vm_mailbox_whitelist_set(&m, 5);

    uint32_t sent = 0x12345678;
    vm_mailbox_send(&m, 5, &sent, 4);

    uint32_t peeked = 0;
    uint16_t peeker_sender = 0;
    ASSERT_EQ_INT(VM_MBOX_OK,
                  vm_mailbox_peek(&m, &peeked, &peeker_sender));
    ASSERT_EQ_INT((int)0x12345678, (int)peeked);
    ASSERT_EQ_INT(5, peeker_sender);

    /* Still queued — peek didn't consume. */
    ASSERT_EQ_INT(1, (int)vm_mailbox_count(&m));
    /* peek did NOT bump recvs. */
    ASSERT_EQ_INT(0, (int)m.recvs);

    /* Subsequent recv returns the same message. */
    uint32_t got = 0;
    uint16_t got_sender = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &got, &got_sender));
    ASSERT_EQ_INT((int)0x12345678, (int)got);
    ASSERT_EQ_INT(5, got_sender);
}

static void test_peek_empty(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);

    uint32_t got = 0;
    uint16_t sender = 0;
    ASSERT_EQ_INT(VM_MBOX_ERR_EMPTY, vm_mailbox_peek(&m, &got, &sender));
}

static void test_peek_with_null_buffers(void) {
    /* Both buffers nullable — caller might just want existence check. */
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    vm_mailbox_whitelist_set(&m, 3);

    uint32_t p = 0xFEEDFACE;
    vm_mailbox_send(&m, 3, &p, 4);

    /* Just check it's present and from whom, ignoring payload. */
    uint16_t sender = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_peek(&m, NULL, &sender));
    ASSERT_EQ_INT(3, sender);

    /* Or just check existence. */
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_peek(&m, NULL, NULL));
}

/* ============================================================
 *  Reset
 * ============================================================ */

static void test_reset_drops_messages(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 4)];
    vm_mailbox_init(&m, storage, 4, 4);
    vm_mailbox_whitelist_set(&m, 1);

    uint32_t p = 0;
    vm_mailbox_send(&m, 1, &p, 4);
    vm_mailbox_send(&m, 1, &p, 4);
    ASSERT_EQ_INT(2, (int)vm_mailbox_count(&m));

    vm_mailbox_reset(&m);
    ASSERT_EQ_INT(0, (int)vm_mailbox_count(&m));
    ASSERT(vm_mailbox_empty(&m));
}

static void test_reset_preserves_whitelist(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    vm_mailbox_whitelist_set(&m, 7);

    vm_mailbox_reset(&m);
    /* Whitelist should still grant VM 7. */
    ASSERT(vm_mailbox_whitelist_check(&m, 7));
}

/* ============================================================
 *  Sender ID round-trips correctly across all valid IDs
 * ============================================================ */

static void test_sender_id_round_trip(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 4)];
    vm_mailbox_init(&m, storage, 4, 4);
    vm_mailbox_whitelist_set_all(&m);

    /* Send from a handful of different IDs and check each one
     * round-trips. */
    uint16_t test_ids[] = { 0, 1, 7, 31, VM_MAILBOX_WHITELIST_BITS - 1 };
    size_t n = sizeof(test_ids) / sizeof(test_ids[0]);

    for (size_t i = 0; i < n; i++) {
        uint32_t p = 0xA5A50000u | test_ids[i];
        ASSERT_EQ_INT(VM_MBOX_OK,
                      vm_mailbox_send(&m, test_ids[i], &p, 4));

        uint32_t got = 0;
        uint16_t sender = 0xFFFF;
        ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &got, &sender));
        ASSERT_EQ_INT(test_ids[i], sender);
        ASSERT_EQ_INT((int)(0xA5A50000u | test_ids[i]), (int)got);
    }
}

/* ============================================================
 *  Mixed payload sizes — small (1-byte) and larger (256-byte)
 * ============================================================ */

static void test_small_payload_one_byte(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(1, 4)];
    vm_mailbox_init(&m, storage, 1, 4);
    vm_mailbox_whitelist_set(&m, 1);

    uint8_t in = 0x77, out = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 1, &in, 1));
    uint16_t sender = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &out, &sender));
    ASSERT_EQ_INT(0x77, out);
    ASSERT_EQ_INT(1, sender);
}

static void test_large_payload_256_bytes(void) {
    VmMailbox m;
    uint8_t storage[vm_mailbox_required_storage_bytes(256, 2)];
    vm_mailbox_init(&m, storage, 256, 2);
    vm_mailbox_whitelist_set(&m, 4);

    uint8_t in[256];
    for (int i = 0; i < 256; i++) in[i] = (uint8_t)(i ^ 0x5A);

    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 4, in, 256));

    uint8_t out[256] = {0};
    uint16_t sender = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, out, &sender));
    ASSERT_EQ_INT(4, sender);
    ASSERT(memcmp(in, out, 256) == 0);
}

/* ============================================================
 *  Send-after-pop succeeds (refill cycle works correctly)
 * ============================================================ */

static void test_send_after_pop_cycles(void) {
    VmMailbox m;
    /* Depth 2 — small enough to exercise wraparound */
    uint8_t storage[vm_mailbox_required_storage_bytes(4, 2)];
    vm_mailbox_init(&m, storage, 4, 2);
    vm_mailbox_whitelist_set_all(&m);

    /* Push two, pop one, push one (wrap), pop one, push two, etc. */
    for (int cycle = 0; cycle < 10; cycle++) {
        uint32_t a = 0xA0000000u | (uint32_t)cycle;
        uint32_t b = 0xB0000000u | (uint32_t)cycle;

        ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 1, &a, 4));
        ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 2, &b, 4));
        ASSERT(vm_mailbox_full(&m));

        uint32_t got;
        uint16_t sender;
        ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &got, &sender));
        ASSERT_EQ_INT(1, sender);
        ASSERT_EQ_INT((int)(0xA0000000u | (uint32_t)cycle), (int)got);

        ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, &got, &sender));
        ASSERT_EQ_INT(2, sender);
        ASSERT_EQ_INT((int)(0xB0000000u | (uint32_t)cycle), (int)got);

        ASSERT(vm_mailbox_empty(&m));
    }
}

/* ============================================================
 *  Locker seam (single-threaded API behavior; the multithreaded
 *  TSan proof lives in docs/reference/)
 * ============================================================ */

static int g_lock_calls, g_unlock_calls;
static uintptr_t counting_lock(void *ctx)              { (void)ctx; g_lock_calls++; return 0x5A; }
static void      counting_unlock(void *ctx, uintptr_t s){ (void)ctx; g_unlock_calls++; ASSERT_EQ_INT(0x5A, (int)s); }

static void test_locker_invoked_balanced(void) {
    VmMailbox m;
    uint8_t storage[(16 + 2) * 4];
    vm_mailbox_init(&m, storage, 16, 4);
    vm_mailbox_whitelist_set(&m, 1);   /* one lock/unlock pair */

    g_lock_calls = g_unlock_calls = 0;
    VmMailboxLocker lk = { counting_lock, counting_unlock, NULL };
    vm_mailbox_set_locker(&m, lk);

    uint8_t payload[16] = {0};
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 1, payload, 16));   /* lock/unlock */
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, payload, NULL));    /* lock/unlock */

    /* Every guarded mutator takes exactly one lock and one unlock. */
    ASSERT_EQ_INT(g_lock_calls, g_unlock_calls);
    ASSERT(g_lock_calls >= 2);   /* at least the send + recv above */
}

static void test_locker_null_fallback(void) {
    VmMailbox m;
    uint8_t storage[(16 + 2) * 4];
    vm_mailbox_init(&m, storage, 16, 4);

    /* A locker with NULL fn pointers must fall back to the no-op locker
     * (not crash on the next mutator). */
    VmMailboxLocker bad = { NULL, NULL, NULL };
    vm_mailbox_set_locker(&m, bad);

    vm_mailbox_whitelist_set(&m, 1);
    uint8_t payload[16] = {0};
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 1, payload, 16));
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, payload, NULL));
}

static void test_locker_default_is_noop(void) {
    /* A mailbox that never calls set_locker behaves exactly as before the
     * seam existed: all mutators work with the implicit null locker. */
    VmMailbox m;
    uint8_t storage[(16 + 2) * 4];
    vm_mailbox_init(&m, storage, 16, 4);
    vm_mailbox_whitelist_set(&m, 2);
    uint8_t payload[16]; memset(payload, 0xCD, sizeof payload);
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_send(&m, 2, payload, 16));
    uint16_t snd = 0;
    ASSERT_EQ_INT(VM_MBOX_OK, vm_mailbox_recv(&m, payload, &snd));
    ASSERT_EQ_INT(2, snd);
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_mailbox");

    /* Init and basic state */
    RUN(test_init_basic);
    RUN(test_init_rejects_null_handle);
    RUN(test_init_rejects_null_storage);
    RUN(test_init_rejects_zero_slot_size);
    RUN(test_init_rejects_zero_depth);
    RUN(test_init_rejects_oversized_slot);
    RUN(test_required_storage_helper);

    /* Whitelist */
    RUN(test_whitelist_default_empty);
    RUN(test_whitelist_set_and_check);
    RUN(test_whitelist_clear);
    RUN(test_whitelist_set_all_and_clear_all);
    RUN(test_whitelist_rejects_out_of_range_id);
    RUN(test_whitelist_self_allowed);

    /* Send and recv happy paths */
    RUN(test_send_recv_round_trip);
    RUN(test_send_fifo_ordering);
    RUN(test_recv_with_null_sender_ok);

    /* Send error paths */
    RUN(test_send_rejected_not_whitelisted);
    RUN(test_send_rejected_full);
    RUN(test_send_rejected_wrong_size);
    RUN(test_send_rejected_bad_vm_id);
    RUN(test_send_rejected_null_payload);

    /* Recv error paths */
    RUN(test_recv_empty);
    RUN(test_recv_rejects_null_payload);

    /* Peek */
    RUN(test_peek_does_not_consume);
    RUN(test_peek_empty);
    RUN(test_peek_with_null_buffers);

    /* Reset */
    RUN(test_reset_drops_messages);
    RUN(test_reset_preserves_whitelist);

    /* Round-trip / coverage */
    RUN(test_sender_id_round_trip);
    RUN(test_small_payload_one_byte);
    RUN(test_large_payload_256_bytes);
    RUN(test_send_after_pop_cycles);

    /* Locker seam (API behavior; multithreaded proof in docs/reference/) */
    RUN(test_locker_invoked_balanced);
    RUN(test_locker_null_fallback);
    RUN(test_locker_default_is_noop);

    return TEST_SUITE_RESULT();
}
