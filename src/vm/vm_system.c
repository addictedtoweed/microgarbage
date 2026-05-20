/* ============================================================
 *  vm_system.c — top-level VM system implementation
 *  See vm/vm_system.h for the public contract.
 *
 *  This file wires up the scheduler + ECALL router + slab + bump
 *  arena + per-VM mailboxes, and provides the ECALL handlers that
 *  need access to all of those (alloc/free, mailbox send/recv,
 *  whitelist management, mailbox info).
 *
 *  Why these handlers live here instead of vm_ecall_handlers.c:
 *  they reach into the VmSystem struct via the 'system' pointer.
 *  Keeping them with the system struct avoids a circular include.
 *  The cpu-only handlers (EXIT, SELF, YIELD, CRITICAL_*) stay in
 *  vm_ecall_handlers.c since they only touch the VmCpu.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_system.h"
#include "memory/slab_stack.h"

#include <string.h>

/* ============================================================
 *  Forward declaration: the CPU-only installer lives in
 *  vm_ecall_handlers.c. Declared here so we can call it during
 *  init.
 * ============================================================ */
bool vm_ecall_install_cpu_handlers(VmEcallRouter *r);

/* ============================================================
 *  Defaults
 *
 *  Constants for fields the caller leaves at zero in the config.
 * ============================================================ */

#define DEFAULT_BASELINE_QUANTUM      5000u
#define DEFAULT_MAX_CRITICAL_OVERRUN  50000u
#define DEFAULT_MAILBOX_SLOT_SIZE     32u
#define DEFAULT_MAILBOX_DEPTH         8u

/* A balanced slab default: bins from 32B to 1MB. We give modest
 * counts to the small sizes (where most allocations live in our
 * imagined workloads) and small counts to the big ones.
 *
 * These defaults are sized to fit in roughly 16 KB so a 32-64 KB
 * shared region has reasonable headroom. Tune via cfg.slab_config
 * for production use.
 *
 * Memory rough-estimate per bin (size * count + bitmap overhead):
 *   32B × 16 = 512 + small overhead
 *   64B × 16 = 1KB
 *  128B × 8  = 1KB
 *  256B × 8  = 2KB
 *  512B × 4  = 2KB
 *   1KB × 2  = 2KB
 *   2KB × 1  = 2KB
 *   4KB × 1  = 4KB
 *   ...      0 (no headroom for those by default)
 *  Total: ~15 KB plus per-block headers.
 */
static const uint16_t DEFAULT_SLAB_BUCKETS[SLAB_BIN_COUNT] = {
    /*   32 */ 16,
    /*   64 */ 16,
    /*  128 */  8,
    /*  256 */  8,
    /*  512 */  4,
    /* 1KB  */  2,
    /* 2KB  */  1,
    /* 4KB  */  1,
    /* 8KB  */  0,
    /* 16KB */  0,
    /* 32KB */  0,
    /* 64KB */  0,
    /* 128KB */ 0,
    /* 256KB */ 0,
    /* 512KB */ 0,
    /* 1MB  */  0,
};

/* ============================================================
 *  Address helpers
 *
 *  Translate between guest shared-region addresses and host
 *  pointers into the slab. The shared region's guest base is
 *  0xC0000000.
 * ============================================================ */

#define SHARED_GUEST_BASE  0xC0000000u

static inline bool addr_in_shared(uint32_t guest_addr) {
    return (guest_addr & 0xC0000000u) == 0xC0000000u;
}

/* ============================================================
 *  ECALL handlers — system context
 *
 *  Each receives (VmCpu *cpu, void *system) where 'system' is
 *  the VmSystem* set during vm_system_init.
 * ============================================================ */

/* SYS_ALLOC (a7 = 1056)
 *   a0 = size
 *   → a0 = guest pointer in shared region, or -ENOMEM / -EINVAL */
static void handle_alloc(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys) return;

    uint32_t size = cpu->regs[VM_REG_A0];
    if (size == 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }

    void *host_p = slab_alloc(sys->shared_slab, size);
    if (!host_p) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_ENOMEM);
        return;
    }

    /* Compute the guest address: shared region base is the host
     * pointer minus shared_storage, offset into the shared region's
     * 30-bit address space. */
    uintptr_t base = (uintptr_t)sys->config.shared_storage;
    uintptr_t ptr  = (uintptr_t)host_p;
    if (ptr < base || ptr - base >= sys->config.shared_storage_size) {
        /* Shouldn't happen — the slab is bounded by the region. */
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_ENOMEM);
        return;
    }
    uint32_t offset = (uint32_t)(ptr - base);
    cpu->regs[VM_REG_A0] = SHARED_GUEST_BASE + offset;
}

