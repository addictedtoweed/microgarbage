# 06_scheduler — preemptive scheduler demo (native tasks)

Unlike examples 01–05, which run guest RV32 code on the VM, this example
demonstrates the **native preemptive scheduler** directly — no guest, no
cross-compiler. It's a host program that creates several native tasks and
lets the scheduler time-slice them.

It shows the two behaviors the committed scheduler provides (Steps 1–2 of
the preemptive backend; see `docs/execution-model.md` §7):

1. **Preemptive round-robin** — four equal-priority tasks interleave
   their progress (execution order like `1302130213…`), driven by a
   periodic systick that preempts the running task, rather than running
   one task to completion at a time.

2. **Strict priority** — a high-priority task runs to completion before a
   low-priority task makes any progress (execution order `99999` then
   `11111`). The scheduler always runs the highest-priority ready task;
   avoiding starvation of low-priority work is the task designer's job
   (here the high task simply finishes).

3. **Block / wake / sleep** — a consumer task blocks until a producer
   signals it (`presched_block` / `presched_wake`), and a third task
   sleeps for a fixed number of systicks (`presched_sleep`) and wakes on
   time. Demonstrates tasks leaving and re-entering the ready set.

This is the full preemptive backend through Step 3 (see
`docs/execution-model.md` §7 and `docs/scheduler-step3-rework.md`).

## Build & run

```
./build.sh run
```

Requires a C compiler and, on Linux, POSIX threads + realtime timer
(`-lpthread -lrt`, added automatically). On native Windows the scheduler
uses Win32 threads (`SuspendThread`/`ResumeThread`); build with the mingw
toolchain — the demo's scheduler logic is the same, the preemption
primitive differs per platform.

## Platform / porting

This is the host (POSIX) backend. The same scheduler core is intended to
run on the microcontroller (e.g. the STM32H745) via the platform hooks
specified in `docs/scheduler-mcu-port.md` — the systick source, the
task/context primitive, the critical-section locker (PRIMASK), and the
idle/WFI hook. Those MCU hooks are a documented contract, not built or
tested here.
