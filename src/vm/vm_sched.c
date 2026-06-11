/* ============================================================
 *  vm_sched.c — cooperative scheduler implementation
 *  See vm/vm_sched.h for the public contract.
 *
 *  The scheduler holds an array of VmCpu pointers and a pair of
 *  64-bit bitmaps (ready/blocked) indicating which slots are in
 *  which state. On each step:
 *
 *   1. Wake any blocked VMs whose deadline has passed.
 *   2. Find the next ready VM starting from cursor.
 *   3. Compute that VM's effective quantum (baseline - debt cap).
 *   4. Run it via vm_step.
 *   5. Process the return code: dispatch ECALL, handle traps,
 *      advance cursor or re-loop for critical sections.
 *
 *  All scheduling state lives in the VmSched struct; the VmCpu
 *  storage is caller-owned (the scheduler holds only pointers).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_sched.h"
#include <string.h>

/* ============================================================
 *  Bitmap helpers — small inline ops on uint64_t.
 *
 *  We use plain shifts and masks rather than __builtin_ctzll
 *  for the find-first-set, since we want portable C and the
 *  inner loop calls these only once per scheduling step.
 * ============================================================ */

static inline void bm_set(VmSchedBitmap *b, uint16_t id) {
    *b |= ((uint64_t)1u << id);
}

static inline void bm_clear(VmSchedBitmap *b, uint16_t id) {
    *b &= ~((uint64_t)1u << id);
}

static inline bool bm_test(VmSchedBitmap b, uint16_t id) {
    return (b & ((uint64_t)1u << id)) != 0;
}

/* Find the next ready VM starting at `start`, wrapping around.
 * Returns the vm_id or -1 if no ready VM. */
static int find_next_ready(VmSchedBitmap ready, uint16_t start) {
    if (ready == 0) return -1;
    for (uint16_t i = 0; i < VM_SCHED_MAX_VMS; i++) {
        uint16_t id = (start + i) % VM_SCHED_MAX_VMS;
        if (bm_test(ready, id)) return (int)id;
    }
    return -1;
}

/* ============================================================
 *  Default callbacks
 * ============================================================ */

static VmTrapAction default_trap_handler(VmCpu *cpu, void *system) {
    (void)cpu; (void)system;
    return VM_TRAP_TERMINATE;
}

