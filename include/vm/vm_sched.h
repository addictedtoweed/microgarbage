/* ============================================================
 *  vm_sched.h — cooperative round-robin scheduler with quanta
 *
 *  The scheduler is the system's main loop. It holds all the VMs,
 *  decides which one runs next, calls vm_step on it with an
 *  instruction budget, and processes the result: routing ECALLs,
 *  handling traps, moving blocked VMs out of the ready set and
 *  back in when their condition is met.
 *
 *  ---------------------------------------------------------------
 *  Scheduling model
 *  ---------------------------------------------------------------
 *
 *  Strict round-robin over the ready set. Each ready VM gets
 *  exactly one quantum per cycle through the bitmap. When it has
 *  consumed its quantum (or yielded, or trapped, or blocked), the
 *  scheduler moves on to the next VM. No priorities. Fairness is
 *  by equal turns.
 *
 *  This is the simplest model that supports the use cases:
 *    - Many independent threads making predictable forward
 *      progress (a 28-thread workload sees each thread once per
 *      28-quantum cycle)
 *    - Critical sections for "I need to finish this without
 *      preemption" cases
 *    - Yields for "I have nothing more to do this turn"
 *    - Blocking recv for "wake me when there's work"
 *
 *  Priorities can be added later as a non-breaking extension
 *  (multiple ready bitmaps, one per priority level) if a real use
 *  case emerges. Don't preemptively add them now.
 *
 *  ---------------------------------------------------------------
 *  Adaptive quantum (critical-section debt)
 *  ---------------------------------------------------------------
 *
 *  Every VM has a configured baseline_quantum (instructions per
 *  turn under normal conditions). When a VM enters a critical
 *  section that runs past its quantum, the scheduler does NOT
 *  rotate — it re-calls vm_step until SYS_CRITICAL_EXIT (or a
 *  trap). The instructions consumed beyond the baseline accrue
 *  as debt on a per-VM counter.
 *
 *  On subsequent turns, the VM's effective quantum is reduced by
 *  up to half the baseline (so the per-turn penalty caps at 50%
 *  even for large debts). Debt drains linearly across turns until
 *  it hits zero, at which point the VM is back to baseline.
 *
 *  This trades a slightly longer recovery period for predictable
 *  fairness: a VM that does one big critical section pays for it
 *  over several turns rather than getting starved for one turn or
 *  hogging the system for one turn. The math:
 *
 *      next_quantum = baseline - min(debt, baseline / 2)
 *      debt_paid    = baseline - next_quantum
 *      new_debt     = debt - debt_paid
 *
 *  Runaway critical sections (guest bug, infinite loop inside
 *  ENTER/EXIT) are bounded by VmSchedConfig::max_critical_overrun.
 *  If a single critical section accumulates more than this many
 *  instructions past the baseline, the scheduler force-terminates
 *  the VM with TRAP_HALT. Set to UINT32_MAX to disable the cap
 *  (not recommended for unverified guest code).
 *
 *  ---------------------------------------------------------------
 *  Block management
 *  ---------------------------------------------------------------
 *
 *  When an ECALL handler sets cpu->block_reason to something other
 *  than BLOCK_NONE, the scheduler moves that VM from the ready set
 *  to the blocked set on the next scheduling pass. The blocked set
 *  is checked on every cycle for VMs whose wake condition is met:
 *
 *    BLOCK_YIELDED       — moved straight back to ready (no real
 *                          wait, just "skip this turn")
 *    BLOCK_MAILBOX_RECV  — woken when a message arrives in the
 *                          target mailbox OR block_deadline passes
 *    BLOCK_SLEEP         — woken when block_deadline passes
 *
 *  Mailbox-triggered wake-ups happen synchronously inside the
 *  SYS_SEND handler — it checks whether the recipient is blocked
 *  on BLOCK_MAILBOX_RECV and, if so, atomically delivers the
 *  message into the recipient's pending receive buffer, writes
 *  the success code to the recipient's a0, clears block_reason,
 *  and signals the scheduler to re-add it to the ready set.
 *  This is fine because we're single-host-threaded: no two
 *  handlers run concurrently.
 *
 *  Timeout wake-ups happen during the scheduler's between-quantum
 *  housekeeping: walk the blocked set, compare each deadline
 *  against the global tick counter, wake any that have expired
 *  with VM_ETIMEDOUT in their a0.
 *
 *  ---------------------------------------------------------------
 *  Capacity
 *  ---------------------------------------------------------------
 *
 *  Maximum number of VMs is VM_SCHED_MAX_VMS, default 64.
 *  Implemented as a uint64_t bitmap pair (ready, blocked) plus a
 *  fixed array of VmCpu pointers. VM IDs are dense, assigned by
 *  the scheduler at registration: the first registered VM gets
 *  vm_id 0, the second gets vm_id 1, etc. Caller can also request
 *  a specific vm_id via the _at variant.
 *
 *  64 VMs is plenty for the imagined workloads (28 threads with
 *  shell+supervisors+terminal infra) with room to grow. If you
 *  need more, redefine VM_SCHED_MAX_VMS (must remain ≤ 64 with
 *  the current single-uint64-bitmap implementation; ≤ 128 would
 *  need a two-word bitmap, etc.).
 *
 *  VM_SCHED_MAX_VMS must match VM_MAILBOX_WHITELIST_BITS — the
 *  mailbox whitelist bitmap and the scheduler ready bitmap need
 *  to address the same set of vm_ids. The default of 64 for both
 *  is consistent.
 *
 *  ---------------------------------------------------------------
 *  Idle detection
 *  ---------------------------------------------------------------
 *
 *  When the ready set is empty (all VMs blocked or halted), the
 *  scheduler is idle. The single-step API returns a distinct
 *  result code so the host can decide what to do — sleep the host
 *  CPU until an external event, poll, etc. The run-until API
 *  takes an idle callback that's invoked when the system would
 *  otherwise spin; default is "sleep for the shortest pending
 *  timeout duration".
 *
 *  ---------------------------------------------------------------
 *  Depends on: vm_core, vm_ecall
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_SCHED_H
#define VM_SCHED_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "vm/vm_core.h"
#include "vm/vm_ecall.h"

/* ============================================================
 *  Capacity
 * ============================================================ */

