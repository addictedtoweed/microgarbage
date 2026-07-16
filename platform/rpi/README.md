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
- **`uart.c`** → BCM2835 **PL011 UART0** driver (0x2020_1000). The C
  library's `_write` is routed here, so `printf` is **real serial**
  (verified: bytes land on QEMU's serial line with CR-LF). newlib
  `rdimon` semihosting is now used only for startup / clean QEMU exit /
  the heap — a real-hardware build swaps that for a boot.S + halt loop.
- **`fb.c`** → **VideoCore mailbox framebuffer** (property interface,
  channel 8). One tagged message sets physical/virtual size + depth,
  allocates the buffer, and reads the pitch; then it draws color bars
  and can dump the framebuffer to a PPM via semihosting (headless visual
  proof). Message buffer is cache-cleaned around the GPU handoff (no-op
  under QEMU; correct on real HW).
- **`platform_rpi.c`** → the real BCM2835 backend behind `host_hwio.h`
  (the desktop `platform_hwio_sim.c` is its blueprint). Linked with
  `vm_host_hwio.c` (the ecall handlers) and installed via
  `vm_host_install_hwio`. GPIO is implemented + verified; I2C/SPI/ADC/PWM
  return `-ENOSYS` pending real-hardware bring-up.

- **`mmu.c`** → ARMv6 **flat identity map + L1 I/D caches** (`mmu_enable`).
  Not virtual memory — a 1:1 VA==PA map whose only job is memory
  attributes: normal write-back/write-allocate for RAM (so it caches),
  device for the 0x2000_0000 peripheral window. ARM1176 is uncached
  without the MMU on, so this is the interpreter's biggest perf lever.

## Roadmap (see docs/rpi-port.md)

Phase 0, semihosting output.

**DONE:**
- **flat cache page table** (`mmu.c`) — MMU-on ARMv6 identity map, L1 I/D
  caches. Verified in QEMU: `SCTLR` M=C=I=1, no translation faults.
- **PL011 UART** (`uart.c`) — real serial output, verified by capturing
  QEMU's serial line to a file (CR-LF proves our driver, not semihosting).
- **Mailbox framebuffer** (`fb.c`) — 640×480×32 requested from the GPU,
  color bars drawn, all 8 verified by sampling the dumped PPM.
- **`platform_rpi.c` GPIO** — a RISC-V guest drives real BCM2835 GPIO
  through the hwio ecalls (config/write/read/toggle, verified in QEMU);
  the identical calls hit the desktop sim in `examples/10_hwio_sim`.

Next, in order:
1. **boot.S + halt** — a proper startup that drops semihosting so it
   boots as a real `kernel.img` on hardware.
2. **I2C/SPI/PWM** — real BCM2835 drivers, written + verified against
   attached devices (the temp-sensor dev-kit workflow; QEMU models
   neither the buses nor slaves). BCM2835 has no ADC.

Note: QEMU is *functional* emulation with no cache-timing model, so the
page table is verified **correct** here (map valid, MMU genuinely on);
the cache **speedup** is a real-hardware property this can't show.
