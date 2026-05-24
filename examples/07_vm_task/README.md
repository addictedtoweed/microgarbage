# 07_vm_task — a real RV32 VM running as a preemptive task

This is **Step 4** of the preemptive scheduler (see
`docs/execution-model.md` §7.9): the point where the VM and the scheduler
meet. A real RV32 guest runs as a *preemptive task* — the scheduler's
systick interrupts it mid-execution and time-slices it against a native
task, with no cooperation from the guest.

The key idea (§7.2): to the scheduler, a **VM task and a native task are
identical** — both are "a thread the systick preempts." A VM task is just
a task whose body drives a `VmCpu` (it loops `vm_step`); the scheduler
neither knows nor cares that the thread is interpreting RV32. So Step 4
adds **no scheduler changes** — it drops the VM onto the proven scheduler
from Steps 1–3.

What the demo shows:

- A compute-bound RV32 guest (`guest.c`) that never yields — it just
  computes a deterministic sum and exits with a known code (64). Because
  it never yields, only *preemption* lets anything else run.
- A native task running concurrently. The two interleave in the output,
  proving the VM is genuinely preempted and time-sliced.
- The guest still computes the correct result (exit 64) despite being
  preempted mid-execution — the `VmCpu` is owned by exactly one task
  thread (never shared), the single-thread-ownership invariant the VM
  core requires.

ECALLs are handled **in the VM task's own thread** (here, just `SYS_EXIT`)
— which is how blocking syscalls and IPC will work in later steps, and
why ECALL handlers touching shared state need real concurrency safety
(Step 5's audit).

## Build & run

```
./build.sh run
```

`guest.elf` is checked in; it's rebuilt only if a RISC-V cross-compiler
(`riscv64-unknown-elf-gcc` / `riscv-none-elf-gcc`) is present. The host
needs a C compiler and, on Linux, `-lpthread -lrt` (added automatically);
native Windows uses Win32 threads (build with mingw).

## Where this sits

Steps 1–3 built the scheduler (round-robin, priority, block/wake/sleep).
Step 4 (this) runs VM tasks on it. Step 5 is services + integration
(audio under preemption, the service-exclusion invariant, the ECALL
concurrency audit). The same scheduler core targets the microcontroller
via the hooks in `docs/scheduler-mcu-port.md`.
