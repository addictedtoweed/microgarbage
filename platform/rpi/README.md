# platform/rpi — Raspberry Pi port (bare-metal ARM)

The Pi port's home. Today it is **first light**: the microgarbage RV32IMC
VM running on bare-metal ARM (Pi Zero / `raspi0` / ARM1176) with no OS
underneath, hosting a RISC-V guest ELF. A RISC-V VM on an ARM CPU.

Output is via **ARM semihosting**, so it runs in QEMU with no UART or
framebuffer bring-up. This is the same VM and the same guest `.elf` that
run on the desktop (`examples/11_run`) — only the host shim differs. That
is the "develop on the dev kit, deploy the same binary to the micro"
story from `docs/rpi-port.md`, proven on the target architecture.

## Build & run

Requires `arm-none-eabi-gcc` (+ newlib/rdimon), a RISC-V cross (for
`tools/rvcc`), and `qemu-system-arm`.

```sh
./build.sh run
```

Expected output:

```
== microgarbage on bare-metal ARM (raspi0 / arm1176) ==
host: RV32IMC interpreter, no OS.  guest: 1056-byte ELF
--- guest output ---
  hello from the RV32 guest (interpreted on ARM)
  fib: 0 1 1 2 3 5 8 13 21 34
  sum(1..100) = 5050
--- guest halted cleanly ---
== a RISC-V guest ran on an ARM CPU. first light. ==
```

## How it fits together

- **`guest_hello.c`** → compiled by **`tools/rvcc`** to a RISC-V ELF,
  embedded into the image as a byte array (no filesystem yet).
- **`main.c`** → the bare-metal host: static VM pools, `vm_system_init`,
  install the platform layer (`printf` via `SYS_FORMAT_AND_WRITE`), load
  + run the embedded guest. Guest `printf` → semihosting → console.
- Built from the **portable, cooperative VM subset** only — no
  win32/pthread/tui/fs/audio sources. `vm_host_stdio` is omitted (its
  TTY-raw path needs `termios`, which bare metal lacks); `printf` goes
  through the platform formatter instead.
- Runtime output uses newlib **`rdimon`** semihosting; the eventual real
  hardware floor swaps this for a UART / framebuffer.

## Roadmap (see docs/rpi-port.md)

This is Phase 0 (semihosting). Next, in order:
1. **UART** output (BCM2835 PL011) — real serial instead of semihosting.
2. **Flat cache page table** — MMU-on identity map for L1/L2 (ARM1176
   is uncached without it; the interpreter wants cache).
3. **Mailbox framebuffer** — first pixels on a TV (the real MVP).
4. **`platform_rpi.c`** — the real GPIO/I2C/SPI/ADC backend behind the
   `host_hwio.h` seam (mirrors `src/host/platform_hwio_sim.c`), and the
   PWM audio path.

The interpreter runs uncached here (no page table yet), so it is not
representative of real-hardware speed — that lands with step 2.
