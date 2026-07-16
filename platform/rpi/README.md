# platform/rpi — Raspberry Pi port (bare-metal ARM)

The Pi port's home. A complete bare-metal stack for the Pi Zero (`raspi0`
/ ARM1176): the microgarbage RV32IMC VM runs with **no OS**, hosting a
RISC-V guest that drives real hardware. It boots as a genuine
`kernel.img` — **no semihosting** — and every layer is verified in QEMU:

- **first light** — the RV32 interpreter runs a RISC-V guest on the ARM CPU
- **cache page table** — MMU-on flat identity map, L1 I/D caches
- **PL011 UART** — real serial output
- **mailbox framebuffer** — 640×480 from the VideoCore GPU
- **GPIO** — the guest drives real BCM2835 pins through the hwio ecalls

It's the same VM and the same guest `.elf` that run on the desktop
(`examples/11_run`, `examples/10_hwio_sim`) — only the host shim differs.
That is the "develop on the dev kit, deploy the same binary to the micro"
story from `docs/rpi-port.md`, proven on the target architecture.

## Build & run

Requires `arm-none-eabi-gcc` (+ newlib), a RISC-V cross (for `tools/rvcc`),
and `qemu-system-arm`.

```sh
./build.sh run      # build build/kernel.{elf,img} and boot it in QEMU
```

`build/kernel.img` is the raw binary a real Pi boots from an SD card.
Because it halts (rather than "exits") when done, QEMU keeps running —
end it with Ctrl-A X, or `timeout N qemu-system-arm -M raspi0 -cpu arm1176
-nographic -kernel build/kernel.elf`.

Expected output:

```
== microgarbage on bare-metal ARM (raspi0 / arm1176) ==
uart: PL011 @0x20201000 up (this text is real serial)
mmu: identity map on  SCTLR=0x0085187d  M=1 C=1 I=1
host: RV32IMC interpreter, no OS.  guest: 1196-byte ELF
--- guest output ---
  hello from the RV32 guest (interpreted on ARM)
  fib: 0 1 1 2 3 5 8 13 21 34
  sum(1..100) = 5050
  gpio17: write 1 -> reads 1
  gpio17: toggle  -> reads 0
--- guest halted cleanly ---
--- framebuffer (mailbox property interface) ---
fb: 640x480 x32bpp  base=0x1c100000 pitch=2560 size=1200KB
fb: readback bar0=0xffffff bar5=0xff0000 (expect ffffff, ff0000)
== a RISC-V guest ran on an ARM CPU. first light. ==
(bare-metal kernel.img -- no semihosting -- halting)
```

## How it fits together

- **`boot.S`** + **`kernel.ld`** + **`syscalls.c`** → the bare-metal
  startup: entry at 0x8000 sets the stack, zeros `.bss`, calls `main`;
  the linker script lays the image out; the syscall stubs (`_sbrk` bump
  heap, `_exit` halt, inert file stubs) replace rdimon so nothing depends
  on a semihosting host. `main` returning halts the core (`wfi`).
- **`guest_hello.c`** → compiled by **`tools/rvcc`** to a RISC-V ELF,
  embedded as a byte array (no filesystem yet). Prints, allocs, and pokes
  GPIO through the portable SDK headers.
- **`main.c`** → the host: static VM pools, `vm_system_init`, install
  platform (`printf` via `SYS_FORMAT_AND_WRITE`) + hwio, load + run the
  guest, then the framebuffer demo. Built from the **portable cooperative
  VM subset** only (no win32/pthread/tui/fs/audio; `vm_host_stdio` omitted
  — its TTY path needs `termios`).
- **`mmu.c`** → ARMv6 **flat identity map + L1 I/D caches**. Not virtual
  memory — a 1:1 VA==PA map for memory *attributes* (WB-WA for RAM so it
  caches, device for the peripheral window). ARM1176 is uncached without
  the MMU on; this is the interpreter's biggest perf lever.
- **`uart.c`** → BCM2835 **PL011 UART0** (0x2020_1000). The C library's
  `_write` is routed here, so `printf` is real serial (verified: bytes
  land on QEMU's serial line with CR-LF).
- **`fb.c`** → **VideoCore mailbox framebuffer** (property interface,
  channel 8): one tagged message sets size + depth, allocates, reads the
  pitch; draws color bars. (`fb_dump_ppm` can dump the buffer over
  semihosting for a headless visual check — unused in the bare build.)
  Message buffer is cache-cleaned around the GPU handoff (real-HW correct;
  no-op under QEMU).
- **`platform_rpi.c`** → the real BCM2835 backend behind `host_hwio.h`
  (the desktop `platform_hwio_sim.c` is its blueprint), linked with
  `vm_host_hwio.c` + installed via `vm_host_install_hwio`. GPIO is
  implemented + verified; I2C/SPI/ADC/PWM return `-ENOSYS` pending
  real-hardware bring-up.

## Roadmap (see docs/rpi-port.md)

**DONE (all verified on QEMU raspi0):** first light · cache page table
(`SCTLR` M=C=I=1) · PL011 UART (serial line captured to file) · mailbox
framebuffer (8 bars sampled from the PPM) · real GPIO (guest→registers→
readback) · **bare-metal boot** (`boot.S`, no semihosting, real
`kernel.img`).

**Next:** **I2C / SPI / PWM** — real BCM2835 drivers, written and verified
against attached devices (the temp-sensor dev-kit workflow; QEMU models
neither the buses nor slaves). BCM2835 has no ADC.

**Honest caveats (QEMU is functional emulation):**
- No cache-*timing* model, so the page table is verified **correct**
  (map valid, MMU genuinely on) but the cache **speedup** is a
  real-hardware property this can't show.
- The framebuffer is verified by **CPU readback** (coherent with our own
  cached writes). On real hardware the FB wants a non-cacheable mapping
  (or a post-draw cache clean) for the GPU to see the pixels.
- A minimal boot: no exception-vector table yet, so a fault would run off
  the rails — fine for this non-faulting demo, a refinement for real HW.
