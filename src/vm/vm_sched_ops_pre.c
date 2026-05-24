/* ============================================================
 *  vm_sched_ops_pre.c — preemptive backend ops (presched)
 *  Public domain (CC0). No warranty.
 *
 *  Implements the VmSchedOps vtable on top of the preemptive
 *  scheduler (presched.h). Each VM runs in its own presched task
 *  whose body drives vm_step and routes ECALLs through the
 *  system's router IN ITS OWN THREAD — so a blocking handler
 *  (handle_recv) parks that thread via presched_block, and the
 *  woken receiver delivers the message into its own memory. The
 *  cross-VM synchronous write the cooperative backend uses is
 *  dropped here (see handle_send in vm_system.c).
 *
 *  Concurrency safety: send/recv touch the shared mailbox queue
 *  from different task threads, so each mailbox runs a real mutex
 *  locker (vm_pre_mailbox_locker); the slab/FS are audit items
 *  #3/#5, out of scope here.
 *
 *  The whole file compiles to nothing unless the preemptive mode
 *  is selected, so a cooperative build links neither presched nor
 *  pthreads through it.
 * ============================================================ */

#include "config.h"

#if GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE

#include "vm/vm_pre.h"
#include "vm/vm_system.h"
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"
#include "vm/presched.h"

#include <string.h>

/* Per-vm_step budget. Preemption is external (the systick), so this
 * is just "a chunk" — big enough to be efficient, small enough to
 * reach ECALLs promptly. Mirrors examples/07_vm_task. */
#define VM_PRE_STEP_BUDGET  50000u

/* ---- the VM task body ----------------------------------------
 * Runs as a presched task. Drives vm_step; on ECALL routes through
 * the standard handlers (which, under this mode, block/wake on
 * presched). Returns when the guest halts or traps. */
static void vm_pre_task_body(void *arg) {
    struct { void *sys; uint16_t vm_id; } *ta = arg;
    VmSystem *sys = (VmSystem *)ta->sys;
    VmCpu *cpu = sys->vms[ta->vm_id];
    if (!cpu) return;

    for (;;) {
        uint32_t steps = 0;
        VmStepResult r = vm_step(cpu, VM_PRE_STEP_BUDGET, &steps);
        if (r == VM_STEP_HALTED || r == VM_STEP_TRAPPED) break;
        if (r == VM_STEP_ECALL) {
            vm_ecall_dispatch(sys->ecall_router, cpu, sys);
            if (cpu->halted) break;
        }
        /* VM_STEP_QUANTUM_EXPIRED (or resumed ECALL): keep going;
         * the systick preempts this thread between/within steps. */
    }

    /* Halted. If a parent is in SYS_SPAWN_AND_WAIT on us, wake it so it
     * can reap our exit code. (The parent reads our state then unloads
     * us; we touch nothing after this wake.) The wake is sticky, and the
     * parent also checks our halted flag before it parks, so this is not
     * a lost-wake even if we finished before it parked. */
    {
        VmPreCtx *pc = (VmPreCtx *)sys->ops.ctx;
        for (uint16_t pid = 0; pid < VM_SCHED_MAX_VMS; pid++) {
            VmCpu *p = sys->vms[pid];
            if (p && p->block_reason == BLOCK_ON_CHILD &&
                p->block_child_vm == ta->vm_id) {
                presched_wake(pc->sched, pc->task_for_vm[pid]);
                break;
            }
        }
    }
}

/* ---- ops ------------------------------------------------------ */

static uint32_t pre_now(void *ctx) {
    return (uint32_t)presched_total_ticks(((VmPreCtx *)ctx)->sched);
}
static uint32_t pre_ticks_hz(void *ctx) {
    return ((VmPreCtx *)ctx)->ticks_per_second;
}

static int pre_register_vm(void *ctx, VmCpu *cpu) {
    VmPreCtx *pc = (VmPreCtx *)ctx;
    if (!cpu) return -2;
    int id = -1;
    for (int i = 0; i < VM_SCHED_MAX_VMS; i++) {
        if (!pc->used[i]) { id = i; break; }
    }
    if (id < 0) return -1;                 /* no free slot */

    cpu->vm_id = (uint16_t)id;
    pc->targ[id].sys   = pc->sys;
    pc->targ[id].vm_id = (uint16_t)id;
    int tid = presched_add_task(pc->sched, vm_pre_task_body, &pc->targ[id]);
    if (tid < 0) return -1;                /* presched task cap reached */

    pc->used[id]        = true;
    pc->task_for_vm[id] = tid;
    return id;
}

