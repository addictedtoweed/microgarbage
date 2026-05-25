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

Known caps: one task per VM, so at most `PRESCHED_MAX_TASKS` live VMs. With
the FS work below, Step 5's preemption-safety items (mailbox, scheduler seam,
blocking IPC, slab, spawn, FS) are all addressed.

## FS concurrency under preemption (audit FS item)

`vm_host_fs.c` keeps process-global state (the fd table, the mount table),
so peer VM tasks doing file syscalls concurrently would
race. A single coarse FS mutex (gated on `GARBAGE_SCHED_MODE`; a no-op inline
that pulls in no pthread dependency under cooperative) now serializes every FS
entry point via thin `lk_` trampolines: the six direct handlers (`openat`,
`close`, `lseek`, `mkdirat`, `unlinkat`, `readdir`), the three stdio fd-hooks
(`fs_read_fd`/`fs_write_fd`/`fs_close_fd`), and the three public
`vm_host_fs_route_*` transport entry points. The handlers themselves are
unchanged (the trampolines keep their many early-returns and the lock scope
obviously correct). `SYS_TTY_SET_RAW` is not wrapped (it touches no FS state),
and `SYS_SPAWN_AND_WAIT` locks **only** its path-resolve + ELF-load — it must
drop the lock before parking on the child, which may itself take the FS lock.

**Validated:** both modes compile `-Wall -Wextra -pedantic`; the cooperative
shell still builds; the preemptive spawn test (which loads the child ELF
through the now-locked FS path) still runs cleanly with no deadlock. Like the
slab locker, concurrent FS from multiple peer VM tasks is not *stress*-tested
here (it'd want a two-VM file-I/O guest under TSan in the POSIX sandbox); the
serialization is correct by construction (one mutex around all global FS
state) and cooperative behavior is unchanged (no-op lock).

## Spawn-and-wait under preemption (audit spawn item)

`SYS_SPAWN_AND_WAIT` (in `vm_host_fs.c`) loads a child ELF and, since "Round
V", runs it *asynchronously*: the cooperative path registers the child as a
scheduler VM, parks the parent (`BLOCK_ON_CHILD`), and the reap loop in
`vm_system_step` wakes the parent with the child's exit code. Three things
made that not work under preemption, now addressed:

1. **Runtime task creation.** `presched` only built task threads in
   `presched_run`, so a child registered mid-run never started. `presched`
   now supports adding a task while running (atomic `n_tasks` + a `running`
   flag; `presched_add_task` creates+integrates the thread itself and
   publishes `n_tasks` last). Proven in isolation by `test_presched_spawn.c`.
2. **Parent actually waits.** Under preemption the spawn handler (in the
   parent's own task thread) sets the `block_child_vm` marker, then loops:
   check the child's `halted` flag, else `presched_block`. It reads the
   child's exit code from the child and unloads it. Checking `halted` before
   each park + sticky `presched_block` closes the race where a fast child
   exits before the parent parks (no lost wake, no deadlock).
3. **Child wakes the parent.** `vm_pre_task_body`, when its VM halts, finds a
   parent waiting on it (`block_child_vm`) and `presched_wake`s that task.

**Validated** end-to-end on native Windows: a parent guest spawning
`/host/hello.elf` under the preemptive backend — the child runs as its own
dynamically-created task, prints, exits; the parent reaps the exit code and
resumes; clean shutdown, no deadlock. Reproduce with the preemptive shell
(`GARBAGE_PREEMPTIVE=1 ./build-win.sh`, then `run /host/hello.elf` from a real
terminal) or a minimal host that mounts `host_files/` and loads a spawning
parent guest. (`test_vm_host_fs.c`'s spawn test drives the syscall directly
without a running scheduler, so it covers the cooperative path only.)

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
