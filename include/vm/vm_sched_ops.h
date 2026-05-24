/* ============================================================
 *  vm_sched_ops.h — the scheduler seam for vm_system
 *  Public domain (CC0). No warranty.
 *
 *  vm_system routes every scheduler interaction (tick reads, VM
 *  registration, run/step, and the wake hooks) through this small
 *  vtable instead of calling vm_sched_* directly. That makes the
 *  syscall-routing core scheduler-agnostic: the cooperative
 *  backend (vm_sched) and the preemptive backend (presched) each
 *  supply a VmSchedOps, and vm_system never has to know which is
 *  in use.
 *
 *  Selected once in vm_system_init. The cooperative ops are a 1:1
 *  forward to the existing vm_sched_* API — installing them does
 *  not change cooperative behavior. The preemptive ops live behind
 *  GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE (see config.h)
 *  and are added in a later step; a cooperative-only build never
 *  references the preemptive backend.
 *
 *  NOTE: blocking is deliberately NOT an op here. The two backends
 *  block in structurally opposite ways (cooperative sets
 *  block_reason and returns to the scheduler; preemptive parks the
 *  calling task thread), so the block/unblock shaping lives in
 *  vm_system itself, not in this vtable. This vtable covers only
 *  the interactions that have the same shape in both backends.
 * ============================================================ */

#ifndef VM_SCHED_OPS_H
#define VM_SCHED_OPS_H

#include <stdint.h>
#include <stdbool.h>

#include "vm/vm_sched.h"   /* VmSched, VmCpu, VmSchedStepResult */

typedef struct VmSchedOps {
    /* Current system tick and tick rate (cooperative: the
     * scheduler's global_tick / config.ticks_per_second). */
    uint32_t (*now)(void *ctx);
    uint32_t (*ticks_hz)(void *ctx);

    /* VM lifecycle. register_vm returns the assigned vm_id, or a
     * negative error (mirrors vm_sched_register). */
    int  (*register_vm)(void *ctx, VmCpu *cpu);
    void (*unregister_vm)(void *ctx, uint16_t vm_id);

    /* Drive execution. run loops until all VMs halt or max_cycles
     * elapse (0 = unbounded); step does one scheduling cycle. */
    bool              (*run)(void *ctx, uint64_t max_cycles);
    VmSchedStepResult (*step)(void *ctx);

    /* Unblock hooks called from ECALL handlers. Return false if the
     * target was not blocked on the matching reason. */
    bool (*wake_mailbox)(void *ctx, uint16_t vm_id, int32_t a0_value);
    bool (*wake_child)(void *ctx, uint16_t vm_id, int32_t a0_value);

    /* Backend context: the cooperative ops cast this to VmSched *
     * (it is set to sys->sched). */
    void *ctx;
} VmSchedOps;

/* The cooperative backend's ops — a thin forward to vm_sched_*.
 * The returned table is static/const; install it with .ctx set to
 * the VmSched * (vm_system does this in vm_system_init). */
const VmSchedOps *vm_sched_ops_cooperative(void);

#endif /* VM_SCHED_OPS_H */