/* SYS_FREE (a7 = 1057)
 *   a0 = guest address
 *   → a0 = 0 on success, -EINVAL otherwise */
static void handle_free(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys) return;

    uint32_t guest_addr = cpu->regs[VM_REG_A0];
    if (!addr_in_shared(guest_addr)) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }

    uint32_t offset = guest_addr - SHARED_GUEST_BASE;
    if (offset >= sys->config.shared_storage_size) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }

    void *host_p = (uint8_t *)sys->config.shared_storage + offset;
    SlabResult r = slab_free(sys->shared_slab, host_p);
    if (r != SLAB_OK) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_SEND (a7 = 1072)
 *   a0 = target vm_id
 *   a1 = payload guest address
 *   a2 = payload size (must equal target's slot_size)
 *   → a0 = 0 on success, negative errno on failure
 *
 * Synchronous-delivery optimization: if the target is currently
 * blocked on BLOCK_MAILBOX_RECV, we copy the payload directly
 * into the target's saved dest pointer (recorded by handle_recv
 * in _internal[0]), set the target's a0 to this sender's vm_id,
 * and unblock. The message never enters the queue. This both
 * delivers the payload correctly AND avoids one queue insert/pop. */
static void handle_send(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys) return;

    uint32_t target_id = cpu->regs[VM_REG_A0];
    uint32_t guest_payload = cpu->regs[VM_REG_A1];
    uint32_t payload_size = cpu->regs[VM_REG_A2];

    if (target_id >= VM_SCHED_MAX_VMS) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_ENOENT);
        return;
    }

    VmMailbox *target_mbox = vm_system_get_mailbox(sys, (uint16_t)target_id);
    VmCpu *target_cpu = sys->vms[target_id];
    if (!target_mbox || !target_cpu) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_ENOENT);
        return;
    }

    if (payload_size != target_mbox->slot_size) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }

    /* Resolve the payload pointer via the sender's region map. */
    const void *host_payload = vm_translate_read(cpu, guest_payload,
                                                  payload_size);
    if (!host_payload) {
        /* vm_translate_read sets trap_cause; we override to -EFAULT
         * since this is an ECALL handler error, not a guest-fault. */
        cpu->trap_cause = TRAP_NONE;
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EFAULT);
        return;
    }

    /* Whitelist check: do this independent of blocked-or-not, so
     * sending to a blocked-but-not-whitelisted target still
     * returns -EPERM. The mailbox's own check covers it but we
     * also need it for the synchronous-delivery path which
     * bypasses vm_mailbox_send. */
    if (!vm_mailbox_whitelist_check(target_mbox, cpu->vm_id)) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EPERM);
        return;
    }

    /* Synchronous-delivery fast path: target is blocked on RECV.
     * Resolve their saved dest pointer through their region map,
     * copy the payload directly into it, set their a0 to our
     * vm_id, and unblock them. */
    if (target_cpu->block_reason == BLOCK_MAILBOX_RECV) {
        uint32_t target_dest = target_cpu->_internal[0];
        void *target_host = vm_translate_write(target_cpu,
                                                target_dest,
                                                payload_size);
        /* If target's dest pointer somehow became invalid (shouldn't
         * happen — region descriptors don't change while blocked),
         * fall through to queueing. */
        if (target_host) {
            memcpy(target_host, host_payload, payload_size);
            /* Clear trap state that vm_translate_write may have
             * set on a successful call (it doesn't, but defensively). */
            target_cpu->trap_cause = TRAP_NONE;
            /* Unblock with the sender id as their a0 result. */
            vm_sched_wake_mailbox(sys->sched, (uint16_t)target_id,
                                  (int32_t)cpu->vm_id);
            /* Clear the saved dest so it's not stale. */
            target_cpu->_internal[0] = 0;

            /* This send's result is success. */
            cpu->regs[VM_REG_A0] = 0;
            /* Bump the mailbox's accepted counter even though we
             * bypassed it — keeps stats truthful. */
            target_mbox->sends_accepted++;
            return;
        }
    }

    /* Normal path: queue the message. */
    VmMailboxResult r = vm_mailbox_send(target_mbox,
                                         cpu->vm_id,
                                         host_payload,
                                         (uint16_t)payload_size);
    switch (r) {
    case VM_MBOX_OK:
        cpu->regs[VM_REG_A0] = 0;
        break;
    case VM_MBOX_ERR_NOT_WHITELISTED:
    case VM_MBOX_ERR_BAD_VM_ID:
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EPERM);
        break;
    case VM_MBOX_ERR_FULL:
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EAGAIN);
        break;
    case VM_MBOX_ERR_INVALID_ARG:
    default:
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        break;
    }
}

