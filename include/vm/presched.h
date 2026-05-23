/* ============================================================
 *  presched.h — preemptive thread-per-task scheduler (skeleton)
 *
 *  STEP 1 of the preemptive scheduler (see docs/execution-model.md §7).
 *  This is the BARE round-robin skeleton: N tasks, each in its own OS
 *  thread, time-sliced by a systick that preempts round-robin across
 *  them at a single priority level. It proves the preemption mechanism
 *  and that nothing deadlocks. Priorities, blocking, VM tasks, and
 *  services are LATER steps — not here yet.
 *
 *  Model:
 *    - A "task" is an OS thread the scheduler preempts, plus its run
 *      state. The thread's entry function is what it runs — a native
 *      compute loop here; the VM interpreter loop later. The scheduler
 *      is content-agnostic.
 *    - A "systick" fires periodically. On each tick the scheduler makes
 *      the current task yield the CPU and lets the next ready task run
 *      (round-robin). Exactly one task runs at a time (this skeleton
 *      models a single core).
 *
 *  Preemption primitive (platform):
 *    - POSIX: a periodic timer (timer_create + SIGEV_SIGNAL) delivers a
 *      systick signal to the running task thread; the handler parks that
 *      thread and the scheduler releases the next. VALIDATED in-sandbox.
 *      Validates scheduling LOGIC, not SuspendThread's async semantics.
 *    - Windows: a timer thread + SuspendThread/ResumeThread. The real
 *      async suspend. COMPILE-CHECKED only here; user-verified on
 *      Windows (same boundary as channel_win32.c / waveOut).
 *
 *  Single-core model: this skeleton runs exactly one task at a time, as
 *  a real single-core RTOS would. The OS may place threads on multiple
 *  cores, so the scheduler enforces single-task-running via per-task run
 *  gates — that enforcement is the thing under test.
 *
 *  Depends on: nothing (pthreads/Win32 + a timer, linked by the host)
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef PRESCHED_H
#define PRESCHED_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef PRESCHED_MAX_TASKS
#define PRESCHED_MAX_TASKS  16
#endif

/* A task's body. Runs in its own thread; returns when the task is
 * done. The scheduler preempts it between/within iterations via the
 * systick — the body needs no yield calls (preemption is external).
 * `arg` is the user pointer passed to presched_add_task. */
typedef void (*presched_task_fn)(void *arg);

typedef struct PreSched PreSched;

/* Number of priority levels. Level 0 is lowest, PRESCHED_PRIO_LEVELS-1
 * is highest. Kept small and <= 32 so the ready bitmap is one uint32
 * and "highest ready level" is a single clz. */
#ifndef PRESCHED_PRIO_LEVELS
#define PRESCHED_PRIO_LEVELS  8
#endif
#define PRESCHED_PRIO_MIN  0
#define PRESCHED_PRIO_MAX  (PRESCHED_PRIO_LEVELS - 1)

/* Create a scheduler with a given systick period. tick_us is the
 * systick period in microseconds (e.g. 1000 = 1ms). Returns NULL on
 * failure. */
PreSched *presched_create(unsigned tick_us);

/* Destroy the scheduler (must be stopped first). */
void presched_destroy(PreSched *s);

/* Register a task at a given priority level (0..PRESCHED_PRIO_MAX;
 * clamped into range). The scheduler always runs a task from the
 * highest priority level that has a ready task; tasks at the SAME level
 * round-robin. Strict priority: a never-blocking high-priority task
 * starves lower levels by design (the task designer's responsibility).
 * Returns the task id (>=0) or -1 on failure. */
int presched_add_task_prio(PreSched *s, presched_task_fn fn, void *arg,
                           int priority);

/* Convenience: register at a default mid priority. (Back-compatible
 * with the step-1 single-level usage — all default-priority tasks
 * round-robin together.) */
int presched_add_task(PreSched *s, presched_task_fn fn, void *arg);

/* Run the scheduler until all tasks have returned. Starts the systick,
 * releases the first task, and blocks until every task body has
 * finished. Returns when the run is complete. */
void presched_run(PreSched *s);

/* ---- Block / wake / sleep (step 3) ----------------------------
 * Called from within a task body (the calling task is "self").
 *
 * presched_self_id() — id of the calling task (-1 if not in one).
 *
 * presched_block() — block the calling task: it leaves the ready set,
 *   hands the CPU to the next ready task, and does not run again until
 *   woken. Wakeups are STICKY: a presched_wake(self) issued before this
 *   block is remembered, so block returns immediately and consumes it —
 *   no lost-wakeup race even without a caller-side condition loop.
 *
 * presched_wake(id) — mark task `id` runnable again. Safe from any task.
 *   If `id` is blocked it becomes ready; if not yet blocked the wake is
 *   recorded as pending (sticky).
 *
 * presched_sleep(ticks) — block the calling task until at least `ticks`
 *   systicks have elapsed, then it becomes runnable again. */
int  presched_self_id(void);
void presched_block(PreSched *s);
void presched_wake(PreSched *s, int id);
void presched_sleep(PreSched *s, uint32_t ticks);

/* Diagnostics (read after presched_run returns). */
uint64_t presched_total_ticks(const PreSched *s);
uint64_t presched_switches(const PreSched *s);   /* context switches made */

#endif /* PRESCHED_H */
