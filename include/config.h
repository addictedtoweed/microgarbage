/* ============================================================
 *  config.h — compile-time configuration for the garbage library
 *
 *  Selects build-wide options. Override any of these by defining
 *  them before including this header (e.g. -DGARBAGE_SCHED_MODE=...)
 *  or by editing this file for your target.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef GARBAGE_CONFIG_H
#define GARBAGE_CONFIG_H

/* ------------------------------------------------------------
 *  Scheduling discipline
 *
 *  The execution model is selected here (see docs/execution-model.md).
 *  Both configurations are multi-task; the axis is scheduling
 *  discipline, not task count.
 *
 *    GARBAGE_SCHED_COOPERATIVE — the existing vm_sched: one host
 *        thread, budgeted vm_step, tasks yield/block. No systick.
 *        Lower footprint. The accessible default and the safety net.
 *
 *    GARBAGE_SCHED_PREEMPTIVE  — thread-per-task, a systick preempts
 *        across a fixed-priority ready set. Real on the micro;
 *        simulated on Windows (timer thread + SuspendThread) and on
 *        POSIX (timer signal) for development.
 *
 *  The unused mode's code is stripped on the micro.
 * ------------------------------------------------------------ */
#define GARBAGE_SCHED_COOPERATIVE  0
#define GARBAGE_SCHED_PREEMPTIVE   1

#ifndef GARBAGE_SCHED_MODE
#define GARBAGE_SCHED_MODE  GARBAGE_SCHED_COOPERATIVE
#endif

/* ------------------------------------------------------------
 *  Memory protection tier (software VM-ownership map)
 *
 *  Selects per-block memory protection for the SHARED region. Private
 *  code/rodata/data segments are always isolated per-VM by the
 *  interpreter's region bounds regardless of this setting.
 *
 *    OFF   (0) — no ownership check; the hooks compile out entirely
 *                (zero cost; the lowest-tier micro / smallest runtime)
 *    SWMAP (1) — tier-1 software owner map: a per-owner check on
 *                shared-region access, folded into vm_translate so the
 *                interpreter AND every ecall shim inherit it. MMU /
 *                TrustZone tiers select in later behind the same seam.
 *
 *  The call sites (vm_core.c xlat, vm_system.c SYS_ALLOC) exist today
 *  but the owner side-table is not wired yet, so enabling this does
 *  not yet change behavior. See include/vm/vm_mem_protect.h.
 * ------------------------------------------------------------ */
#define GARBAGE_MEM_PROTECT_OFF    0
#define GARBAGE_MEM_PROTECT_SWMAP  1

#ifndef GARBAGE_MEM_PROTECT
#define GARBAGE_MEM_PROTECT  GARBAGE_MEM_PROTECT_OFF
#endif

/* ------------------------------------------------------------
 *  Container default pool sizes
 *
 *  Aggregated here so one umbrella config carries the build-wide
 *  knobs, but the per-type node-count defaults live in their own
 *  file. Same override rule: define a knob before this header is
 *  reached (build -D or host pre-include) and its #ifndef leaves
 *  your value untouched.
 * ------------------------------------------------------------ */
#include "containers/containers_config.h"

#endif /* GARBAGE_CONFIG_H */
