/* ============================================================
 *  vm_mailbox.c — implementation
 *  See vm/vm_mailbox.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_mailbox.h"
#include <string.h>

/* ============================================================
 *  Internal slot layout
 *
 *  Each FIFO slot is wider than the user-visible payload by
 *  VM_MAILBOX_SLOT_OVERHEAD bytes (currently 2 for sender_id):
 *
 *      [ uint16_t sender_id ][ payload (slot_size bytes) ]
 *
 *  Total internal slot size: slot_size + 2. The FIFO is initialized
 *  with this internal size; vm_mailbox_send composes a slot from
 *  (sender_id, payload) before push, and vm_mailbox_recv splits a
 *  slot back into (out_payload, out_sender) after pop.
 *
 *  We use a stack-allocated scratch buffer for the slot composition
 *  rather than a flexible-array struct so the size can vary at
 *  runtime. The buffer is sized to the maximum payload; mailboxes
 *  with smaller slots only use the first slot_size + 2 bytes.
 * ============================================================ */

#define INTERNAL_SLOT_BYTES(payload_bytes) \
    ((payload_bytes) + VM_MAILBOX_SLOT_OVERHEAD)

/* ============================================================
 *  Whitelist helpers
 *
 *  Bit N corresponds to vm_id N. The single bitmap word width is
 *  VM_MAILBOX_WHITELIST_BITS (32 or 64); we use the
 *  VmMailboxWhitelist typedef from the header for portability.
 * ============================================================ */

static inline bool whitelist_id_valid(uint16_t vm_id) {
    return vm_id < VM_MAILBOX_WHITELIST_BITS;
}

