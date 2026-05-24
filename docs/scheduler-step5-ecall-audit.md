# Step 5 — ECALL concurrency audit (preemptive VM tasks)

Step 4 made VM tasks real: a `VmCpu` runs in its own task thread,
preempted by the systick, concurrently with other VM and native tasks.
That changes a load-bearing assumption. This audit records what is safe
and unsafe when ECALL handlers run **concurrently across task threads**,
grounded in the current code, and sketches the fix — *before* writing the
concurrency-sensitive changes, because the affected modules (mailbox,
ecall handlers) are also used by the working cooperative `vm_sched` and
must not regress.

## The load-bearing assumption (now violated)

`vm_ecall.h` states it plainly:

> Single host thread by design. Only one handler runs at a time, across
> all VMs in the system. Handlers may freely read and write shared state
> (slab allocator, mailbox list, scheduler queues) without locking.

`vm_mailbox.h` likewise: *"No locking inside this module."*

This is **true and fine under cooperative `vm_sched`** — it runs exactly
one VM at a time in one host thread, so no two handlers ever overlap. It
is **false under the preemptive backend** (Step 4): VM tasks run in
separate threads, so `SYS_SEND` in VM-A's thread and `SYS_RECV` in VM-B's
thread can execute simultaneously and race on shared state.

This is not a bug in the existing code — it is a correct design for the
cooperative model. It is an *integration gap* that the preemptive model
introduces and Step 5 must close.

## Findings by syscall category

**Safe under preemption (no shared mutable state):**
- Pure CPU/self syscalls: `SYS_SELF`, `SYS_EXIT`, register/CPU-only ops.
  They touch only the calling VM's own `VmCpu`, which is owned by exactly
  one task thread. (Step 4 exercised `SYS_EXIT` — clean, TSan-clean.)
- Read-only stats snapshots are *mostly* safe but can read torn values
  (`SYS_SLAB_STATS`, `SYS_VM_STATS`) — acceptable for diagnostics, not
  for control flow.

**Unsafe under preemption (shared mutable state, no locking):**
- **Mailbox IPC — `SYS_SEND` / `SYS_RECV` / `SYS_MAILBOX_INFO`.** The
  prime offender. `handle_send` has a *synchronous-delivery fast path*
  that resolves the **target** VM's saved dest pointer through the
  target's region map and writes the payload directly into it — a
  cross-thread write into another running VM's memory/registers. The
  mailbox queues have no locks. Concurrent send/recv on the same mailbox
  races.
- **Blocking recv uses the wrong mechanism.** `handle_recv` sets
  `cpu->block_reason = BLOCK_MAILBOX_RECV` — the *cooperative*
  scheduler's blocking signal, which `vm_sched` interprets between
  quanta. The preemptive backend doesn't read `block_reason`; it has its
  own `presched_block`/`presched_wake`. So a blocking recv under
  preemption would not actually block correctly.
- **Slab allocator shared across VMs** (`SYS_*` that allocate, spawn).
  The slab has a locker seam (`SlabLocker`) but the system currently
  installs `slab_null_locker` for the single-threaded assumption.
  Concurrent allocation from VM tasks needs a real locker.
- **Spawn/wait — `SYS_SPAWN_AND_WAIT`** touches the VM table and
  scheduler queues; cross-thread under preemption.
- **Filesystem syscalls** (`SYS_OPENAT`/`READ`/`WRITE`/… against the host
  FS / trashdrive) — shared host state, not concurrency-safe as written.

## Fix design (for when this is built — not yet)

The integration is a real piece of work; sketch, not code:

1. **Mailbox thread-safety.** Give the mailbox a locker seam exactly like
   `SlabLocker` (a `MailboxLocker`: lock/unlock + ctx; PRIMASK on MCU,
   mutex on host, null for the cooperative build so it stays zero-cost).
   Guard send/recv/whitelist with it. Keep the cooperative path using the
   null locker so it is unchanged and unregressed.
2. **Blocking recv via preemptive block/wake.** Under the preemptive
   backend, `handle_recv` on an empty mailbox should call
   `presched_block` (the calling task parks); `handle_send`'s delivery
   should `presched_wake` the receiver. This *unifies* IPC blocking with
   the Step 3 mechanism — the natural, powerful result. The cooperative
   backend keeps using `block_reason`. The handler picks the mechanism by
   config (`GARBAGE_SCHED_MODE`) or via a small scheduler-agnostic
   block/wake indirection.
3. **Slab locker.** Install a real `SlabLocker` (mutex on host, PRIMASK on
   MCU) when `GARBAGE_SCHED_PREEMPTIVE`; keep `slab_null_locker` for
   cooperative. The slab already supports this — it is a config choice,
   not new code.
4. **Synchronous-delivery fast path.** The cross-VM direct write in
   `handle_send` must happen under the target mailbox's lock, and must be
   safe against the target running concurrently. Simplest correct form:
   under preemption, drop the fast path and always go through the locked
   queue + wake; keep the fast path only for the cooperative build.
5. **Filesystem / spawn.** Lower priority (a game/app may not need
   concurrent FS from multiple VM tasks initially). Audit and lock per
   need; document as not-yet-concurrent-safe until done.

## Services (the other half of Step 5)

Audio is a **service**, not a task: it lives outside the scheduler, paced
by its device/DMA clock, and must **never** be suspended by the systick
(the service-exclusion invariant, §7.3 / §2.3). Under the preemptive
backend the audio thread must not be a registered task. The audio channel
is already **lock-free SPSC**, so a VM task producing audio commands and
the audio service consuming them is already safe *by construction* —
which is why audio was built that way. This is the one piece of shared
state that is *already* preemption-safe; the mailbox is the one that is
not.

## Recommended build order for Step 5

1. **Mailbox locker seam** (mirror `SlabLocker`); cooperative path uses
   the null locker (zero change). Verify cooperative tests still pass.
2. **Preemptive block/wake recv** wired into `handle_recv`/`handle_send`
   under the preemptive config.
3. **Slab locker** under the preemptive config.
4. **A VM-IPC-under-preemption example/test** (two VM tasks exchanging
   messages) — the proof, analogous to 07_vm_task.
5. Services-under-preemption (audio thread excluded from the task set;
   the channel is already safe) and the service-exclusion invariant test.
6. FS/spawn concurrency as needed.

Each step keeps the cooperative `vm_sched` path unchanged (it uses null
lockers / `block_reason` as today). The honesty boundary is unchanged:
POSIX validates the logic in-sandbox under TSan; the MCU lockers (PRIMASK)
and real-time behavior are user-verified.

## Status

This is the audit only — no concurrency-sensitive code changed. Steps 1–4
(scheduler through VM tasks) are committed and working. Step 5's
mailbox/locker work is the next build, and it is deliberately *not* rushed
into the shared mailbox module without the care it needs, since that
module also backs the working cooperative scheduler and audio paths.
