# Preemptive scheduler — platform port contract (host & MCU)

What the preemptive scheduler needs from the platform, as a small set of
hooks, so that the **same scheduler core** runs on a hosted OS (for
development/validation) and on the bare-metal target (STM32H745) with
only these hooks differing. This is the "proper hook consideration for
microcontroller implementation": the MCU port is *implement these hooks*,
not *rewrite the scheduler*.

This document is the **contract / spec**. It is grounded in the platform
seam conventions the codebase already uses (so a porter sees one
consistent pattern), and it is the interface the Step 3 rework (see
`scheduler-step3-rework.md`) should build the scheduler against. The
proposed C interface is included below as the spec — it is **not yet
wired into `presched.c`**; the current committed scheduler (Steps 1–2)
still has its timer setup inline and will be refactored onto these hooks
when the rework lands. Marking this clearly so the interface is not
mistaken for live, exercised code.

## Two seam styles already in the codebase (be consistent with these)

**Pattern 1 — caller-supplied struct of function pointers + ctx**
(`SlabLocker` in `memory/slab_stack.h`). `lock()` returns a `uintptr_t`
of saved state (PRIMASK on Cortex-M, a mutex token on host); `unlock()`
restores it. Used for per-instance, caller-injected behavior. The
scheduler's **critical-section locker** uses exactly this shape — reuse
what you already wrote for the allocator.

**Pattern 2 — compile-selected free functions** (`vm/host_platform.h`:
`host_platform_monotonic_ms`, `host_platform_sleep_ms`,
`host_platform_request_stop`). One declaration, one implementation per
platform selected at compile time. Used for global per-platform
primitives: the scheduler's **systick source**, **task/context
primitive**, and **idle/WFI**.

## The four seams

### A. Systick source (pattern 2)
A periodic tick that drives time-slicing and wakes sleeping tasks.

- **host (POSIX):** a `timer_create` periodic timer raising `SIGRTMIN`,
  consumed by a **dedicated scheduler thread** via `sigwait` (NOT a
  signal handler running in an arbitrary task thread — that was the
  deadlock bug; see the rework doc). The scheduler thread runs the tick
  logic.
- **host (Win32):** a timer thread that periodically runs the tick logic
  and Suspend/Resumes task threads.
- **MCU:** the SysTick IRQ (or a hardware timer IRQ) at the tick period.
  The ISR runs the tick bookkeeping and requests a PendSV to perform the
  actual context switch at a safe point.

Contract: `start(period_us)` begins ticks and returns success; `stop()`
ceases them (no tick callback after it returns); the tick must reach the
scheduler's bookkeeping/switch path **without depending on which task is
currently running** receiving it.

### B. Task context (pattern 2)
Create a runnable task and switch the CPU between tasks.

- **host:** a task is an OS **thread**; "switch" is gate/park/resume
  (post the next task's run-gate semaphore/event, park the current on
  its own). The host has no real register-frame switching — the OS does
  it. Preempting a *running* task mid-body uses a directed signal
  (`pthread_kill`, POSIX) or `SuspendThread` (Win32).
- **MCU:** a task is a **stack + saved register frame**. `create`
  initializes an exception-return stack frame so the task "returns" into
  its entry. "switch" is a **PendSV** handler that saves the outgoing
  task's registers to its stack, loads the incoming task's, and
  exception-returns into it — the standard Cortex-M cooperative/preemptive
  context switch. The SysTick ISR sets PendSV-pending; PendSV (lowest
  priority) does the switch when no higher ISR is active.

Operations: `create(entry, arg, stack, stack_size)`; `yield_to(to)`;
`park()`; `resume(task)`. On host these wrap threads+gates; on MCU they
wrap stack-frame init + PendSV. The exact signatures live in the core's
per-platform section; this is the conceptual contract.

### C. Critical-section locker (pattern 1 — `PreschedLocker`, SlabLocker shape)
Brief mutual exclusion around the scheduler's own O(1) bookkeeping (the
ready set, the current-task pointer, the priority bitmap).