static void default_idle_handler(uint32_t ticks_until_next_deadline,
                                 void *system) {
    (void)ticks_until_next_deadline; (void)system;
    /* Tight return — busy-loop. Real hosts should provide a
     * callback that actually sleeps. */
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

void vm_sched_init(VmSched *s, const VmSchedConfig *cfg) {
    if (!s) return;
    memset(s, 0, sizeof(*s));
    if (cfg) {
        s->config = *cfg;
    }
    /* Fill in defaults for any unset callbacks. */
    if (!s->config.trap_handler) {
        s->config.trap_handler = default_trap_handler;
    }
    if (!s->config.idle_handler) {
        s->config.idle_handler = default_idle_handler;
    }
    /* Sane fallback for baseline if the caller left it 0. */
    if (s->config.baseline_quantum == 0) {
        s->config.baseline_quantum = 1000;
    }
    if (s->config.max_critical_overrun == 0) {
        s->config.max_critical_overrun = 10 * s->config.baseline_quantum;
    }
}

/* ============================================================
 *  Registration
 * ============================================================ */

int vm_sched_register(VmSched *s, VmCpu *cpu) {
    if (!s) return -1;
    if (!cpu) return -2;

    /* Find lowest free slot */
    for (uint16_t i = 0; i < VM_SCHED_MAX_VMS; i++) {
        if (s->vms[i] == NULL) {
            s->vms[i] = cpu;
            cpu->vm_id = i;
            bm_set(&s->ready, i);
            s->debt[i] = 0;
            s->critical_section_consumed[i] = 0;
            s->registered_count++;
            return (int)i;
        }
    }
    return -1;   /* full */
}

int vm_sched_register_at(VmSched *s, VmCpu *cpu, uint16_t requested_id) {
    if (!s) return -1;
    if (requested_id >= VM_SCHED_MAX_VMS) return -1;
    if (!cpu) return -3;
    if (s->vms[requested_id] != NULL) return -2;

    s->vms[requested_id] = cpu;
    cpu->vm_id = requested_id;
    bm_set(&s->ready, requested_id);
    s->debt[requested_id] = 0;
    s->critical_section_consumed[requested_id] = 0;
    s->registered_count++;
    return 0;
}

void vm_sched_unregister(VmSched *s, uint16_t vm_id) {
    if (!s) return;
    if (vm_id >= VM_SCHED_MAX_VMS) return;
    if (s->vms[vm_id] == NULL) return;   /* already gone */

    s->vms[vm_id] = NULL;
    bm_clear(&s->ready, vm_id);
    bm_clear(&s->blocked, vm_id);
    s->debt[vm_id] = 0;
    s->critical_section_consumed[vm_id] = 0;
    if (s->registered_count > 0) s->registered_count--;
}

VmCpu *vm_sched_get(const VmSched *s, uint16_t vm_id) {
    if (!s) return NULL;
    if (vm_id >= VM_SCHED_MAX_VMS) return NULL;
    return s->vms[vm_id];
}

/* ============================================================
 *  Wake / halt hooks
 * ============================================================ */

bool vm_sched_wake_mailbox(VmSched *s, uint16_t vm_id, int32_t a0_value) {
    if (!s) return false;
    if (vm_id >= VM_SCHED_MAX_VMS) return false;
    VmCpu *cpu = s->vms[vm_id];
    if (!cpu) return false;
    if (cpu->block_reason != BLOCK_MAILBOX_RECV) return false;
    if (!bm_test(s->blocked, vm_id)) return false;

    /* Transition: blocked → ready, clear block_reason, set a0. */
    cpu->block_reason = BLOCK_NONE;
    cpu->block_deadline = 0;
    cpu->regs[VM_REG_A0] = (uint32_t)a0_value;
    bm_clear(&s->blocked, vm_id);
    bm_set(&s->ready, vm_id);
    return true;
}

/* Wake a parent VM that was blocked in SYS_SPAWN_AND_WAIT on a
 * child that has now halted. Delivers `a0_value` (the child's exit
 * code, or a negative errno) into the parent's a0 and moves it
 * blocked → ready. Returns false if vm_id isn't a VM blocked on a
 * child (e.g. it was already woken, or never waited). */
bool vm_sched_wake_child(VmSched *s, uint16_t vm_id, int32_t a0_value) {
    if (!s) return false;
    if (vm_id >= VM_SCHED_MAX_VMS) return false;
    VmCpu *cpu = s->vms[vm_id];
    if (!cpu) return false;
    if (cpu->block_reason != BLOCK_ON_CHILD) return false;
    if (!bm_test(s->blocked, vm_id)) return false;

    cpu->block_reason   = BLOCK_NONE;
    cpu->block_deadline = 0;
    cpu->block_child_vm = UINT16_MAX;
    cpu->regs[VM_REG_A0] = (uint32_t)a0_value;
    bm_clear(&s->blocked, vm_id);
    bm_set(&s->ready, vm_id);
    return true;
}

unsigned vm_sched_wake_frame_consumed(VmSched *s, uint32_t now_consumed) {
    if (!s) return 0;
    unsigned woken = 0;
    VmSchedBitmap blocked = s->blocked;
    while (blocked) {
        uint16_t id = 0;
        VmSchedBitmap b = blocked;
        while ((b & 1u) == 0) { b >>= 1; id++; }
        blocked &= blocked - 1;

        VmCpu *cpu = s->vms[id];
        if (!cpu) continue;
        if (cpu->block_reason != BLOCK_FRAME_CONSUMED) continue;

        /* block_deadline stores the target frame_consumed value that
         * the wait should reach (typically g_frame_staged at wait
         * call time). Wake when consumed has caught up. */
        if (now_consumed >= cpu->block_deadline) {
            cpu->block_reason   = BLOCK_NONE;
            cpu->block_deadline = 0;
            cpu->regs[VM_REG_A0] = 0;
            bm_clear(&s->blocked, id);
            bm_set(&s->ready, id);
            woken++;
        }
    }
    return woken;
}

void vm_sched_halt(VmSched *s, uint16_t vm_id) {
    if (!s) return;
    if (vm_id >= VM_SCHED_MAX_VMS) return;
    VmCpu *cpu = s->vms[vm_id];
    if (!cpu) return;

    cpu->halted = true;
    bm_clear(&s->ready, vm_id);
    bm_clear(&s->blocked, vm_id);
}

/* ============================================================
 *  Timeout wakeup
 *
 *  Walk the blocked set, wake any whose deadline has passed.
 *  Called at the top of vm_sched_step.
 *
 *  For BLOCK_MAILBOX_RECV with timeout: deliver -ETIMEDOUT.
 *  For BLOCK_SLEEP: deliver 0 (clean wake).
 *  For BLOCK_YIELDED: wake immediately (no deadline check; YIELD
 *    is just "skip one round", which we model by moving the VM
 *    to blocked for one step pass through this function).
 *
 *  Returns the smallest non-zero number of ticks until the next
 *  unexpired deadline, or UINT32_MAX if all blocked VMs have no
 *  deadline (e.g., all are BLOCK_MAILBOX_RECV with no timeout).
 * ============================================================ */

/* Wraparound-safe "now has reached deadline" comparison.
 *
 * Treat now and deadline as unsigned 32-bit timestamps that can
 * wrap. The signed-subtract idiom is safe as long as the gap
 * between them is less than 2^31 ticks (≈24 days at 1 ms). For
 * any practical sleep duration that's never violated.
 */
static inline bool tick_reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static uint32_t wake_expired_timeouts(VmSched *s) {
    uint32_t next_deadline_delta = UINT32_MAX;
    VmSchedBitmap blocked = s->blocked;
    while (blocked) {
        /* Find lowest set bit */
        uint16_t id = 0;
        VmSchedBitmap b = blocked;
        while ((b & 1u) == 0) { b >>= 1; id++; }
        blocked &= blocked - 1;   /* clear that bit for next iter */

        VmCpu *cpu = s->vms[id];
        if (!cpu) continue;

        bool wake = false;
        int32_t a0_value = 0;

        switch (cpu->block_reason) {
        case BLOCK_YIELDED:
            /* Always wake — yielded VMs come back after one cycle. */
            wake = true;
            a0_value = 0;
            break;

        case BLOCK_SLEEP:
            if (tick_reached(s->global_tick, cpu->block_deadline)) {
                wake = true;
                a0_value = 0;
            } else {
                uint32_t delta = cpu->block_deadline - s->global_tick;
                if (delta < next_deadline_delta) next_deadline_delta = delta;
            }
            break;

        case BLOCK_MAILBOX_RECV:
            /* Only times out if block_deadline is non-zero. A
             * deadline of 0 means "wait indefinitely". */
            if (cpu->block_deadline != 0) {
                if (tick_reached(s->global_tick, cpu->block_deadline)) {
                    wake = true;
                    a0_value = -(int32_t)VM_ETIMEDOUT;
                } else {
                    uint32_t delta = cpu->block_deadline - s->global_tick;
                    if (delta < next_deadline_delta) next_deadline_delta = delta;
                }
            }
            break;

        case BLOCK_ON_CHILD:
            /* Event-driven, no timeout: the parent stays blocked
             * until its child halts, at which point the reap path
             * (vm_system_reap_halted_children) calls
             * vm_sched_wake_child. Nothing to do here — leave it
             * blocked and don't contribute a deadline. */
            break;

        case BLOCK_FRAME_CONSUMED:
            /* Event-driven, no timeout: stays blocked until the
             * cart_window port-7 read callback bumps frame_consumed
             * past block_deadline (the staged-at-commit value), at
             * which point vm_sched_wake_frame_consumed wakes us.
             * Don't contribute a deadline. */
            break;

        case BLOCK_NONE:
            /* Shouldn't be in blocked set with BLOCK_NONE, but
             * defensively wake it. */
            wake = true;
            break;
        }

        if (wake) {
            cpu->block_reason = BLOCK_NONE;
            cpu->block_deadline = 0;
            cpu->regs[VM_REG_A0] = (uint32_t)a0_value;
            bm_clear(&s->blocked, id);
            bm_set(&s->ready, id);
        }
    }
    return next_deadline_delta;
}

/* ============================================================
 *  Block transition (called after a quantum returns)
 *
 *  If the VM set block_reason during dispatch (via ECALL handler),
 *  move it from ready to blocked.
 *
 *  For BLOCK_YIELDED specifically, we still mark it blocked — the
 *  next call to wake_expired_timeouts will immediately wake it,
 *  which gives the "skip one round" semantics.
 * ============================================================ */

static void apply_block_transition(VmSched *s, uint16_t vm_id) {
    VmCpu *cpu = s->vms[vm_id];
    if (!cpu) return;
    if (cpu->block_reason == BLOCK_NONE) return;

    bm_clear(&s->ready, vm_id);
    bm_set(&s->blocked, vm_id);
}

/* ============================================================
 *  Trap handling
 * ============================================================ */

static void handle_trap(VmSched *s, uint16_t vm_id) {
    VmCpu *cpu = s->vms[vm_id];
    if (!cpu) return;

    VmTrapAction action = s->config.trap_handler(cpu, s->config.system);

    switch (action) {
    case VM_TRAP_RESUME:
        /* Clear the trap state and leave the VM in ready. The
         * caller's handler is responsible for fixing up cpu state
         * (PC, registers) to a valid resumable position. */
        cpu->trap_cause = TRAP_NONE;
        cpu->trap_pc    = 0;
        cpu->trap_addr  = 0;
        cpu->trap_insn  = 0;
        break;

    case VM_TRAP_LOG_AND_TERMINATE:
        s->trapped_vms++;
        /* fallthrough */
    case VM_TRAP_TERMINATE:
    default:
        cpu->halted = true;
        bm_clear(&s->ready, vm_id);
        bm_clear(&s->blocked, vm_id);
        break;
    }
}

/* ============================================================
 *  Effective quantum
 *
 *  baseline minus the cap-half-baseline portion of the debt.
 *  The portion paid is recorded so we can update the debt after
 *  the run.
 * ============================================================ */

static uint32_t effective_quantum(const VmSched *s, uint16_t vm_id,
                                  uint32_t *out_debt_paid) {
    uint32_t baseline = s->config.baseline_quantum;
    uint32_t cap = baseline / 2;
    uint32_t debt = s->debt[vm_id];
    uint32_t pay = debt < cap ? debt : cap;
    if (out_debt_paid) *out_debt_paid = pay;
    return baseline - pay;
}

/* ============================================================
 *  vm_sched_step — one scheduling decision
 * ============================================================ */

VmSchedStepResult vm_sched_step(VmSched *s) {
    if (!s) return VM_SCHED_ALL_HALTED;

    /* 0. If the host provides a wall-clock tick_source, sample it
     *    NOW — before waking timers — so deadlines are evaluated
     *    against current time even when no VM runs this step. Without
     *    this, global_tick only advanced inside the run-a-VM branch
     *    below, so when EVERY VM was blocked (e.g. a parked shell that
     *    spawned a child + a child sleeping between frames) the clock
     *    froze and sleeping VMs never woke — the classic "spawned TUI
     *    game renders its first frame then hangs" deadlock introduced
     *    once spawn became async (the parent no longer busy-runs).
     *    With no tick_source, ticks == retired instructions, which
     *    can only advance by running a VM, so there's nothing to do
     *    here in that mode. */
    if (s->config.tick_source) {
        s->global_tick = s->config.tick_source(s->config.tick_source_userdata);
    }

    /* 1. Wake any blocked VMs whose deadlines have passed. */
    (void)wake_expired_timeouts(s);

    /* 2. If no ready VMs, decide between IDLE and ALL_HALTED. */
    if (s->ready == 0) {
        if (s->blocked == 0) {
            /* No ready, no blocked. Either nothing registered, or
             * everything halted. */
            return s->registered_count == 0
                 ? VM_SCHED_ALL_HALTED   /* empty system */
                 : VM_SCHED_ALL_HALTED;
        }
        return VM_SCHED_IDLE;
    }

    /* 3. Pick the next ready VM (round-robin from cursor). */
    int next = find_next_ready(s->ready, s->cursor);
    if (next < 0) {
        /* Shouldn't happen given s->ready != 0, but be safe. */
        return VM_SCHED_IDLE;
    }
    uint16_t vm_id = (uint16_t)next;
    VmCpu *cpu = s->vms[vm_id];

    /* 4. Compute effective quantum. */
    uint32_t debt_paid = 0;
    uint32_t budget = effective_quantum(s, vm_id, &debt_paid);

    /* 5. Run.
     *
     * Critical-section loop: while the VM is in_critical AND
     * exited because the quantum expired (not because of trap,
     * halt, or ecall), call vm_step again with another full
     * baseline budget and accumulate debt. Bounded by
     * max_critical_overrun.
     */
    bool advance_cursor = true;

    for (;;) {
        uint32_t used = 0;
        VmStepResult r = vm_step(cpu, budget, &used);
        s->total_quanta_run++;
        s->total_instructions += used;

        /* Update global_tick. If a tick_source callback is set
         * (host has a real clock), read from it; otherwise fall
         * back to "ticks == retired instructions". */
        if (s->config.tick_source) {
            s->global_tick = s->config.tick_source(s->config.tick_source_userdata);
        } else {
            s->global_tick += used;
        }

        if (r == VM_STEP_QUANTUM_EXPIRED) {
            if (cpu->in_critical) {
                /* Track overrun against the cap. */
                s->critical_section_consumed[vm_id] += used;
                if (s->critical_section_consumed[vm_id]
                    > s->config.max_critical_overrun) {
                    /* Runaway. Force-halt as TRAP_HALT. */
                    cpu->halted = true;
                    cpu->trap_cause = TRAP_HALT;
                    bm_clear(&s->ready, vm_id);
                    s->critical_section_consumed[vm_id] = 0;
                    s->debt[vm_id] = 0;
                    break;
                }
                /* Keep running this VM with a full baseline budget;
                 * each iteration past the first accrues debt. */
                budget = s->config.baseline_quantum;
                /* Accrue debt for the just-completed (extra) budget. */
                s->debt[vm_id] += used;
                continue;
            }
            /* Normal quantum expiry — pay debt down. */
            if (debt_paid <= s->debt[vm_id]) {
                s->debt[vm_id] -= debt_paid;
            } else {
                s->debt[vm_id] = 0;
            }
            /* If we just exited a critical section in this turn,
             * also clear its consumed counter. */
            if (!cpu->in_critical) {
                s->critical_section_consumed[vm_id] = 0;
            }
            break;
        }

        if (r == VM_STEP_ECALL) {
            /* Dispatch through the router. */
            if (s->config.ecall_router) {
                vm_ecall_dispatch(s->config.ecall_router, cpu,
                                  s->config.system);
            } else {
                /* No router — return -ENOSYS directly. */
                cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_ENOSYS);
            }
            /* After dispatch, the handler may have set halted,
             * block_reason, in_critical, etc. Re-check and act. */
            if (cpu->halted) {
                bm_clear(&s->ready, vm_id);
                bm_clear(&s->blocked, vm_id);
                break;
            }
            if (cpu->block_reason != BLOCK_NONE) {
                apply_block_transition(s, vm_id);
                break;
            }
            /* Otherwise resume in this same quantum if budget
             * remains. The VM continues running. */
            if (used >= budget) {
                /* Quantum used up; pay debt and move on. */
                if (debt_paid <= s->debt[vm_id]) {
                    s->debt[vm_id] -= debt_paid;
                } else {
                    s->debt[vm_id] = 0;
                }
                break;
            }
            budget -= used;
            continue;
        }

        if (r == VM_STEP_HALTED) {
            bm_clear(&s->ready, vm_id);
            bm_clear(&s->blocked, vm_id);
            break;
        }

        /* VM_STEP_TRAPPED */
        handle_trap(s, vm_id);
        /* Don't advance cursor if the trap handler resumed — the
         * VM is still at this slot. But if it terminated, we
         * should still advance to avoid spinning on a dead slot.
         * Either way, advance_cursor remains true; ready is
         * already updated by handle_trap. */
        break;
    }

    /* 6. Advance the cursor to the next slot for the next call. */
    if (advance_cursor) {
        s->cursor = (uint16_t)((vm_id + 1) % VM_SCHED_MAX_VMS);
    }

    return VM_SCHED_RAN;
}

/* ============================================================
 *  vm_sched_run — looped driver
 * ============================================================ */

bool vm_sched_run(VmSched *s, uint64_t max_cycles) {
    if (!s) return true;

    uint64_t cycles = 0;
    while (max_cycles == 0 || cycles < max_cycles) {
        VmSchedStepResult r = vm_sched_step(s);
        cycles++;

        if (r == VM_SCHED_ALL_HALTED) return true;
        if (r == VM_SCHED_IDLE) {
            /* Look ahead to find the next deadline */
            uint32_t delta = wake_expired_timeouts(s);
            s->config.idle_handler(delta, s->config.system);
            /* With an external tick source, the host's clock
             * advances on its own — we just resample on the next
             * scheduler step. Without one, we bump global_tick by
             * 1 so SLEEP timeouts can eventually fire even when
             * no VM is running. */
            if (!s->config.tick_source) {
                s->global_tick++;
            } else {
                s->global_tick = s->config.tick_source(s->config.tick_source_userdata);
            }
        }
    }
    return false;
}
