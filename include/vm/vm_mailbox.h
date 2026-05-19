/* ============================================================
 *  vm_mailbox.h — per-VM message mailbox with sender whitelisting
 *
 *  One mailbox lives per VM. Senders deposit fixed-size payloads
 *  via vm_mailbox_send; the owning VM consumes them via
 *  vm_mailbox_recv. Reject-on-full: a slow consumer doesn't lose
 *  old messages, a fast producer is told to back off.
 *
 *  Each mailbox is gated by a whitelist owned by the receiver:
 *  the receiver explicitly permits senders by vm_id. Sends from
 *  non-whitelisted VMs are rejected as VM_EPERM at the entry to
 *  vm_mailbox_send. The receiver controls the policy, and the
 *  whitelist is the only access mechanism — there is no global
 *  override.
 *
 *  ---------------------------------------------------------------
 *  Slot layout
 *  ---------------------------------------------------------------
 *
 *  Each mailbox declares a payload size at init. The receiver
 *  sees that size as "the size of messages I will read"; the
 *  sender must match it exactly (caller's responsibility — the
 *  ECALL handler in vm_ecall.c enforces this against the syscall
 *  argument).
 *
 *  Internally, each FIFO slot holds the payload plus a 2-byte
 *  sender ID, packed as:
 *
 *      [ uint16_t sender_id ][ payload (slot_size bytes) ]
 *
 *  The internal slot size is therefore slot_size + 2 (rounded up
 *  to the underlying ring_buffer's alignment). This is hidden from
 *  callers — sends take a payload pointer, receives write the
 *  payload to the caller's buffer and return the sender_id
 *  separately.
 *
 *  Maximum payload size is VM_MAILBOX_MAX_SLOT_SIZE (default
 *  4096). Larger messages should be transferred via shared region
 *  with a small mailbox message carrying the shared offset.
 *
 *  ---------------------------------------------------------------
 *  Whitelist
 *  ---------------------------------------------------------------
 *
 *  Stored as a bitmap of allowed sender vm_ids, one bit per VM.
 *  Default size: 32 bits (so up to 32 VMs in the system). Adjust
 *  VM_MAILBOX_WHITELIST_BITS if you need more; choose 64 if you
 *  have 33–64 VMs. Beyond that, switch to a different
 *  representation (sorted array, bloom filter + linear fallback,
 *  hashtable) — the bitmap stops being the right structure once
 *  you have hundreds of VMs.
 *
 *  Bit 0 corresponds to vm_id 0, bit 1 to vm_id 1, etc. A VM is
 *  allowed to whitelist itself (self-messaging is a valid
 *  pattern: a VM can post to its own mailbox to defer work).
 *
 *  By default the whitelist starts empty (no senders allowed).
 *  The receiver explicitly opens itself up to senders it knows
 *  about — possibly all-of-them at startup if it's a "public
 *  service" VM, possibly a small named set if it's a private
 *  helper.
 *
 *  ---------------------------------------------------------------
 *  Threading
 *  ---------------------------------------------------------------
 *
 *  Single host thread by design (see vm_core.h). Only one VM is
 *  in a vm_step call at any moment, and only one ECALL handler
 *  runs at a time, so no two threads ever touch a mailbox
 *  concurrently. No locking inside this module.
 *
 *  If you ever drive multiple VMs on multiple host threads, every
 *  call into vm_mailbox needs external synchronization — and the
 *  whole VM model in vm_core.h needs revisiting first.
 *
 *  ---------------------------------------------------------------
 *  Depends on: containers/fifo_queue, containers/ring_buffer
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_MAILBOX_H
#define VM_MAILBOX_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "containers/fifo_queue.h"

/* ============================================================
 *  Tunables
 * ============================================================ */

/* Width of the whitelist bitmap. Must be 32 or 64. Caps the
 * maximum vm_id (and therefore the maximum number of VMs in the
 * system) at this value minus one. Default 32 matches a small
 * MCU's typical task count. */
#ifndef VM_MAILBOX_WHITELIST_BITS
#define VM_MAILBOX_WHITELIST_BITS  32
#endif

#if VM_MAILBOX_WHITELIST_BITS == 32
typedef uint32_t VmMailboxWhitelist;
#elif VM_MAILBOX_WHITELIST_BITS == 64
typedef uint64_t VmMailboxWhitelist;
#else
#error "VM_MAILBOX_WHITELIST_BITS must be 32 or 64"
#endif

/* Hard cap on per-mailbox payload size. Larger payloads should
 * be passed by reference through the shared region. */
#ifndef VM_MAILBOX_MAX_SLOT_SIZE
#define VM_MAILBOX_MAX_SLOT_SIZE  4096
#endif

/* Internal: number of bytes prefixed to each payload inside the
 * FIFO to carry the sender's vm_id. Exposed here only so the
 * caller can size the storage buffer correctly via
 * vm_mailbox_required_storage_bytes(). */
#define VM_MAILBOX_SLOT_OVERHEAD  2   /* sizeof(uint16_t) */

