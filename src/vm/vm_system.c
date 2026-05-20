/* ============================================================
 *  vm_system.c — top-level VM system implementation
 *  See vm/vm_system.h for the public contract.
 *
 *  This file wires up the scheduler + ECALL router + slabs
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
#include <stdio.h>

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

/* SYS_SET_RELOAD_PERIOD (1047)
 *   a0 = period in ticks (0 = clear / disable)
 *   → a0 = 0
 *
 * Anchors the reload state so the FIRST subsequent
 * yield_until_reload sleeps for one full period. Subsequent
 * yields fire one period apart from the previous wake. Passing
 * period=0 clears the state; a guest can switch between flavors.
 *
 * Does not change block_reason — the guest stays runnable
 * after this call. The actual blocking happens in
 * yield_until_reload. */
static void handle_set_reload_period(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys || !sys->sched) return;

    uint32_t p = cpu->regs[VM_REG_A0];
    cpu->reload_period = p;

    if (p == 0) {
        cpu->reload_next_deadline = 0;
    } else {
        cpu->reload_next_deadline = sys->sched->global_tick + p;
    }

    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_YIELD_UNTIL_RELOAD (1048)
 *   (no arguments)
 *   → a0 = 0 on wake
 *
 * Blocks until reload_next_deadline. After waking, the
 * kernel advances reload_next_deadline by one (or more)
 * periods using the FreeRTOS-style catch-up policy: if the
 * guest is more than one period behind, skip ahead to the
 * next future boundary instead of firing the missed events
 * back-to-back. Phase is preserved; missed frames are
 * dropped cleanly.
 *
 * Misuse: if the guest calls yield_until_reload without
 * setting a period, we treat it as a plain yield and return
 * 0. This avoids surprising "blocks forever" behavior. */
static void handle_yield_until_reload(VmCpu *cpu, void *system_p) {
    VmSystem *sys = (VmSystem *)system_p;
    if (!cpu || !sys || !sys->sched) return;

    if (cpu->reload_period == 0) {
        /* No period set — degrade to plain yield. */
        cpu->regs[VM_REG_A0] = 0;
        cpu->block_reason = BLOCK_YIELDED;
        return;
    }

    uint32_t now      = sys->sched->global_tick;
    uint32_t deadline = cpu->reload_next_deadline;
    uint32_t period   = cpu->reload_period;

    /* Find the next deadline that's STRICTLY in the future.
     *
     * Why "in the future" rather than "advance by exactly one
     * period": on real systems the host scheduler is jittery
     * — Cygwin/Windows in particular can preempt the host
     * process for hundreds of ms at a time. By the time we get
     * here, `now` may already be well past `deadline`. We want
     * the guest's next wake-up to land on a future tick boundary,
     * which preserves phase but cleanly drops missed frames.
     *
     * Earlier versions of this handler had a separate "wake
     * immediately if already past" branch (BLOCK_YIELDED). That
     * caused visible stutter: any time the host overslept by
     * even a few ms, the guest would rapid-fire ONE catch-up
     * frame and then resume. From the user's POV, the snake
     * would jump two cells back-to-back. The fix is to just
     * always BLOCK_SLEEP until the next future boundary — no
     * rapid-fire, no catch-up bursts. */
    uint32_t next = deadline;
    int safety = 1024;
    /* Advance while `next` is NOT strictly in the future. The
     * <= comparison (via signed-subtract) catches both "already
     * past" and "exactly at now". */
    while ((int32_t)(now - next) >= 0 && safety-- > 0) {
        next += period;
    }
    cpu->reload_next_deadline = next + period;

    cpu->block_reason = BLOCK_SLEEP;
    cpu->block_deadline = next;
}

/* ============================================================
 *  Install all the system-context handlers.
 *
 *  Best-effort all-or-nothing: rolls back on the first failure.
 * ============================================================ */

