/* ============================================================
 *  vm_pre.h — preemptive backend context for vm_system
 *  Public domain (CC0). No warranty.
 *
 *  The VmSchedOps "ctx" for the preemptive backend. Holds the
 *  PreSched, the vm_id <-> presched-task-id map, the per-task
 *  bodies' args, and a per-mailbox mutex that backs the real
 *  VmMailboxLocker (so concurrent send/recv across VM task threads
 *  is safe — the cooperative build keeps the null locker).
 *
 *  Entirely behind GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE:
 *  a cooperative build sees an empty header and pulls in neither
 *  presched nor pthreads.
 *
 *  NOTE: PRESCHED_MAX_TASKS (16) < VM_SCHED_MAX_VMS (64). The
 *  preemptive backend maps one task per VM, so it supports at most
 *  PRESCHED_MAX_TASKS live VMs; register_vm fails past that. The
 *  IPC proof uses two. Raising the task cap is a presched concern,
 *  not this layer's.
 * ============================================================ */

#ifndef VM_PRE_H
#define VM_PRE_H

#include "config.h"

#if GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include "vm/vm_sched.h"        /* VM_SCHED_MAX_VMS, VmCpu */
#include "vm/vm_sched_ops.h"    /* VmSchedOps */
#include "vm/vm_mailbox.h"      /* VmMailboxLocker */
#include "vm/presched.h"        /* PreSched */

typedef struct {
    PreSched *sched;
    void     *sys;                 /* VmSystem * (cast in the .c) */
    uint32_t  ticks_per_second;

    bool used[VM_SCHED_MAX_VMS];        /* assigned vm_id slots */
    int  task_for_vm[VM_SCHED_MAX_VMS]; /* presched task id per vm_id */

    /* Per-task body argument (sys + vm_id), referenced by presched. */
    struct { void *sys; uint16_t vm_id; } targ[VM_SCHED_MAX_VMS];

    /* Per-mailbox mutex backing the real locker under preemption. */
    pthread_mutex_t mtx[VM_SCHED_MAX_VMS];
} VmPreCtx;

/* Initialize the context: create the PreSched (tick_us systick) and
 * the per-mailbox mutexes. Returns false on allocation failure. */
bool vm_pre_ctx_init(VmPreCtx *pc, void *sys, unsigned tick_us,
                     uint32_t ticks_hz);

/* The preemptive ops table (template; install with .ctx = the
 * VmPreCtx *). */
const VmSchedOps *vm_sched_ops_preemptive(void);

/* The real (mutex) mailbox locker for a given vm_id's mailbox.
 * Install on each mailbox under preemption via vm_mailbox_set_locker. */
VmMailboxLocker vm_pre_mailbox_locker(VmPreCtx *pc, uint16_t vm_id);

#endif /* GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE */
#endif /* VM_PRE_H */
