/* ============================================================
 *  vm_ecall_handlers.c — built-in CPU-only ECALL handlers
 *
 *  This file holds the handlers that touch only the VM's own
 *  CPU state (cpu->halted, cpu->in_critical, cpu->vm_id, etc.)
 *  without needing access to the wider system context (slab,
 *  mailbox table, scheduler).
 *
 *  Handlers that DO need system context (SYS_ALLOC, SYS_FREE,
 *  SYS_SEND, SYS_RECV, etc.) live in vm_system.c since they
 *  reach into the VmSystem struct.
 *
 *  Each handler here matches the ABI described in vm_ecall.h:
 *  args read from regs[a0..a5], number from regs[a7], result
 *  written to regs[a0]. The 'system' pointer is ignored by these
 *  handlers since they don't need it.
 *
 *  All handlers here are intended to be registered against the
 *  appropriate SYS_* number via vm_ecall_register. The function
 *  vm_ecall_install_cpu_handlers (defined at the bottom) does
 *  this for all of them in one call — vm_system_init calls it
 *  during setup.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_ecall.h"
#include "vm/vm_core.h"

/* ============================================================
 *  SYS_EXIT (a7 = 93)
 *
 *    a0 = exit code (informational; we don't propagate it
 *         beyond setting cpu->halted = true)
 *    → does not return (next vm_step returns VM_STEP_HALTED)
 *
 *  We don't write to a0 — the halted flag is the only signal.
 *  The host's scheduler observes cpu->halted after dispatch
 *  and stops scheduling this VM.
 * ============================================================ */
void vm_handle_exit(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;
    cpu->halted = true;
}

/* ============================================================
 *  SYS_SELF (a7 = 1024)
 *
 *    (no arguments)
 *    → a0 = this VM's vm_id
 *
 *  Always succeeds. vm_id is a uint16_t so the result is always
 *  in the non-negative half of int32_t.
 * ============================================================ */
void vm_handle_self(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;
    cpu->regs[VM_REG_A0] = (uint32_t)cpu->vm_id;
}

/* ============================================================
 *  SYS_YIELD (a7 = 1040)
 *
 *    (no arguments)
 *    → a0 = 0
 *  Side effect: cpu->block_reason = BLOCK_YIELDED.
 *
 *  The scheduler observes block_reason after dispatch and moves
 *  the VM out of the ready set for one round. A VM in
 *  BLOCK_YIELDED is auto-woken on the next cycle.
 * ============================================================ */
void vm_handle_yield(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;
    cpu->regs[VM_REG_A0] = 0;
    cpu->block_reason = BLOCK_YIELDED;
}

/* ============================================================
 *  SYS_CRITICAL_ENTER (a7 = 1041)
 *
 *    (no arguments)
 *    → a0 = 0 on success
 *    → a0 = -EBUSY if already in a critical section (no nesting)
 *  Side effect (on success): cpu->in_critical = true.
 *
 *  Nesting is rejected with -EBUSY rather than supported. If a
 *  guest needs to enter critical-while-critical, that's a guest
 *  bug — the second enter would be a no-op and the EXIT would
 *  prematurely release the section. Failing loudly is better.
 * ============================================================ */
void vm_handle_critical_enter(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;
    if (cpu->in_critical) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBUSY);
        return;
    }
    cpu->in_critical = true;
    cpu->regs[VM_REG_A0] = 0;
}

/* ============================================================
 *  SYS_CRITICAL_EXIT (a7 = 1042)
 *
 *    (no arguments)
 *    → a0 = 0 on success
 *    → a0 = -EINVAL if not currently in a critical section
 *  Side effect (on success): cpu->in_critical = false.
 * ============================================================ */
void vm_handle_critical_exit(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;
    if (!cpu->in_critical) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EINVAL);
        return;
    }
    cpu->in_critical = false;
    cpu->regs[VM_REG_A0] = 0;
}

/* ============================================================
 *  Bulk registration
 *
 *  Installs all CPU-only handlers into the given router. Called
 *  from vm_system_init. Returns false on registration failure
 *  (e.g., conflict — caller already registered something at one
 *  of these slots), true on full success.
 *
 *  Conflict handling: registers each one; on first failure,
 *  unregisters those already registered and returns false. This
 *  is best-effort all-or-nothing; the caller can also detect
 *  partial install by checking the return.
 * ============================================================ */

/* Forward declaration so we can publish the API in a small header
 * later (or just export through vm_system.h). For now, declared
 * at file scope; vm_system.c can either #include this file's
 * declarations via a header or declare extern manually. */
bool vm_ecall_install_cpu_handlers(VmEcallRouter *r);

bool vm_ecall_install_cpu_handlers(VmEcallRouter *r) {
    if (!r) return false;

    struct { uint32_t num; VmEcallHandler h; } entries[] = {
        { SYS_EXIT,           vm_handle_exit            },
        { SYS_SELF,           vm_handle_self            },
        { SYS_YIELD,          vm_handle_yield           },
        { SYS_CRITICAL_ENTER, vm_handle_critical_enter  },
        { SYS_CRITICAL_EXIT,  vm_handle_critical_exit   },
    };
    size_t n = sizeof(entries) / sizeof(entries[0]);

    for (size_t i = 0; i < n; i++) {
        if (!vm_ecall_register(r, entries[i].num, entries[i].h)) {
            /* Roll back: unregister what we did install. */
            for (size_t j = 0; j < i; j++) {
                vm_ecall_unregister(r, entries[j].num);
            }
            return false;
        }
    }
    return true;
}