/* SYS_RECV (a7 = 1073)
 *   a0 = destination guest address
 *   a1 = timeout in step-quanta (0 = poll, UINT32_MAX = forever)
 *   → a0 = sender vm_id, or -EAGAIN / -ETIMEDOUT / -EFAULT / -EBUSY
 *
 * For timeout=0, attempt one recv. If empty, return -EAGAIN.
 * For timeout > 0, if empty: save the guest dest pointer in
 * cpu->_internal[0] and set block_reason = BLOCK_MAILBOX_RECV.
 * The next SYS_SEND targeting this VM will see the block, deliver
 * the payload directly into the saved dest_ptr, set this VM's a0
 * to the sender id, and unblock.
 *
 * On timeout (handled by the scheduler's wake_expired_timeouts),
 * a0 is set to -ETIMEDOUT and the VM is unblocked. */
static void handle_recv(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys) return;

    uint32_t guest_dest = cpu->regs[VM_REG_A0];
    uint32_t timeout = cpu->regs[VM_REG_A1];

    VmMailbox *my_mbox = vm_system_get_mailbox(sys, cpu->vm_id);
    if (!my_mbox) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }

    /* Verify the destination pointer is valid and writable in
     * this VM's address space. */
    void *host_dest = vm_translate_write(cpu, guest_dest,
                                          my_mbox->slot_size);
    if (!host_dest) {
        cpu->trap_cause = TRAP_NONE;
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EFAULT);
        return;
    }

    /* Try to receive immediately. */
    uint16_t sender = 0;
    VmMailboxResult r = vm_mailbox_recv(my_mbox, host_dest, &sender);
    if (r == VM_MBOX_OK) {
        cpu->regs[VM_REG_A0] = (uint32_t)sender;
        return;
    }
    if (r != VM_MBOX_ERR_EMPTY) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }

    /* Mailbox empty. */
    if (timeout == 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EAGAIN);
        return;
    }
    if (cpu->in_critical) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBUSY);
        return;
    }

    /* Block. Save the guest dest pointer in _internal[0] so the
     * next SYS_SEND can deliver synchronously into it. _internal
     * is opaque to user code; the system layer owns its meaning. */
    cpu->_internal[0] = guest_dest;
    cpu->block_reason = BLOCK_MAILBOX_RECV;
    if (timeout == UINT32_MAX) {
        cpu->block_deadline = 0;   /* wait forever */
    } else {
        cpu->block_deadline = sys->sched->global_tick + timeout;
    }
    /* a0 will be written when we resume (by send or by timeout). */
}

/* SYS_MAILBOX_INFO (a7 = 1074)
 *   a0 = target vm_id
 *   → a0 = slot_size on success, -ENOENT/-EPERM on failure
 *   → a1 = free slots on success */
static void handle_mailbox_info(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys) return;

    uint32_t target_id = cpu->regs[VM_REG_A0];
    if (target_id >= VM_SCHED_MAX_VMS) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_ENOENT);
        return;
    }

    VmMailbox *target_mbox = vm_system_get_mailbox(sys, (uint16_t)target_id);
    if (!target_mbox) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_ENOENT);
        return;
    }

    /* Check whether caller is whitelisted by target. */
    if (!vm_mailbox_whitelist_check(target_mbox, cpu->vm_id)) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EPERM);
        return;
    }

    cpu->regs[VM_REG_A0] = target_mbox->slot_size;
    cpu->regs[VM_REG_A1] = (uint32_t)vm_mailbox_free_slots(target_mbox);
}

/* SYS_WHITELIST_ADD (a7 = 1075)
 *   a0 = sender vm_id to allow
 *   → a0 = 0 on success, -ENOENT on out-of-range */
static void handle_whitelist_add(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys) return;

    uint32_t sender_id = cpu->regs[VM_REG_A0];
    VmMailbox *my_mbox = vm_system_get_mailbox(sys, cpu->vm_id);
    if (!my_mbox) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }
    VmMailboxResult r = vm_mailbox_whitelist_set(my_mbox,
                                                  (uint16_t)sender_id);
    cpu->regs[VM_REG_A0] = (r == VM_MBOX_OK)
                         ? 0
                         : (uint32_t)-((int32_t)VM_ENOENT);
}

