# Preemptive scheduler — Step 3 status & rework plan

Status as of the Step 2 commit (`feat(vm): preemptive scheduler —
priority dimension`). This file is the handoff: it records a real bug
found during Step 3, the root cause, and a **fix architecture already
proven in isolation**, so a fresh context (a new session, or Claude
Code) can continue from the repo alone.

See `docs/execution-model.md` §7 for the overall preemptive-scheduler
design. This file is specifically about the Step 3 (block/wake/sleep)
rework.

## What works and is committed
- **Step 1** (`presched.c`): bare round-robin preemption, single
  priority level, native tasks. True mid-body preemption. TSan-clean.
- **Step 2**: fixed-priority preemptive, round-robin within a level,
  strict priority. 7 tests, TSan-clean.

Both use this mechanism: a periodic POSIX timer raises `SIGRTMIN`; the
signal handler runs **in whatever thread the signal is delivered to**
and acts only if that thread is the current task — it parks itself and
releases the next task's gate.

## The bug (found in Step 3, not yet fixed)
Step 3 added `presched_block` / `presched_wake` / `presched_sleep` plus
an internal idle task. Integrated, it **deadlocks** in the
`test_lone_sleeper_wakes` case: a single real task that sleeps with no
other ready real task. (A sleeper *plus* another ready task passes —
which is why an early single-scenario test missed it; the full suite,
run in sequence, caught it.)

### Root cause (demonstrated empirically)
The Step 1/2 mechanism assumes "the tick handler runs in the current
task's thread." But a **process-directed** POSIX timer signal
(`SIGEV_SIGNAL` + `SIGRTMIN`) is delivered to an **arbitrary** thread
that has the signal unblocked — and empirically it lands on a *parked*
(`sem_wait`) thread, **not** the running/idle one. A standalone test
showed ~23 deliveries to the parked thread and **0** to the busy/idle
thread over the same interval.

Consequence: when all real tasks are blocked and only the idle task is
"running," the tick's sleep-deadline wake-scan runs on the wrong
(parked) thread, which returns early (`current != self`), so **sleepers
never wake → deadlock.** The idle task does not fix this, because the
problem is *which thread receives the signal*, not *whether a thread is
alive*.

## The fix — proven in isolation (works; sleeper wakes at exactly tick=50)
Replace "signal handler runs in a task thread" with a **dedicated
scheduler thread**:

1. **Block `SIGRTMIN` in every thread** (`pthread_sigmask(SIG_BLOCK,…)`),
   and have **one dedicated scheduler thread consume ticks via
   `sigwait(SIGRTMIN)`.** Now the tick is handled *deterministically* by
   exactly one known thread — no random delivery.
2. The scheduler thread does **all** time bookkeeping (tick counter,
   sleep-deadline wake-scan) and **drives preemption**: to preempt the
   currently-running task it sends a **directed** signal
   `pthread_kill(runner_thread, SIGUSR1)` (whose handler just makes that
   task park on its gate), then `sem_post`s the next task's gate.
3. Task threads **no longer handle the tick at all.** They run between
   gate grants; the directed `SIGUSR1` is their "park yourself" nudge.

Requires `_GNU_SOURCE` (or the right feature macro) for `sigwait` /
`pthread_kill`. Keep the `EINTR` retry loops around `sem_wait`.

A complete, working standalone proof of this architecture was built and
verified during the session (a lone sleeper wakes at exactly its
deadline; no deadlock). Rebuild the module from this shape — do **not**
try to salvage the stashed WIP, which is the flawed signal-in-task-thread
version.

## Integration scope (this touches the Step 1/2 mechanism too)
- `presched_run`: spawn a scheduler thread; set `SIGRTMIN` blocked in all
  threads + install a trivial `SIGUSR1` (park) handler.
- Remove the preemption logic from the old `systick_handler`; move the
  wake-scan + task selection into the scheduler-thread loop.
- `presched_block` / `presched_wake` / `presched_sleep`: the
  state-machine logic is fine (set state, hand off, park; sticky wake to
  avoid lost wakeups; sleep sets a deadline). The *time-driven* wakes and
  preemption now come from the scheduler thread.
- **Re-evaluate the idle task:** its only purpose was "keep a thread
  alive to receive ticks." Once a dedicated scheduler thread owns ticks,
  that thread is *always* alive to fire time-based wakes — so the idle
  task may be **unnecessary** now. Check whether it can be removed
  (simplification), or whether it's still wanted as an always-runnable
  handoff target when all real tasks block. Likely removable.
- The **Windows path** (SuspendThread/ResumeThread, compile-checked only)
  also needs the equivalent: a timer thread is already the right shape
  there — it Suspend/Resumes task threads directly, which is the natural
  Windows analogue of the scheduler-thread model. Bring it in line and
  add the Step 3 block/wake/sleep on that side too (still user-verified,
  not sandbox-runnable).

## Re-verify after the rework (the bar for "Step 3 done")
- Full `test_presched` suite **including `test_lone_sleeper_wakes`** (the
  hang case) — all green.
- **TSan clean** (remember: TSan instruments signal handlers imperfectly,
  so signal-path cleanliness is strong-evidence-not-proof; the directed-
  signal model has *less* signal surface than the old one, which helps).
- Stress: 20× repeat + a fast-tick (≈100µs) multi-task run, no deadlock
  (124 = hang).
- Only then is it the "mostly working" push point for Steps 1–3.

## Working discipline that kept catching these bugs
Prove each tricky concurrency mechanism in a **small isolated probe
first**, then build the module around the proven shape. This caught the
original preemption design, the block/wake lost-wakeup, and this
signal-delivery deadlock. A fresh agent will not inherit this caution
automatically — apply it deliberately.