/* Maximum number of VMs the scheduler can hold. Must be <= 64
 * with the single-uint64 bitmap implementation. Must match
 * VM_MAILBOX_WHITELIST_BITS so mailbox whitelist bits and
 * scheduler ready bits address the same vm_id space. */
#ifndef VM_SCHED_MAX_VMS
#define VM_SCHED_MAX_VMS  64
#endif

#if VM_SCHED_MAX_VMS > 64
#error "VM_SCHED_MAX_VMS > 64 requires a multi-word bitmap implementation"
#endif

typedef uint64_t VmSchedBitmap;

/* ============================================================
 *  Trap-handler callback (caller-provided)
 *
 *  Invoked when a VM traps with anything other than TRAP_ECALL or
 *  TRAP_HALT. The handler can inspect cpu->trap_cause, trap_pc,
 *  trap_addr, trap_insn and decide what to do.
 *
 *  Return value semantics:
 *    VM_TRAP_TERMINATE — mark the VM halted, never run again
 *    VM_TRAP_RESUME    — clear the trap state and continue running
 *                        (caller should have fixed up cpu state
 *                        first if needed)
 *    VM_TRAP_LOG_AND_TERMINATE — same as TERMINATE but also bumps
 *                        the scheduler's "trapped_vms" counter
 *                        for diagnostics
 *
 *  The default handler (used when none is registered) is
 *  TERMINATE: any trap kills the VM, the other VMs keep running.
 *  This is the "fault isolation" default — one bad VM doesn't
 *  bring down the system.
 *
 *  Handler runs synchronously inside the scheduler's main loop.
 *  Keep it fast or shunt work to a supervisor VM via the mailbox.
 * ============================================================ */

typedef enum {
    VM_TRAP_TERMINATE = 0,
    VM_TRAP_RESUME,
    VM_TRAP_LOG_AND_TERMINATE,
} VmTrapAction;