/* SYS_WHITELIST_REMOVE (a7 = 1076)
 *   a0 = sender vm_id to revoke
 *   → a0 = 0 on success, -ENOENT on out-of-range */
static void handle_whitelist_remove(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys) return;

    uint32_t sender_id = cpu->regs[VM_REG_A0];
    VmMailbox *my_mbox = vm_system_get_mailbox(sys, cpu->vm_id);
    if (!my_mbox) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }
    VmMailboxResult r = vm_mailbox_whitelist_clear(my_mbox,
                                                    (uint16_t)sender_id);
    cpu->regs[VM_REG_A0] = (r == VM_MBOX_OK)
                         ? 0
                         : (uint32_t)-((int32_t)VM_ENOENT);
}

/* ============================================================
 *  Timer / clock handlers
 *
 *  All four read from the scheduler's global_tick (which the
 *  host's tick_source callback owns — see VmSystemConfig).
 *  SLEEP variants set BLOCK_SLEEP + block_deadline; the
 *  scheduler's wake_expired_timeouts handles the rest.
 * ============================================================ */

/* SYS_TICKS_NOW (1043)
 *   () → a0 = current global_tick */
static void handle_ticks_now(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys || !sys->sched) return;
    cpu->regs[VM_REG_A0] = sys->sched->global_tick;
}

/* SYS_TICK_HZ (1044)
 *   () → a0 = configured ticks_per_second (0 if no real-time
 *             tick_source was provided) */
static void handle_tick_hz(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys || !sys->sched) return;
    cpu->regs[VM_REG_A0] = sys->sched->config.ticks_per_second;
}

/* SYS_SLEEP_TICKS (1045)
 *   a0 = n        sleep for n ticks
 *   → on wake: a0 = 0
 *
 * n == 0 is equivalent to SYS_YIELD (give someone else a turn,
 * resume on the next cycle). */
static void handle_sleep_ticks(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys || !sys->sched) return;

    uint32_t n = cpu->regs[VM_REG_A0];

    if (n == 0) {
        cpu->regs[VM_REG_A0] = 0;
        cpu->block_reason = BLOCK_YIELDED;
        return;
    }

    cpu->block_reason = BLOCK_SLEEP;
    cpu->block_deadline = sys->sched->global_tick + n;
    /* a0 will be set to 0 by the scheduler on wake. */
}

/* SYS_SLEEP_UNTIL (1046)
 *   a0 = deadline       block until global_tick >= deadline
 *   → on wake: a0 = 0
 *
 * If deadline is already in the past (within 2^31 ticks), wake
 * on the next scheduler pass — same as YIELD. Wraparound-safe:
 * uses signed-subtract comparison. */
static void handle_sleep_until(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys || !sys->sched) return;

    uint32_t deadline = cpu->regs[VM_REG_A0];
    uint32_t now      = sys->sched->global_tick;

    if ((int32_t)(now - deadline) >= 0) {
        cpu->regs[VM_REG_A0] = 0;
        cpu->block_reason = BLOCK_YIELDED;
        return;
    }

    cpu->block_reason = BLOCK_SLEEP;
    cpu->block_deadline = deadline;
}

/* ============================================================
 *  Install all the system-context handlers.
 *
 *  Best-effort all-or-nothing: rolls back on the first failure.
 * ============================================================ */

