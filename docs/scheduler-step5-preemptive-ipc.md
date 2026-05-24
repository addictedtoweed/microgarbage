# Step 5 item #2 (Part B) — blocking IPC on the preemptive backend

Part A made `vm_system` scheduler-agnostic via the `VmSchedOps` seam (the
cooperative ops are a 1:1 forward to `vm_sched_*`). Part B fills the seam's
preemptive side so a `VmSystem` runs each VM as a `presched` task and
re-expresses blocking mailbox recv on the preemptive primitives — closing
the integration gap the audit (`scheduler-step5-ecall-audit.md`) describes.

## What changed

- **`vm_sched_ops_pre.c` + `vm_pre.h` (`VmPreCtx`).** The preemptive
  `VmSchedOps`. Each VM is registered as a `presched` task whose body
  (`vm_pre_task_body`) drives `vm_step` and routes ECALLs through
  `vm_ecall_dispatch` **in that VM's own thread**. `now` reads
  `presched_total_ticks`; `wake_mailbox` is `presched_wake(task_of(vm))`;
  `run` is `presched_run`. The context holds the `PreSched`, the
  vm_id↔task_id map, and a per-mailbox mutex.
- **`handle_recv` (preemptive branch).** Runs in the VM's task thread, so
  instead of setting `block_reason` it loops: try the (locked)
  `vm_mailbox_recv`; if empty, `presched_block` (no timeout) or the new
  `presched_block_timeout` (with one), delivering into its **own** dest on
  wake. No cross-VM memory write.
- **`handle_send` (preemptive branch).** Drops the cooperative
  synchronous-delivery fast path (which writes another VM's memory — unsafe
  when that VM may be running concurrently). Always enqueues under the
  mailbox lock, then `presched_wake`s the receiver, which pulls the message
  in its own thread.
- **Real mailbox locker.** Under preemption each mailbox gets a pthread
  mutex via `vm_mailbox_set_locker` (the seam from item #1); the
  cooperative build keeps the zero-cost null locker.
- **Real slab locker (audit item #3).** Under preemption the shared/local
  slabs are initialized with a real mutex `SlabLocker` (a `VmSystem`-owned
  `pthread_mutex_t`, since `slab_init` takes the locker by value) so
  concurrent `SYS_ALLOC`/`SYS_FREE` across VM task threads is safe; the
  cooperative build keeps `slab_null_locker`. Same mutex-seam pattern as
  the mailbox; reuses the same lock/unlock callbacks.
- **`presched_block_timeout` (new primitive).** A *sticky*, deadline-bounded
  park — `presched_block` (no lost wake) plus a wake deadline. Needed
  because a blocking recv-with-timeout must neither lose a racing send
  (ruling out the non-sticky `presched_sleep`) nor busy-poll (which starves
  the sender under round-robin). Added to both the POSIX and Windows paths.

All preemptive code is behind `#if GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE`
(see `config.h`); the cooperative (default) build links none of it.

Known caps / still deferred (per the audit): one task per VM, so at most
`PRESCHED_MAX_TASKS` live VMs. **Spawn-and-wait** is not preemption-safe —
`presched` adds all tasks before `presched_run`, so runtime VM spawn needs a
`presched` extension (dynamic task creation); `pre_wake_child` is a stub.
**FS concurrency** (`SYS_OPENAT`/`READ`/`WRITE` against the host FS) is not
guarded and is documented not-yet-concurrent-safe (also gated on FatFs,
which isn't vendored). Both are the audit's lower-priority items.

## The proof (build & run)

The cooperative `examples/03_mailbox` (a producer that `SYS_SEND`s 0..9 and
a consumer that blocking-`SYS_RECV`s them) is also the preemptive IPC proof:
build it in preemptive mode and the same guests now exchange messages via
`presched_block`/`presched_wake` instead of the cooperative fast path.

```
# from repo root, with mingw-w64 + the build env (see the toolchain note)
gcc -std=c11 -Wall -Wextra -pedantic -DGARBAGE_SCHED_MODE=1 -I include \
    -o examples/03_mailbox/build/host_pre.exe \
    examples/03_mailbox/host.c \
    src/vm/vm_core.c src/vm/vm_loader.c src/vm/vm_ecall.c \
    src/vm/vm_ecall_handlers.c src/vm/vm_mailbox.c src/vm/vm_sched.c \
    src/vm/vm_sched_ops_coop.c src/vm/vm_sched_ops_pre.c src/vm/vm_system.c \
    src/vm/vm_host_stdio.c src/vm/vm_host_stdio_win32.c \
    src/vm/vm_host_platform.c src/vm/vm_host_tui.c src/vm/presched.c \
    src/memory/bump.c src/memory/slab_stack.c \
    src/containers/fifo_queue.c src/containers/ring_buffer.c -lpthread
( cd examples/03_mailbox && ./build/host_pre.exe )   # expect: consumer got 0..9, done
```

## Validation boundary (honest)

- **Cooperative (default, regression):** `test_vm_system` 33/0 unchanged;
  `03_mailbox`, `06_scheduler`, `07_vm_task` pass. The seam is a 1:1
  transcription and the preemptive code is inert here.
- **Preemptive:** functionally verified on **native Windows** (mingw-w64,
  Win32 `SuspendThread` preemption) — all ten messages delivered in order
  via blocking recv, no deadlock, clean exit. This is the same
  user-verified boundary as `presched.c`'s Windows path and
  `channel_win32.c`.
- **Not done here:** TSan validation of the preemptive IPC under the POSIX
  timer-signal backend (mingw on Windows has no ThreadSanitizer). The race
  freedom rests on the design (single-thread VM ownership; the locked
  mailbox queue, whose locker seam is the subject of the item-#1
  `vm_mailbox_locker_tsan_proof.c`) and should be confirmed under TSan in
  the POSIX sandbox — the project's standard honesty boundary: POSIX/TSan
  validates the logic, Windows/MCU are user-verified.
- **Slab locker:** installed and exercised by the `03_mailbox` run, but
  those guests don't `SYS_ALLOC`, so concurrent allocation is not *stressed*
  here. It is the same mutex seam as the mailbox locker; a concurrent-alloc
  guest under TSan (POSIX sandbox) would be the direct proof.