typedef VmTrapAction (*VmTrapHandler)(VmCpu *cpu, void *system);

/* ============================================================
 *  Idle callback (caller-provided)
 *
 *  Invoked by vm_sched_run when the ready set is empty AND there
 *  are no imminent timeouts (next deadline > a few quanta away).
 *  The host can:
 *    - sleep the CPU (WFI on ARM, host-OS sleep)
 *    - poll external I/O sources that might unblock a VM
 *    - return quickly to let the scheduler check timeouts again
 *
 *  The callback receives the number of ticks until the next
 *  pending timeout (UINT32_MAX if no timeouts pending) so it can
 *  bound how long it sleeps. Returning normally hands control
 *  back to the scheduler, which checks timeouts and resumes.
 *
 *  The default (used when none is registered) is a tight return:
 *  the scheduler immediately re-checks timeouts and the ready
 *  set. This busy-loops; install a callback that sleeps on real
 *  hardware to actually save power.
 * ============================================================ */

typedef void (*VmIdleHandler)(uint32_t ticks_until_next_deadline,
                              void *system);

/* ============================================================
 *  Scheduler configuration
 * ============================================================ */

typedef struct {
    /* Baseline quantum: instructions per turn under normal
     * conditions. Smaller values give lower latency (faster
     * round-robin rotation) at the cost of higher dispatch
     * overhead. For a 28-VM workload aiming at <10ms response,
     * something like 1000-5000 is reasonable. */
    uint32_t baseline_quantum;

    /* Maximum instructions any single critical section is
     * allowed to consume past the baseline before the VM is
     * force-terminated. UINT32_MAX disables the cap (use only
     * with trusted guest code). Default suggestion: 10 *
     * baseline_quantum. */
    uint32_t max_critical_overrun;

    /* Trap handler. NULL means use the built-in default
     * (terminate the trapping VM). */
    VmTrapHandler trap_handler;

    /* Idle handler. NULL means use the built-in default
     * (tight return, busy-loop). */
    VmIdleHandler idle_handler;

    /* The ECALL router used by all VMs. The scheduler dispatches
     * ECALLs through this when vm_step returns VM_STEP_ECALL. */
    VmEcallRouter *ecall_router;

    /* === External tick source (optional) ===
     *
     * If non-NULL, the scheduler reads global_tick from this
     * callback instead of incrementing it by retired instructions
     * on each step. Use this to back ticks with a real-time
     * source — a 1 ms SysTick on a microcontroller, or
     * clock_gettime(CLOCK_MONOTONIC) on a PC.
     *
     * Behavior:
     *   - tick_source != NULL: global_tick = tick_source(userdata)
     *     refreshed on each scheduler step. ticks_per_second tells
     *     guests how to interpret tick deltas. SYS_TICK_HZ returns
     *     ticks_per_second.
     *   - tick_source == NULL: global_tick increments by the number
     *     of guest instructions retired on each step (today's
     *     behavior, abstract). SYS_TICK_HZ returns 0 (unknown).
     *
     * The callback should be fast — it's called from inside the
     * scheduler's hot path. A function that just reads a memory-
     * mapped counter (HAL_GetTick, DWT->CYCCNT) is ideal.
     *
     * The userdata pointer is passed through unchanged; useful if
     * the callback needs context (e.g., pointer to a TIM peripheral).
     */
    uint32_t (*tick_source)(void *userdata);
    void     *tick_source_userdata;
    uint32_t  ticks_per_second;

    /* Opaque pointer passed to all handler callbacks. Typically
     * the VmSystem struct from vm_system.h. */
    void *system;
} VmSchedConfig;

/* ============================================================
 *  Scheduler state
 *
 *  Caller declares one VmSched per system. The scheduler holds
 *  pointers to caller-owned VmCpu structs — it does not own them.
 *  The VmCpu storage is the caller's problem (typically static
 *  arrays or bump-allocated at boot).
 * ============================================================ */