static bool install_system_handlers(VmEcallRouter *r) {
    struct { uint32_t num; VmEcallHandler h; } entries[] = {
        { SYS_ALLOC,            handle_alloc           },
        { SYS_FREE,             handle_free            },
        { SYS_SEND,             handle_send            },
        { SYS_RECV,             handle_recv            },
        { SYS_MAILBOX_INFO,     handle_mailbox_info    },
        { SYS_WHITELIST_ADD,    handle_whitelist_add   },
        { SYS_WHITELIST_REMOVE, handle_whitelist_remove},
        { SYS_TICKS_NOW,        handle_ticks_now       },
        { SYS_TICK_HZ,          handle_tick_hz         },
        { SYS_SLEEP_TICKS,      handle_sleep_ticks     },
        { SYS_SLEEP_UNTIL,      handle_sleep_until     },
    };
    size_t n = sizeof(entries) / sizeof(entries[0]);

    for (size_t i = 0; i < n; i++) {
        if (!vm_ecall_register(r, entries[i].num, entries[i].h)) {
            for (size_t j = 0; j < i; j++) {
                vm_ecall_unregister(r, entries[j].num);
            }
            return false;
        }
    }
    return true;
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

bool vm_system_init(VmSystem *sys, const VmSystemConfig *cfg) {
    if (!sys || !cfg) return false;
    if (!cfg->shared_storage || cfg->shared_storage_size == 0) return false;
    if (!cfg->local_storage || cfg->local_storage_size == 0) return false;

    memset(sys, 0, sizeof(*sys));

    /* Copy and fill in defaults. */
    sys->config = *cfg;
    if (sys->config.baseline_quantum == 0)
        sys->config.baseline_quantum = DEFAULT_BASELINE_QUANTUM;
    if (sys->config.max_critical_overrun == 0)
        sys->config.max_critical_overrun = DEFAULT_MAX_CRITICAL_OVERRUN;
    if (sys->config.default_mailbox_slot_size == 0)
        sys->config.default_mailbox_slot_size = DEFAULT_MAILBOX_SLOT_SIZE;
    if (sys->config.default_mailbox_depth == 0)
        sys->config.default_mailbox_depth = DEFAULT_MAILBOX_DEPTH;

    /* Slab config: zero-init means "all bucket_counts == 0", which
     * is unusable. Fill in defaults. */
    bool any_bucket = false;
    for (int i = 0; i < SLAB_BIN_COUNT; i++) {
        if (sys->config.slab_config.bucket_counts[i] != 0) {
            any_bucket = true;
            break;
        }
    }
    if (!any_bucket) {
        for (int i = 0; i < SLAB_BIN_COUNT; i++) {
            sys->config.slab_config.bucket_counts[i] =
                DEFAULT_SLAB_BUCKETS[i];
        }
    }

    /* Wire public pointers to the internal instances. */
    sys->ecall_router = &sys->_ecall_router;
    sys->sched        = &sys->_sched;
    sys->shared_slab  = &sys->_shared_slab;
    sys->local_arena  = &sys->_local_arena;

    /* 1. Initialize the slab. */
    SlabResult sr = slab_init(sys->shared_slab,
                               sys->config.shared_storage,
                               sys->config.shared_storage_size,
                               &sys->config.slab_config,
                               slab_null_locker);
    if (sr != SLAB_OK) {
        return false;
    }

    /* 2. Initialize the bump arena over local storage. */
    bump_init(sys->local_arena,
              sys->config.local_storage,
              sys->config.local_storage_size);

    /* 3. Initialize the ECALL router and install standard handlers. */
    vm_ecall_router_init(sys->ecall_router);
    if (!vm_ecall_install_cpu_handlers(sys->ecall_router)) {
        return false;
    }
    if (!install_system_handlers(sys->ecall_router)) {
        return false;
    }

    /* 4. Initialize the scheduler. */
    VmSchedConfig sched_cfg = {
        .baseline_quantum     = sys->config.baseline_quantum,
        .max_critical_overrun = sys->config.max_critical_overrun,
        .trap_handler         = sys->config.trap_handler,
        .idle_handler         = sys->config.idle_handler,
        .ecall_router         = sys->ecall_router,
        .system               = sys,   /* what the handlers receive */
        .tick_source          = sys->config.tick_source,
        .tick_source_userdata = sys->config.tick_source_userdata,
        .ticks_per_second     = sys->config.ticks_per_second,
    };
    vm_sched_init(sys->sched, &sched_cfg);

    return true;
}

void vm_system_destroy(VmSystem *sys) {
    if (!sys) return;
    /* Halt any remaining VMs. */
    for (uint16_t i = 0; i < VM_SCHED_MAX_VMS; i++) {
        if (sys->vms[i]) {
            sys->vms[i]->halted = true;
        }
    }
    /* Don't free caller-owned storage; just clear our state. */
    slab_destroy(sys->shared_slab);
    memset(sys, 0, sizeof(*sys));
}

/* ============================================================
 *  VM loading
 * ============================================================ */

VmLoadVmResult vm_system_load_vm_with_mailbox(VmSystem *sys,
                                               const void *elf_image,
                                               size_t elf_size,
                                               uint32_t data_region_size,
                                               VmBacking code_backing,
                                               VmBacking rodata_backing,
                                               uint16_t mailbox_slot_size,
                                               uint16_t mailbox_depth) {
    VmLoadVmResult result = {
        .code = VM_SYS_OK,
        .load_result = VM_LOAD_OK,
        .assigned_vm_id = -1,
    };
    if (!sys || !elf_image || elf_size == 0 || data_region_size == 0) {
        result.code = VM_SYS_ERR_INVALID_ARG;
        return result;
    }

    /* 1. Allocate the VmCpu struct from the bump arena. */
    VmCpu *cpu = (VmCpu *)bump_alloc(sys->local_arena, sizeof(VmCpu));
    if (!cpu) {
        result.code = VM_SYS_ERR_NO_ARENA_SPACE;
        return result;
    }

    /* 2. Find the slot we'll get, so we can stash a pointer.
     *    The scheduler picks the actual id later. We don't have
     *    a "peek next free id" API, so register first and patch
     *    the slot afterward. But registering needs the cpu init'd,
     *    so init first. We use a tentative vm_id of 0; the
     *    scheduler will overwrite it. */
    vm_init(cpu, 0);

    /* 3. Allocate mailbox storage and initialize the mailbox at
     *    a known slot in sys->mailboxes[]. We need the vm_id for
     *    indexing, so we register with the scheduler first to get
     *    the id assigned, then set up the mailbox at that index. */
    int assigned = vm_sched_register(sys->sched, cpu);
    if (assigned < 0) {
        result.code = VM_SYS_ERR_FULL;
        return result;
    }
    sys->vms[assigned] = cpu;

    /* Mailbox storage. */
    size_t mbox_storage_bytes = vm_mailbox_required_storage_bytes(
        mailbox_slot_size, mailbox_depth);
    if (mbox_storage_bytes == 0) {
        /* Invalid mailbox shape — undo the registration. */
        vm_sched_unregister(sys->sched, (uint16_t)assigned);
        sys->vms[assigned] = NULL;
        result.code = VM_SYS_ERR_INVALID_ARG;
        return result;
    }
    void *mbox_storage = bump_alloc(sys->local_arena, mbox_storage_bytes);
    if (!mbox_storage) {
        vm_sched_unregister(sys->sched, (uint16_t)assigned);
        sys->vms[assigned] = NULL;
        result.code = VM_SYS_ERR_NO_ARENA_SPACE;
        return result;
    }
    VmMailboxResult mr = vm_mailbox_init(&sys->mailboxes[assigned],
                                          mbox_storage,
                                          mailbox_slot_size,
                                          mailbox_depth);
    if (mr != VM_MBOX_OK) {
        vm_sched_unregister(sys->sched, (uint16_t)assigned);
        sys->vms[assigned] = NULL;
        result.code = VM_SYS_ERR_INVALID_ARG;
        return result;
    }

    /* 4. Load the ELF. */
    VmLoaderConfig loader_cfg = {
        .code_backing      = code_backing,
        .rodata_backing    = rodata_backing,
        .ram_arena         = sys->local_arena,
        .region_data_size  = data_region_size,
        .shared_base       = sys->config.shared_storage,
        .shared_size       = (uint32_t)sys->config.shared_storage_size,
    };
    VmLoadResult lr = vm_load(cpu, elf_image, elf_size, &loader_cfg);
    if (lr != VM_LOAD_OK) {
        vm_sched_unregister(sys->sched, (uint16_t)assigned);
        sys->vms[assigned] = NULL;
        result.code = VM_SYS_ERR_LOAD_FAILED;
        result.load_result = lr;
        return result;
    }

    result.assigned_vm_id = assigned;
    return result;
}

VmLoadVmResult vm_system_load_vm(VmSystem *sys,
                                  const void *elf_image, size_t elf_size,
                                  uint32_t data_region_size,
                                  VmBacking code_backing,
                                  VmBacking rodata_backing) {
    return vm_system_load_vm_with_mailbox(
        sys, elf_image, elf_size, data_region_size,
        code_backing, rodata_backing,
        sys->config.default_mailbox_slot_size,
        sys->config.default_mailbox_depth);
}

VmMailbox *vm_system_get_mailbox(VmSystem *sys, uint16_t vm_id) {
    if (!sys) return NULL;
    if (vm_id >= VM_SCHED_MAX_VMS) return NULL;
    if (sys->vms[vm_id] == NULL) return NULL;
    return &sys->mailboxes[vm_id];
}

/* ============================================================
 *  Execution — thin pass-through to the scheduler
 * ============================================================ */

bool vm_system_run(VmSystem *sys, uint64_t max_cycles) {
    if (!sys) return false;
    return vm_sched_run(sys->sched, max_cycles);
}

VmSchedStepResult vm_system_step(VmSystem *sys) {
    if (!sys) return VM_SCHED_ALL_HALTED;
    return vm_sched_step(sys->sched);
}
