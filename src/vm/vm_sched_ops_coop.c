/* ============================================================
 *  vm_sched_ops_coop.c — cooperative backend ops (vm_sched)
 *  Public domain (CC0). No warranty.
 *
 *  A 1:1 forward of the VmSchedOps vtable to the existing
 *  vm_sched_* API. Installing these ops changes no cooperative
 *  behavior — they are exactly the calls vm_system used to make
 *  inline. `ctx` is the VmSched *.
 * ============================================================ */

#include "vm/vm_sched_ops.h"
#include "vm/vm_sched.h"

static uint32_t coop_now(void *ctx) {
    return ((VmSched *)ctx)->global_tick;
}
static uint32_t coop_ticks_hz(void *ctx) {
    return ((VmSched *)ctx)->config.ticks_per_second;
}
static int coop_register_vm(void *ctx, VmCpu *cpu) {
    return vm_sched_register((VmSched *)ctx, cpu);
}
static void coop_unregister_vm(void *ctx, uint16_t vm_id) {
    vm_sched_unregister((VmSched *)ctx, vm_id);
}
static bool coop_run(void *ctx, uint64_t max_cycles) {
    return vm_sched_run((VmSched *)ctx, max_cycles);
}
static VmSchedStepResult coop_step(void *ctx) {
    return vm_sched_step((VmSched *)ctx);
}
static bool coop_wake_mailbox(void *ctx, uint16_t vm_id, int32_t a0_value) {
    return vm_sched_wake_mailbox((VmSched *)ctx, vm_id, a0_value);
}
static bool coop_wake_child(void *ctx, uint16_t vm_id, int32_t a0_value) {
    return vm_sched_wake_child((VmSched *)ctx, vm_id, a0_value);
}

/* Template table; ctx is NULL here and filled in by the caller
 * (vm_system copies this by value and sets .ctx = sys->sched). */
static const VmSchedOps coop_ops = {
    .now           = coop_now,
    .ticks_hz      = coop_ticks_hz,
    .register_vm   = coop_register_vm,
    .unregister_vm = coop_unregister_vm,
    .run           = coop_run,
    .step          = coop_step,
    .wake_mailbox  = coop_wake_mailbox,
    .wake_child    = coop_wake_child,
    .ctx           = NULL,
};

const VmSchedOps *vm_sched_ops_cooperative(void) {
    return &coop_ops;
}
