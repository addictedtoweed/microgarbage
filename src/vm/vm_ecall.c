/* ============================================================
 *  vm_ecall.c — ECALL router implementation
 *  See vm/vm_ecall.h for the public contract.
 *
 *  This file is just the routing fabric. The actual ECALL
 *  handlers (SYS_EXIT, SYS_YIELD, SYS_ALLOC, etc.) live in
 *  their respective subsystem files (vm_ecall_handlers.c for
 *  the CPU-only handlers, vm_mailbox.c for messaging, etc.) and
 *  are registered with this router at system init.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_ecall.h"
#include <string.h>

/* ============================================================
 *  Default fallback — write -ENOSYS to a0
 *
 *  Called when the syscall number doesn't have a registered
 *  handler. Matches Linux behavior so guest libc that probes
 *  for optional syscalls (e.g., set_tid_address) gets a clean
 *  failure and continues.
 * ============================================================ */

static void default_fallback(VmCpu *cpu, void *system) {
    (void)system;
    if (cpu) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_ENOSYS);
    }
}

/* ============================================================
 *  Internal helpers — translate a syscall number into the
 *  table slot pointer it belongs in, or NULL if out of range.
 *
 *  Returns a pointer to the slot so registration can both
 *  install and inspect-for-existing. NULL means "no slot for
 *  this number; route to fallback".
 * ============================================================ */

static VmEcallHandler *slot_for(VmEcallRouter *r, uint32_t syscall_num) {
    if (!r) return NULL;

    if (syscall_num < VM_ECALL_LINUX_RANGE_START + VM_ECALL_LINUX_RANGE_SIZE) {
        /* 0..255 */
        return &r->linux_slots[syscall_num - VM_ECALL_LINUX_RANGE_START];
    }
    if (syscall_num >= VM_ECALL_VM_RANGE_START &&
        syscall_num <  VM_ECALL_VM_RANGE_START + VM_ECALL_VM_RANGE_SIZE) {
        /* 1024..1279 */
        return &r->vm_slots[syscall_num - VM_ECALL_VM_RANGE_START];
    }
    return NULL;
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

void vm_ecall_router_init(VmEcallRouter *r) {
    if (!r) return;
    memset(r, 0, sizeof(*r));
    r->fallback = default_fallback;
}

/* ============================================================
 *  Registration
 * ============================================================ */

bool vm_ecall_register(VmEcallRouter *r,
                       uint32_t syscall_num,
                       VmEcallHandler handler) {
    if (!r || !handler) return false;
    VmEcallHandler *slot = slot_for(r, syscall_num);
    if (!slot) return false;
    if (*slot != NULL) return false;   /* already registered */
    *slot = handler;
    return true;
}

bool vm_ecall_unregister(VmEcallRouter *r, uint32_t syscall_num) {
    if (!r) return false;
    VmEcallHandler *slot = slot_for(r, syscall_num);
    if (!slot) return false;
    if (*slot == NULL) return false;   /* nothing to remove */
    *slot = NULL;
    return true;
}

void vm_ecall_set_fallback(VmEcallRouter *r, VmEcallHandler fallback) {
    if (!r) return;
    /* NULL means "go back to the built-in default". */
    r->fallback = fallback ? fallback : default_fallback;
}

/* ============================================================
 *  Dispatch
 *
 *  Reads cpu->regs[VM_REG_A7] (the syscall number), looks up
 *  the handler, calls it with (cpu, system). Falls back to
 *  router->fallback for unregistered numbers or any number
 *  outside the supported ranges.
 * ============================================================ */

void vm_ecall_dispatch(VmEcallRouter *r, VmCpu *cpu, void *system) {
    if (!r || !cpu) return;

    uint32_t syscall_num = cpu->regs[VM_REG_A7];
    VmEcallHandler *slot = slot_for(r, syscall_num);
    VmEcallHandler handler = slot ? *slot : NULL;

    if (handler) {
        handler(cpu, system);
    } else if (r->fallback) {
        r->fallback(cpu, system);
    } else {
        /* No fallback at all (someone passed NULL to set_fallback
         * and we somehow ended up with NULL — shouldn't happen
         * given the init contract, but be defensive). */
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_ENOSYS);
    }
}