```c
typedef struct {
    uintptr_t (*lock)(void *ctx);
    void      (*unlock)(void *ctx, uintptr_t saved);
    void      *ctx;
} PreschedLocker;
extern const PreschedLocker presched_null_locker;  /* single-threaded */
```

- **host:** wrap a mutex (lock returns 0, takes the mutex; unlock
  releases). Or, since the bookkeeping is O(1) and the dedicated
  scheduler thread already serializes most of it, a lightweight scheme.
- **MCU:** interrupt-disable —
  ```c
  static uintptr_t m_lock(void *c){ (void)c;
      uintptr_t pm = __get_PRIMASK(); __disable_irq(); return pm; }
  static void m_unlock(void *c, uintptr_t pm){ (void)c; __set_PRIMASK(pm); }
  ```
  Justified because the protected region is provably O(1)/bounded, so
  interrupt latency stays bounded — the same argument that makes the slab
  allocator's interrupt-disable locker acceptable.

### D. Idle / WFI (pattern 2)
What to do when no task is runnable, to wait efficiently until the next
tick/event.

- **host:** a short sleep (e.g. `nanosleep` for a fraction of the tick).
- **MCU:** `__WFI()` — wait-for-interrupt, low power until the next
  SysTick/IRQ wakes the core. This is also where the idle task's purpose
  goes once the dedicated-scheduler-thread model owns ticks: on the MCU
  the SysTick IRQ fires regardless of WFI, so a sleeping task's wake is
  driven by the ISR, and an explicit idle *task* may be replaced by the
  idle *hook* (WFI in the scheduler's no-runnable-task branch). Re-decide
  during the rework (the host already showed the idle task may be
  removable once a scheduler thread owns ticks).

## Proposed C interface (the spec the rework implements)

```c
/* presched_port.h (to be created BY the rework, wired into presched.c) */
typedef struct PreSched PreSched;

/* C. locker — pattern 1 */
typedef struct {
    uintptr_t (*lock)(void *ctx);
    void      (*unlock)(void *ctx, uintptr_t saved);
    void      *ctx;
} PreschedLocker;
extern const PreschedLocker presched_null_locker;

/* A. systick source — pattern 2 */
bool presched_port_systick_start(PreSched *s, unsigned period_us);
void presched_port_systick_stop(PreSched *s);
void presched_tick(PreSched *s);   /* core's tick entry; the port calls it */

/* D. idle — pattern 2 */
void presched_port_idle(void);

/* B. task context — pattern 2, signatures finalized when the MCU port is
 *    built; host wraps threads+gates, MCU wraps stack-frame + PendSV. */
```

## What is verified where (honesty boundary)
- **host (POSIX):** implementable and validated in-sandbox (the rework's
  scheduler-thread model is proven — see the rework doc's reference
  proof).
- **host (Win32):** structurally the timer-thread model; compile-checked,
  user-verified (same boundary as the audio `channel_win32` path).
- **MCU (Cortex-M / STM32H745):** this document is the **contract**. The
  SysTick-ISR + PendSV context switch, the PRIMASK locker, and `__WFI()`
  idle are standard Cortex-M, but are **not compiled or tested in this
  environment** — the MCU author implements and verifies on hardware.
  The value here is that the seams are placed so that port is bounded and
  follows the codebase's existing conventions, not a rewrite.

## Port checklist (MCU)
1. Provide the SysTick (or timer) ISR that calls `presched_tick()` and
   sets PendSV-pending.
2. Implement the PendSV handler: save/restore task register frames.
3. Implement task-context `create` (init the stack frame to enter
   `entry(arg)` with a clean xPSR/LR for exception return).
4. Supply a `PreschedLocker` using PRIMASK (above).
5. Implement `presched_port_idle()` as `__WFI()`.
6. Set tick period, priorities (SysTick mid, PendSV lowest), and stack
   sizes per task.
