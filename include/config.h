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

#endif /* GARBAGE_CONFIG_H */