typedef struct {
    /* === Public read-only counters === */
    uint64_t total_quanta_run;       /* total times vm_step has been called */
    uint64_t total_instructions;     /* sum of all VMs' retired counts */
    uint32_t global_tick;            /* tick counter for timeout deadlines */
    uint32_t trapped_vms;            /* count of VMs that ever trapped */
    uint16_t registered_count;       /* how many slots are in use */

    /* === Public read-only config snapshot === */
    VmSchedConfig config;

    /* === Internal — managed by the implementation === */

    /* Per-slot VM pointer. NULL if the slot is unused. */
    VmCpu *vms[VM_SCHED_MAX_VMS];

    /* Per-VM scheduling state. Indexed by vm_id. */
    uint32_t debt[VM_SCHED_MAX_VMS];                 /* unpaid overrun */
    uint32_t critical_section_consumed[VM_SCHED_MAX_VMS]; /* current section's overrun */

    /* Ready/blocked sets. A bit set in `ready` means vm[N] is
     * runnable; a bit set in `blocked` means vm[N] is paused.
     * Halted VMs have both bits clear. The two sets are disjoint
     * and their union is the registered-and-not-halted set. */
    VmSchedBitmap ready;
    VmSchedBitmap blocked;

    /* Round-robin cursor: vm_id of the next VM to schedule. The
     * scheduler advances from this position to find the next
     * ready bit, wrapping around. */
    uint16_t cursor;
} VmSched;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Initialize the scheduler. Copies the config in. Clears all
 * slots and counters. Sets the global tick to 0. */
void vm_sched_init(VmSched *s, const VmSchedConfig *cfg);

/* ============================================================
 *  VM registration
 *
 *  The scheduler does not own VmCpu storage. The caller declares
 *  VmCpu structs (statically, on a bump arena, however), calls
 *  vm_init + vm_load to set them up, then registers them with
 *  the scheduler.
 *
 *  Registration assigns the VM a vm_id (either the lowest free
 *  slot, or a caller-requested slot) and marks it ready. Once
 *  registered, vm_sched_run will start scheduling it.
 * ============================================================ */

/* Register a VM, picking the lowest free vm_id. The VmCpu's
 * vm_id field is overwritten with the assigned ID.
 *
 * Returns the assigned vm_id on success, or a negative error:
 *   -1: scheduler full (all VM_SCHED_MAX_VMS slots in use)
 *   -2: cpu is NULL
 *
 * The VM is added to the ready set; it will be scheduled on the
 * next vm_sched_step call. */
int vm_sched_register(VmSched *s, VmCpu *cpu);

/* Register at a specific vm_id (caller-chosen). Useful when
 * mailbox whitelists or message routing have pre-committed to
 * particular IDs.
 *
 * Returns 0 on success, negative on error:
 *   -1: requested_id out of range
 *   -2: requested_id already in use
 *   -3: cpu is NULL */
int vm_sched_register_at(VmSched *s, VmCpu *cpu, uint16_t requested_id);

/* Remove a VM from the scheduler. The VmCpu storage is not freed
 * (the caller still owns it). The slot becomes available for
 * future registrations. Safe to call on an already-unregistered
 * vm_id (no-op). */
void vm_sched_unregister(VmSched *s, uint16_t vm_id);

/* Look up a VM by ID. Returns NULL for unregistered IDs. */
VmCpu *vm_sched_get(const VmSched *s, uint16_t vm_id);

/* ============================================================
 *  Wake / block hooks
 *
 *  Called by ECALL handlers (specifically SYS_SEND) to atomically
 *  unblock a VM that was waiting on its mailbox. The scheduler
 *  clears block_reason on the target, writes the success code to
 *  the target's regs[a0], and moves the bit from `blocked` to
 *  `ready`.
 *
 *  The mailbox handler is responsible for actually delivering
 *  the message (copying the payload into the target's receive
 *  buffer); this function only handles the state transition.
 *  Returns false if vm_id isn't currently blocked on
 *  BLOCK_MAILBOX_RECV.
 * ============================================================ */

bool vm_sched_wake_mailbox(VmSched *s, uint16_t vm_id, int32_t a0_value);

