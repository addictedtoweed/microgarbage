# 08_rtos_demo — several RV32 guests running concurrently

Where `07_vm_task` ran **one** VM as a preemptive task (alongside a
native task), this runs **several guest VMs at once**. Each guest ELF is
loaded into its own `VmCpu` and added to the preemptive scheduler as a
task; the 1 ms systick time-slices them, so their output **interleaves** —
a tiny "RTOS" running multiple real VMs concurrently.

It builds on two earlier examples:
- `06_scheduler` — the preemptive scheduler over native tasks.
- `07_vm_task` — a single VM *as* a task (a VM task and a native task are
  identical to the scheduler; see `docs/execution-model.md` §7.2).

The guests are built from one `guest.c` (three times, `-DGUEST_NAME=…`)
against the **shared guest SDK** (`examples/common/guest/`): `vm_runtime`
supplies `_start`, and `puts()` routes to the host over `SYS_WRITE`. The
host stays tiny — it routes just the two ecalls the demo uses (`SYS_WRITE`
and `SYS_EXIT`) in each VM's own task thread.

## Build & run

```sh
./build.sh run
```

You'll see the three guests interleave, e.g.:

```
=== 3 guest VMs, preemptively time-sliced (1 ms systick) ===
[A] working
[B] working
[C] working
[A] working
[C] working
[B] working
...
[A] done
[B] done
[C] done
=== all guests exited; context switches=… ===
```

(Exact ordering varies run to run — that's the point: the systick, not
the guests, decides who runs when.)

Run with your own guest ELFs:

```sh
(cd build && ./rtos_demo path/to/g1.elf path/to/g2.elf)
```

## Notes

- This is the preemptive scheduler driving multiple VMs directly
  (`presched` + bare `VmCpu`s) — the same shape the full `05_shell` host
  uses under `GARBAGE_PREEMPTIVE=1`, distilled to its essence.
- Extending it to shared resources (mailbox IPC between the guests,
  mixing audio across them) is the natural next step — they already share
  one address-space-isolated scheduler.