/* ============================================================
 *  Result codes
 *
 *  Maps cleanly onto VM_E* errno values from vm_ecall.h; the
 *  ECALL handlers convert these to negated errnos for the
 *  guest. We use a dedicated enum here so vm_mailbox is not a
 *  hard dependency of vm_ecall (the mailbox can be tested
 *  standalone without the ECALL machinery).
 * ============================================================ */

typedef enum {
    VM_MBOX_OK = 0,
    VM_MBOX_ERR_INVALID_ARG,    /* NULL pointer, bad slot size, etc. */
    VM_MBOX_ERR_NOT_WHITELISTED,/* sender vm_id not in receiver's whitelist */
    VM_MBOX_ERR_FULL,           /* mailbox at capacity, message rejected */
    VM_MBOX_ERR_EMPTY,          /* recv on empty mailbox */
    VM_MBOX_ERR_BAD_VM_ID,      /* vm_id >= VM_MAILBOX_WHITELIST_BITS */
} VmMailboxResult;

/* ============================================================
 *  VmMailbox
 *
 *  Caller declares a VmMailbox per VM and provides the storage
 *  buffer for the underlying FIFO. The slot_size visible to
 *  callers is the payload size; the storage buffer must be sized
 *  to (slot_size + VM_MAILBOX_SLOT_OVERHEAD) * depth bytes.
 *  vm_mailbox_required_storage_bytes computes this for you.
 *
 *  Public read-only stat fields (drop counters, etc.) are visible
 *  directly. Internal fields are managed by the implementation.
 * ============================================================ */

typedef struct {
    /* === Public read-only stats === */
    uint32_t sends_accepted;        /* successful pushes */
    uint32_t sends_rejected_full;   /* pushes rejected, mailbox full */
    uint32_t sends_rejected_perm;   /* pushes rejected, not whitelisted */
    uint32_t recvs;                 /* successful pops */

    /* === Public read-only config (set at init) === */
    uint16_t slot_size;             /* payload bytes per message */
    uint16_t depth;                 /* total slot count */

    /* === Whitelist (read via the helpers below) === */
    VmMailboxWhitelist whitelist;

    /* === Internal — managed by the implementation === */
    FifoQueue _fifo;
} VmMailbox;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Compute the storage size needed for a mailbox of the given
 * shape. Returns 0 if slot_size or depth are zero/too large. */
static inline size_t vm_mailbox_required_storage_bytes(uint16_t slot_size,
                                                       uint16_t depth) {
    if (slot_size == 0 || slot_size > VM_MAILBOX_MAX_SLOT_SIZE) return 0;
    if (depth == 0) return 0;
    return ((size_t)slot_size + VM_MAILBOX_SLOT_OVERHEAD) * (size_t)depth;
}

/* Initialize a mailbox over caller-provided storage.
 *
 *   m:           mailbox handle (caller-owned)
 *   storage:     pointer to a buffer of at least
 *                vm_mailbox_required_storage_bytes(slot_size, depth)
 *                bytes; must outlive m
 *   slot_size:   payload size in bytes, 1..VM_MAILBOX_MAX_SLOT_SIZE
 *   depth:       number of slots, > 0
 *
 * Starts with an empty whitelist (no senders allowed). The
 * receiver must explicitly call vm_mailbox_whitelist_set for any
 * sender it wants to receive from.
 *
 * Returns VM_MBOX_OK on success. */
VmMailboxResult vm_mailbox_init(VmMailbox *m,
                                void *storage,
                                uint16_t slot_size,
                                uint16_t depth);

/* Reset to empty (drops all queued messages) without touching the
 * whitelist or storage. */
void vm_mailbox_reset(VmMailbox *m);

/* ============================================================
 *  Whitelist
 *
 *  The whitelist is owned by the receiving mailbox. The receiver
 *  decides which senders may post.
 * ============================================================ */

/* Allow the given vm_id to send to this mailbox. */
VmMailboxResult vm_mailbox_whitelist_set(VmMailbox *m, uint16_t sender_vm_id);

/* Revoke the given vm_id's permission to send to this mailbox.
 * Messages already in the queue from this sender are NOT removed
 * — the whitelist check happens at send time only. */
VmMailboxResult vm_mailbox_whitelist_clear(VmMailbox *m, uint16_t sender_vm_id);

/* Query whether a sender is currently whitelisted. */
bool vm_mailbox_whitelist_check(const VmMailbox *m, uint16_t sender_vm_id);

/* Allow all valid sender vm_ids. Equivalent to setting every bit
 * up to VM_MAILBOX_WHITELIST_BITS. Useful for "public service"
 * VMs that accept messages from anyone. */
void vm_mailbox_whitelist_set_all(VmMailbox *m);

/* Deny all senders. The receiver becomes effectively unreachable
 * until it whitelists at least one sender again. */
void vm_mailbox_whitelist_clear_all(VmMailbox *m);