static bool install_system_handlers(VmEcallRouter *r) {
    struct { uint32_t num; VmEcallHandler h; } entries[] = {
        { SYS_ALLOC,                handle_alloc              },
        { SYS_FREE,                 handle_free               },
        { SYS_SEND,                 handle_send               },
        { SYS_RECV,                 handle_recv               },
        { SYS_MAILBOX_INFO,         handle_mailbox_info       },
        { SYS_WHITELIST_ADD,        handle_whitelist_add      },
        { SYS_WHITELIST_REMOVE,     handle_whitelist_remove   },
        { SYS_TICKS_NOW,            handle_ticks_now          },
        { SYS_TICK_HZ,              handle_tick_hz            },
        { SYS_SLEEP_TICKS,          handle_sleep_ticks        },
        { SYS_SLEEP_UNTIL,          handle_sleep_until        },
        { SYS_SET_RELOAD_PERIOD,    handle_set_reload_period  },
        { SYS_YIELD_UNTIL_RELOAD,   handle_yield_until_reload },
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

    /* Per-VM sizing defaults. */
    if (sys->config.max_vms == 0) sys->config.max_vms = 8;
    if (sys->config.spawn_data_kb == 0) sys->config.spawn_data_kb = 16;

    /* Wire public pointers to the internal instances. */
    sys->ecall_router = &sys->_ecall_router;
    sys->sched        = &sys->_sched;
    sys->shared_slab  = &sys->_shared_slab;
    sys->local_slab   = &sys->_local_slab;

    /* 1. Initialize the shared slab. */
    SlabResult sr = slab_init(sys->shared_slab,
                               sys->config.shared_storage,
                               sys->config.shared_storage_size,
                               &sys->config.slab_config,
                               slab_null_locker);
    if (sr != SLAB_OK) {
        return false;
    }

    /* 2. Initialize the local slab.
     *
     * Bin sizing is derived from max_vms and spawn_data_kb. Each
     * VM consumes one block from each of these bins on load:
     *
     *    VmCpu          - one  512 B block
     *    Mailbox        - one    2 KB block (default mailbox shape)
     *    Text           - one block at ceil-pow2(text filesz)
     *    Rodata         - one block at ceil-pow2(rodata filesz)
     *    Data region    - one  bin sized to ceil-pow2(spawn_data_kb)
     *
     * Text and rodata sizes vary wildly across guests:
     *   - minimal hello.elf: ~10 B text, no rodata
     *   - shell.elf: ~30 KB text, several KB rodata
     *   - TUI guests: text 5-16 KB, rodata < 1 KB
     *
     * Rather than picking one text bin and rounding everything up,
     * we populate several small-to-medium bins (256 B through the
     * configured spawn_data bin) with max_vms+headroom slots each.
     * The slab's per-block headers are 8 B so the overhead is fine
     * even on small bins. This way any guest fits into some bin
     * without manual tuning.
     *
     * Add a small headroom in each bin (one extra slot) so a brief
     * over-by-one due to ELF variation or temp allocations during
     * load doesn't hit the limit. */
    SlabConfig local_cfg = (SlabConfig){0};
    uint16_t headroom = (uint16_t)((sys->config.max_vms < 32)
                                   ? 1 : (sys->config.max_vms / 16));
    uint16_t per_bin_slots = sys->config.max_vms + headroom;

    int bin_cpu  = slab_bin_for_size(sizeof(VmCpu));
    int bin_mbox = slab_bin_for_size(2048);
    int bin_data = slab_bin_for_size(
                       (size_t)sys->config.spawn_data_kb * 1024);

    /* The per-VM bins. */
    local_cfg.bucket_counts[bin_cpu]  += per_bin_slots;
    local_cfg.bucket_counts[bin_mbox] += per_bin_slots;
    local_cfg.bucket_counts[bin_data] += per_bin_slots;

    /* Variable-size guest segments (text, rodata) might land in
     * any bin from 32 B (a near-empty .text in a minimal hello
     * program) up to slightly below the data bin. Make sure each
     * of those bins can hold at least one per-VM block — the
     * loader picks whichever bin fits. */
    int bin_segment_hi = bin_data > 0 ? bin_data - 1 : 0;
    for (int b = 0; b <= bin_segment_hi; b++) {
        local_cfg.bucket_counts[b] += per_bin_slots;
    }

    sr = slab_init(sys->local_slab,
                   sys->config.local_storage,
                   sys->config.local_storage_size,
                   &local_cfg,
                   slab_null_locker);
    if (sr != SLAB_OK) {
        return false;
    }

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

    /* Resources to clean up on failure. We track them as we go
     * and unwind in reverse order in the error path. */
    VmCpu *cpu = NULL;
    void  *mbox_storage = NULL;
    int    assigned = -1;

    /* 1. Allocate the VmCpu struct from the local slab. */
    cpu = (VmCpu *)slab_alloc(sys->local_slab, sizeof(VmCpu));
    if (!cpu) {
        result.code = VM_SYS_ERR_NO_ARENA_SPACE;
        goto fail;
    }

    /* 2. Init it with a placeholder vm_id; the scheduler will
     *    overwrite. */
    vm_init(cpu, 0);

    /* 3. Allocate mailbox storage and initialize the mailbox at
     *    a known slot in sys->mailboxes[]. We register with the
     *    scheduler first to get the id assigned. */
    assigned = vm_sched_register(sys->sched, cpu);
    if (assigned < 0) {
        result.code = VM_SYS_ERR_FULL;
        goto fail;
    }
    sys->vms[assigned] = cpu;

    size_t mbox_storage_bytes = vm_mailbox_required_storage_bytes(
        mailbox_slot_size, mailbox_depth);
    if (mbox_storage_bytes == 0) {
        result.code = VM_SYS_ERR_INVALID_ARG;
        goto fail;
    }
    mbox_storage = slab_alloc(sys->local_slab, mbox_storage_bytes);
    if (!mbox_storage) {
        result.code = VM_SYS_ERR_NO_ARENA_SPACE;
        goto fail;
    }
    VmMailboxResult mr = vm_mailbox_init(&sys->mailboxes[assigned],
                                          mbox_storage,
                                          mailbox_slot_size,
                                          mailbox_depth);
    if (mr != VM_MBOX_OK) {
        result.code = VM_SYS_ERR_INVALID_ARG;
        goto fail;
    }

    /* 4. Load the ELF. Any allocations vm_load made (text/rodata
     *    in COPY_RAM mode + the data region) get freed in the
     *    error path below via the CPU's region table. */
    VmLoaderConfig loader_cfg = {
        .code_backing      = code_backing,
        .rodata_backing    = rodata_backing,
        .ram_arena         = sys->local_slab,
        .region_data_size  = data_region_size,
        .shared_base       = sys->config.shared_storage,
        .shared_size       = (uint32_t)sys->config.shared_storage_size,
    };
    VmLoadResult lr = vm_load(cpu, elf_image, elf_size, &loader_cfg);
    if (lr != VM_LOAD_OK) {
        result.code = VM_SYS_ERR_LOAD_FAILED;
        result.load_result = lr;
        goto fail;
    }

    result.assigned_vm_id = assigned;
    return result;

fail:
    /* Unwind in reverse order of acquisition.
     *
     * vm_load doesn't clean up its own partial allocations on
     * failure (it returns immediately when a PT_LOAD fails), so
     * we walk the cpu's region table and free any RAM-backed
     * regions ourselves. slab_free silently ignores NULL and
     * non-slab pointers (XIP regions), so this is safe even when
     * vm_load never ran. */
    if (cpu) {
        for (uint32_t i = 0; i < VM_REGION_COUNT; i++) {
            VmRegion *r = &cpu->regions[i];
            if (r->base) {
                slab_free(sys->local_slab, r->base);
                r->base = NULL;
                r->length = 0;
            }
        }
    }
    if (assigned >= 0) {
        vm_sched_unregister(sys->sched, (uint16_t)assigned);
        sys->vms[assigned] = NULL;
    }
    if (mbox_storage) slab_free(sys->local_slab, mbox_storage);
    if (cpu) slab_free(sys->local_slab, cpu);
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
 *  vm_system_unload_vm — reclaim a VM's allocations
 * ============================================================ */

bool vm_system_unload_vm(VmSystem *sys, uint16_t vm_id) {
    if (!sys) return false;
    if (vm_id >= VM_SCHED_MAX_VMS) return false;

    VmCpu *cpu = sys->vms[vm_id];
    if (!cpu) return false;

    /* Ensure the VM won't be stepped further (defensive — most
     * callers will already have observed the halt). */
    cpu->halted = true;

    /* Walk regions and free any that came from the local slab.
     * We try to free every region's base pointer; the slab's
     * bounds check filters out XIP regions whose base points
     * into the caller-provided ELF buffer rather than the slab.
     *
     * The slab also tolerates NULL via early-return (regions
     * with length==0 still have base==NULL after init). */
    for (uint32_t i = 0; i < VM_REGION_COUNT; i++) {
        VmRegion *r = &cpu->regions[i];
        if (!r->base) continue;
        SlabResult sr = slab_free(sys->local_slab, r->base);
        (void)sr;   /* SLAB_ERR_FOREIGN_POINTER and SLAB_ERR_INVALID_ARG
                     * are acceptable here — they just mean this region
                     * wasn't slab-allocated (XIP). */
        r->base = NULL;
        r->length = 0;
    }

    /* Free the mailbox storage. The mailbox wraps a FifoQueue
     * which wraps a RingBuffer; the storage pointer lives at
     * mailbox._fifo.rb.storage. slab_free silently ignores
     * NULL or foreign pointers. */
    {
        void *mbox_storage = sys->mailboxes[vm_id]._fifo.rb.storage;
        if (mbox_storage) {
            slab_free(sys->local_slab, mbox_storage);
        }
    }

    /* Free the VmCpu itself. After this, cpu is dangling — must
     * NULL the slot before any caller observes the system. */
    slab_free(sys->local_slab, cpu);

    /* Unregister from scheduler and clear the slot. */
    vm_sched_unregister(sys->sched, vm_id);
    sys->vms[vm_id] = NULL;

    /* Zero the mailbox so a future load gets a clean slot. */
    memset(&sys->mailboxes[vm_id], 0, sizeof(sys->mailboxes[vm_id]));

    return true;
}

/* ============================================================
 *  vm_system_local_required — estimate slab region size
 * ============================================================ */

size_t vm_system_local_required(uint16_t max_vms, uint32_t spawn_data_kb) {
    if (max_vms == 0) max_vms = 8;
    if (spawn_data_kb == 0) spawn_data_kb = 16;

    /* Mirror the bin selection logic from vm_system_init. */
    uint16_t headroom = (max_vms < 32) ? 1 : (max_vms / 16);
    uint16_t per_bin = max_vms + headroom;

    SlabConfig cfg = (SlabConfig){0};

    int bin_cpu  = slab_bin_for_size(sizeof(VmCpu));
    int bin_mbox = slab_bin_for_size(2048);
    int bin_data = slab_bin_for_size((size_t)spawn_data_kb * 1024);

    cfg.bucket_counts[bin_cpu]  += per_bin;
    cfg.bucket_counts[bin_mbox] += per_bin;
    cfg.bucket_counts[bin_data] += per_bin;

    int bin_segment_hi = bin_data > 0 ? bin_data - 1 : 0;
    for (int b = 0; b <= bin_segment_hi; b++) {
        cfg.bucket_counts[b] += per_bin;
    }

    return slab_required_bytes(&cfg);
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