static void pre_unregister_vm(void *ctx, uint16_t vm_id) {
    VmPreCtx *pc = (VmPreCtx *)ctx;
    if (vm_id < VM_SCHED_MAX_VMS) pc->used[vm_id] = false;
    /* presched has no task-removal; tasks run to completion. Unload
     * happens after the run, so there is nothing to stop here. */
}

static bool pre_run(void *ctx, uint64_t max_cycles) {
    (void)max_cycles;   /* presched runs until every task body returns */
    presched_run(((VmPreCtx *)ctx)->sched);
    return true;
}

static VmSchedStepResult pre_step(void *ctx) {
    (void)ctx;
    /* Single-stepping is a cooperative concept; the preemptive
     * backend drives itself. Report "done" so any step-loop ends. */
    return VM_SCHED_ALL_HALTED;
}

static bool pre_wake_mailbox(void *ctx, uint16_t vm_id, int32_t a0_value) {
    VmPreCtx *pc = (VmPreCtx *)ctx;
    (void)a0_value;   /* the woken receiver computes its own a0 */
    if (vm_id >= VM_SCHED_MAX_VMS || !pc->used[vm_id]) return false;
    presched_wake(pc->sched, pc->task_for_vm[vm_id]);
    return true;
}

static bool pre_wake_child(void *ctx, uint16_t vm_id, int32_t a0_value) {
    (void)ctx; (void)vm_id; (void)a0_value;
    /* SYS_SPAWN_AND_WAIT under preemption is a later audit item. */
    return false;
}

static const VmSchedOps pre_ops = {
    .now           = pre_now,
    .ticks_hz      = pre_ticks_hz,
    .register_vm   = pre_register_vm,
    .unregister_vm = pre_unregister_vm,
    .run           = pre_run,
    .step          = pre_step,
    .wake_mailbox  = pre_wake_mailbox,
    .wake_child    = pre_wake_child,
    .ctx           = NULL,
};

const VmSchedOps *vm_sched_ops_preemptive(void) {
    return &pre_ops;
}

/* ---- mailbox locker (per-mailbox mutex) ----------------------- */

static uintptr_t pre_mtx_lock(void *ctx) {
    pthread_mutex_lock((pthread_mutex_t *)ctx);
    return 0;
}
static void pre_mtx_unlock(void *ctx, uintptr_t saved) {
    (void)saved;
    pthread_mutex_unlock((pthread_mutex_t *)ctx);
}

VmMailboxLocker vm_pre_mailbox_locker(VmPreCtx *pc, uint16_t vm_id) {
    VmMailboxLocker lk = { pre_mtx_lock, pre_mtx_unlock, &pc->mtx[vm_id] };
    return lk;
}

/* The SlabLocker has the same lock/unlock/ctx shape as VmMailboxLocker,
 * so the same mutex callbacks back both. */
SlabLocker vm_pre_slab_locker(pthread_mutex_t *mtx) {
    SlabLocker lk = { pre_mtx_lock, pre_mtx_unlock, mtx };
    return lk;
}

/* ---- context lifecycle ---------------------------------------- */

bool vm_pre_ctx_init(VmPreCtx *pc, void *sys, unsigned tick_us,
                     uint32_t ticks_hz) {
    memset(pc, 0, sizeof *pc);
    pc->sys              = sys;
    pc->ticks_per_second = ticks_hz;
    pc->sched            = presched_create(tick_us ? tick_us : 1000);
    if (!pc->sched) return false;
    for (int i = 0; i < VM_SCHED_MAX_VMS; i++) {
        pthread_mutex_init(&pc->mtx[i], NULL);
    }
    return true;
}

#endif /* GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE */

/* In the cooperative build everything above is #if'd out, leaving an
 * empty translation unit — which ISO C forbids and the example build's
 * -pedantic -Werror rejects. This keeps the TU non-empty in all modes. */
typedef int vm_sched_ops_pre_tu_guard;
