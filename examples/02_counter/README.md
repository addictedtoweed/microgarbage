# Example 02: Counter

A guest that prints an incrementing counter forever, yielding the
CPU between iterations.

## What this demonstrates

- **`SYS_YIELD`**: the guest voluntarily gives up the rest of its
  quantum after each line. With only one VM registered this just
  means the scheduler comes back around immediately, but the
  primitive is the same one a multi-VM system uses for cooperative
  scheduling (see 03_mailbox).
- **A manual scheduler loop**: instead of `vm_system_run` (which
  blocks until all VMs halt or a cycle cap is hit), the host calls
  `vm_system_step` in a loop and checks a SIGINT flag between
  steps. This is the pattern any long-running embedding will use.
- **Number formatting without libc**: the guest is freestanding
  (no `<stdio.h>`), so it includes a small `format_u32` helper.

## Build and run

```
./build.sh
./build/host
```

The counter will run forever. Stop it with `Ctrl-C`:

```
host: running counter. Ctrl-C to stop.
counter: 0
counter: 1
counter: 2
...
^C
host: SIGINT received, stopping.
```

## Performance note

This runs roughly half a million counter iterations per second on
a modern x86 host, with the per-iteration cost dominated by:

1. The RV32IMC instructions inside the format loop (interpreted)
2. The SYS_WRITE ECALL dispatch
3. Host-side `fwrite` to stdout
4. The SYS_YIELD ECALL dispatch

For programs less I/O-heavy than this, the dispatcher itself is
the bottleneck — expect a few million instructions per second per
guest on a desktop host.
