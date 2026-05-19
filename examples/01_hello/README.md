# Example 01: Hello, VM

The simplest possible embedding: load one guest, run it, see its output.

## What this demonstrates

- Setting up a `VmSystem` with caller-provided memory pools
- Installing the optional `vm_host_stdio` bridge so guest
  `SYS_WRITE` reaches host stdout
- Loading a guest ELF from disk
- Running the scheduler until the guest calls `SYS_EXIT`

The guest program itself is six lines of actual code — a single
`sys_write` call followed by `sys_exit`.

## Build and run

```
./build.sh run
```

Expected output:

```
01_hello: compiling host...
01_hello: compiling guest (RV32IMC)...
01_hello: built. To run:
    .../examples/01_hello/build/host

01_hello: ===== running =====
Hello from inside the VM!
```

## Files

- `host.c` — host application (~80 lines, mostly setup boilerplate)
- `guest.c` — guest program (~25 lines)
- `build.sh` — compiles both, optionally runs

## What's next

- **02_counter** — guest yields cooperatively; demonstrates
  scheduling
- **03_mailbox** — two guests cooperating via inter-VM messages
- **04_keydump** — raw-mode terminal input