static inline VmMailboxWhitelist whitelist_bit(uint16_t vm_id) {
    /* Caller must have validated vm_id; shift by >= width is UB. */
    return ((VmMailboxWhitelist)1) << vm_id;
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

VmMailboxResult vm_mailbox_init(VmMailbox *m,
                                void *storage,
                                uint16_t slot_size,
                                uint16_t depth) {
    if (!m || !storage) {
        return VM_MBOX_ERR_INVALID_ARG;
    }
    if (slot_size == 0 || slot_size > VM_MAILBOX_MAX_SLOT_SIZE) {
        return VM_MBOX_ERR_INVALID_ARG;
    }
    if (depth == 0) {
        return VM_MBOX_ERR_INVALID_ARG;
    }

    /* Zero out stats and config first; the FIFO init follows. */
    memset(m, 0, sizeof(*m));
    m->slot_size = slot_size;
    m->depth = depth;
    m->whitelist = 0;   /* empty: no senders allowed by default */

    /* The internal slot is wider than the user-visible payload. */
    fifo_init(&m->_fifo, storage,
              (size_t)depth,
              INTERNAL_SLOT_BYTES((size_t)slot_size));

    return VM_MBOX_OK;
}

void vm_mailbox_reset(VmMailbox *m) {
    if (!m) return;
    fifo_reset(&m->_fifo);
    /* Stats and whitelist preserved by design — reset drops
     * messages but doesn't undo configuration. */
}

/* ============================================================
 *  Whitelist management
 * ============================================================ */

VmMailboxResult vm_mailbox_whitelist_set(VmMailbox *m, uint16_t sender_vm_id) {
    if (!m) return VM_MBOX_ERR_INVALID_ARG;
    if (!whitelist_id_valid(sender_vm_id)) return VM_MBOX_ERR_BAD_VM_ID;
    m->whitelist |= whitelist_bit(sender_vm_id);
    return VM_MBOX_OK;
}

VmMailboxResult vm_mailbox_whitelist_clear(VmMailbox *m, uint16_t sender_vm_id) {
    if (!m) return VM_MBOX_ERR_INVALID_ARG;
    if (!whitelist_id_valid(sender_vm_id)) return VM_MBOX_ERR_BAD_VM_ID;
    m->whitelist &= ~whitelist_bit(sender_vm_id);
    return VM_MBOX_OK;
}

bool vm_mailbox_whitelist_check(const VmMailbox *m, uint16_t sender_vm_id) {
    if (!m) return false;
    if (!whitelist_id_valid(sender_vm_id)) return false;
    return (m->whitelist & whitelist_bit(sender_vm_id)) != 0;
}

void vm_mailbox_whitelist_set_all(VmMailbox *m) {
    if (!m) return;
    /* All bits set. For 32-bit: 0xFFFFFFFF. For 64-bit: 0xFFFFFFFFFFFFFFFF.
     * Computed by negating zero in the typedef's width. */
    m->whitelist = (VmMailboxWhitelist)~(VmMailboxWhitelist)0;
}

void vm_mailbox_whitelist_clear_all(VmMailbox *m) {
    if (!m) return;
    m->whitelist = 0;
}

/* ============================================================
 *  Send
 *
 *  Slot composition order matters: we lay out the sender_id at
 *  the start of the scratch buffer, then the payload immediately
 *  after. The internal FIFO copies the entire internal_slot_bytes
 *  region as one block.
 *
 *  The scratch buffer is stack-allocated. Its size is bounded by
 *  VM_MAILBOX_MAX_SLOT_SIZE + VM_MAILBOX_SLOT_OVERHEAD, which is
 *  4098 bytes by default. This is large for a typical embedded
 *  stack frame; if that's a concern, reduce VM_MAILBOX_MAX_SLOT_SIZE
 *  at compile time to bound the stack usage. (For systems running
 *  on a host with megabytes of stack, the size is fine.)
 * ============================================================ */

VmMailboxResult vm_mailbox_send(VmMailbox *m,
                                uint16_t sender_vm_id,
                                const void *payload,
                                uint16_t payload_size) {
    if (!m || !payload) {
        return VM_MBOX_ERR_INVALID_ARG;
    }
    if (payload_size != m->slot_size) {
        return VM_MBOX_ERR_INVALID_ARG;
    }
    if (!whitelist_id_valid(sender_vm_id)) {
        return VM_MBOX_ERR_BAD_VM_ID;
    }
    if ((m->whitelist & whitelist_bit(sender_vm_id)) == 0) {
        m->sends_rejected_perm++;
        return VM_MBOX_ERR_NOT_WHITELISTED;
    }

    /* Compose the internal slot in a scratch buffer. */
    uint8_t scratch[VM_MAILBOX_MAX_SLOT_SIZE + VM_MAILBOX_SLOT_OVERHEAD];
    /* Layout: [sender_id (2B)][payload (payload_size B)] */
    memcpy(&scratch[0], &sender_vm_id, sizeof(uint16_t));
    memcpy(&scratch[VM_MAILBOX_SLOT_OVERHEAD], payload, payload_size);

    if (!fifo_push(&m->_fifo, scratch)) {
        m->sends_rejected_full++;
        return VM_MBOX_ERR_FULL;
    }

    m->sends_accepted++;
    return VM_MBOX_OK;
}

/* ============================================================
 *  Receive
 *
 *  Pops the oldest slot into a scratch buffer, then splits it
 *  into (payload, sender). Stats are bumped only on success.
 * ============================================================ */

VmMailboxResult vm_mailbox_recv(VmMailbox *m,
                                void *out_payload,
                                uint16_t *out_sender) {
    if (!m || !out_payload) {
        return VM_MBOX_ERR_INVALID_ARG;
    }

    uint8_t scratch[VM_MAILBOX_MAX_SLOT_SIZE + VM_MAILBOX_SLOT_OVERHEAD];
    if (!fifo_pop(&m->_fifo, scratch)) {
        return VM_MBOX_ERR_EMPTY;
    }

    /* Split: sender_id at offset 0, payload at offset 2. */
    if (out_sender) {
        memcpy(out_sender, &scratch[0], sizeof(uint16_t));
    }
    memcpy(out_payload, &scratch[VM_MAILBOX_SLOT_OVERHEAD], m->slot_size);

    m->recvs++;
    return VM_MBOX_OK;
}

/* ============================================================
 *  Peek
 *
 *  Same shape as recv, but does not consume the message and does
 *  not bump the recvs counter. The internal FIFO supports peek
 *  natively via fifo_peek.
 * ============================================================ */

VmMailboxResult vm_mailbox_peek(VmMailbox *m,
                                void *out_payload,
                                uint16_t *out_sender) {
    if (!m) {
        return VM_MBOX_ERR_INVALID_ARG;
    }

    uint8_t scratch[VM_MAILBOX_MAX_SLOT_SIZE + VM_MAILBOX_SLOT_OVERHEAD];
    if (!fifo_peek(&m->_fifo, scratch)) {
        return VM_MBOX_ERR_EMPTY;
    }

    if (out_sender) {
        memcpy(out_sender, &scratch[0], sizeof(uint16_t));
    }
    if (out_payload) {
        memcpy(out_payload, &scratch[VM_MAILBOX_SLOT_OVERHEAD], m->slot_size);
    }

    /* Do NOT bump m->recvs — peek is not a receive. */
    return VM_MBOX_OK;
}