/* ============================================================
 *  Send and receive
 *
 *  These are pure host-side functions. The ECALL handlers in
 *  vm_ecall.c marshal the syscall arguments and call these.
 *
 *  vm_mailbox_send: deposits a message into the receiver's queue.
 *    Caller has already (a) translated the sender's payload from
 *    guest to host pointer and (b) validated that the size
 *    matches the receiver's slot_size. Send copies the payload
 *    into the FIFO; the caller's payload buffer may be reused
 *    immediately after return.
 *
 *  vm_mailbox_recv: pops the oldest message into the caller's
 *    buffer. Caller has already translated the destination from
 *    guest to host pointer. The buffer must have at least
 *    m->slot_size bytes available; on success exactly that many
 *    bytes are written. The sender's vm_id is returned via
 *    out_sender.
 *
 *  vm_mailbox_peek: same shape as recv but does not remove the
 *    message. Lets the receiver inspect the head before deciding
 *    to consume or defer. Does not support out-of-order extract;
 *    use multiple mailboxes per priority if you need that.
 * ============================================================ */

/* Send a message from sender_vm_id to this mailbox.
 *
 *   m:              target mailbox
 *   sender_vm_id:   sender's identity, used for whitelist check
 *                   and stored alongside the payload for recv
 *   payload:        pointer to the payload to copy in
 *   payload_size:   must equal m->slot_size
 *
 * Returns:
 *   VM_MBOX_OK
 *   VM_MBOX_ERR_INVALID_ARG    NULL pointer or size mismatch
 *   VM_MBOX_ERR_BAD_VM_ID      sender_vm_id out of whitelist range
 *   VM_MBOX_ERR_NOT_WHITELISTED sender not allowed by this mailbox
 *   VM_MBOX_ERR_FULL           mailbox at capacity */
VmMailboxResult vm_mailbox_send(VmMailbox *m,
                                uint16_t sender_vm_id,
                                const void *payload,
                                uint16_t payload_size);

/* Receive the oldest message from this mailbox.
 *
 *   m:           the mailbox to read
 *   out_payload: buffer to receive the payload; at least slot_size
 *                bytes; exactly slot_size bytes are written on
 *                success
 *   out_sender:  receives the sender's vm_id (may be NULL if the
 *                caller doesn't care, though that's an odd choice)
 *
 * Returns:
 *   VM_MBOX_OK
 *   VM_MBOX_ERR_INVALID_ARG    NULL m or out_payload
 *   VM_MBOX_ERR_EMPTY          no message available
 *
 * On VM_MBOX_ERR_EMPTY, *out_payload and *out_sender are not
 * modified. */
VmMailboxResult vm_mailbox_recv(VmMailbox *m,
                                void *out_payload,
                                uint16_t *out_sender);

/* Inspect the oldest message without removing it.
 *
 *   m:           the mailbox to peek
 *   out_payload: buffer to receive the payload; at least slot_size
 *                bytes; exactly slot_size bytes are written on
 *                success. May be NULL if the caller only wants to
 *                know the sender (in which case no payload bytes
 *                are written).
 *   out_sender:  receives the sender's vm_id. May be NULL.
 *
 * Returns:
 *   VM_MBOX_OK
 *   VM_MBOX_ERR_INVALID_ARG    NULL m
 *   VM_MBOX_ERR_EMPTY          no message available
 *
 * The mailbox is not modified. A subsequent vm_mailbox_recv will
 * return exactly this message (assuming no intervening send/recv).
 *
 * Useful for:
 *   - Deciding whether to consume the head message based on its
 *     sender (e.g., "if it's from VM #3, handle it; otherwise
 *     defer to the next quantum")
 *   - Coalescing: peek, decide it's stale, recv-and-discard, peek
 *     the next one, etc.
 *
 * Not useful for selective extraction by sender — peek only
 * inspects the head, and there is no extract-by-sender operation.
 * If you need that pattern, structure your VMs with multiple
 * mailboxes per priority/source class. */
VmMailboxResult vm_mailbox_peek(VmMailbox *m,
                                void *out_payload,
                                uint16_t *out_sender);

/* ============================================================
 *  Introspection
 * ============================================================ */

/* Number of messages currently queued. */
static inline size_t vm_mailbox_count(const VmMailbox *m) {
    return m ? fifo_count(&m->_fifo) : 0;
}

/* Free slots available for new messages. */
static inline size_t vm_mailbox_free_slots(const VmMailbox *m) {
    return m ? ((size_t)m->depth - fifo_count(&m->_fifo)) : 0;
}

/* Total slot capacity (same as m->depth). */
static inline size_t vm_mailbox_capacity(const VmMailbox *m) {
    return m ? m->depth : 0;
}

static inline bool vm_mailbox_empty(const VmMailbox *m) {
    return m ? fifo_empty(&m->_fifo) : true;
}

static inline bool vm_mailbox_full(const VmMailbox *m) {
    return m ? fifo_full(&m->_fifo) : false;
}

#endif /* VM_MAILBOX_H */