/* Wake a parent VM blocked in SYS_SPAWN_AND_WAIT on a child that
 * has halted. Delivers a0_value (child exit code or -errno) and
 * transitions the parent blocked → ready. Returns false if vm_id
 * isn't currently blocked on BLOCK_ON_CHILD. */
bool vm_sched_wake_child(VmSched *s, uint16_t vm_id, int32_t a0_value);

/* Force a VM into the halted state. Removes from both bitmaps,
 * sets cpu->halted. Subsequent vm_step calls return HALTED
 * immediately. */
void vm_sched_halt(VmSched *s, uint16_t vm_id);

/* ============================================================
 *  Execution
 * ============================================================ */

/* Result codes from vm_sched_step. */
typedef enum {
    /* A quantum was run. May or may not have made progress
     * (a VM in a critical section that's also accruing debt
     * counts as running). */
    VM_SCHED_RAN = 0,

    /* No ready VMs. The scheduler did nothing. The caller may
     * want to poll for external events, sleep, or call
     * vm_sched_step again to re-check timeouts. */
    VM_SCHED_IDLE,

    /* All VMs are halted. The system is effectively done. */
    VM_SCHED_ALL_HALTED,
} VmSchedStepResult;

/* Run one scheduling cycle:
 *   1. Wake any blocked VMs whose deadlines have passed.
 *   2. If no VMs are ready, return VM_SCHED_IDLE (or ALL_HALTED).
 *   3. Pick the next ready VM (round-robin from cursor).
 *   4. Compute its effective quantum = baseline - min(debt, baseline/2).
 *   5. Call vm_step with that quantum.
 *   6. Handle the result:
 *      - ECALL: dispatch through router; update state per
 *        block_reason / halted / in_critical changes.
 *      - TRAP: invoke trap handler.
 *      - HALTED: remove from ready set.
 *      - QUANTUM_EXPIRED + in_critical: keep running (re-call
 *        vm_step with another baseline budget; accumulate debt).
 *      - QUANTUM_EXPIRED + not in_critical: advance cursor.
 *   7. Update global_tick and debt counters.
 *   8. Return VM_SCHED_RAN.
 *
 * One call to vm_sched_step does ONE scheduling decision plus its
 * follow-through (which may include multiple re-calls of vm_step
 * for a critical section). Use vm_sched_run for a continuous loop. */
VmSchedStepResult vm_sched_step(VmSched *s);

/* Run vm_sched_step in a loop until one of:
 *   - All VMs halted (returns true)
 *   - max_cycles cycles elapsed (returns false; pass 0 for unbounded)
 *
 * When idle, invokes the config's idle_handler (or busy-spins if
 * none is set). */
bool vm_sched_run(VmSched *s, uint64_t max_cycles);

/* ============================================================
 *  Introspection
 * ============================================================ */

/* Number of VMs currently in the ready set. */
static inline uint32_t vm_sched_ready_count(const VmSched *s) {
#if defined(__GNUC__) || defined(__clang__)
    return s ? (uint32_t)__builtin_popcountll(s->ready) : 0;
#else
    if (!s) return 0;
    VmSchedBitmap b = s->ready;
    uint32_t n = 0;
    while (b) { b &= b - 1; n++; }
    return n;
#endif
}

/* Number of VMs currently in the blocked set. */
static inline uint32_t vm_sched_blocked_count(const VmSched *s) {
#if defined(__GNUC__) || defined(__clang__)
    return s ? (uint32_t)__builtin_popcountll(s->blocked) : 0;
#else
    if (!s) return 0;
    VmSchedBitmap b = s->blocked;
    uint32_t n = 0;
    while (b) { b &= b - 1; n++; }
    return n;
#endif
}

/* Current debt for a VM (instructions owed to fairness). 0 for
 * a VM with no outstanding overrun, or for an unregistered id. */
static inline uint32_t vm_sched_debt(const VmSched *s, uint16_t vm_id) {
    if (!s || vm_id >= VM_SCHED_MAX_VMS) return 0;
    return s->debt[vm_id];
}

#endif /* VM_SCHED_H */
