# Raspberry Pi Zero / Zero W port — design

Direction locked 2026-07-14 (design conversation; no code yet). A bare-metal
port of the microgarbage OS to the Pi Zero / Zero W as a **fast-boot kiosk /
costume platform** and an **integrated on-device dev kit**. Same RV32IMC VM,
same guest apps, a new hardware floor underneath. See memory [[rpi-zero-port]].

Audience: whoever brings up the `platform/rpi` target, and anyone deciding
where the native/shim boundary sits. Read end-to-end before touching
`vm_ecall.h`.

## Why

Two motivations, one architecture.

1. **Costumes / kiosks.** John builds interactive costumes that want video,
   sound, GPIO, and instant-ish on (a few seconds cold). The Pi Zero gives a
   TV-out framebuffer, PWM audio, and GPIO with no OS in the way.
2. **CRA / SBOM collapse.** The pitch is "one shimmed firmware for the whole
   OS around the RISC-V apps." Replacing a full Linux stack with a tiny fixed
   firmware + sandboxed guests **collapses** the sprawling-Linux SBOM/CVE-churn
   problem into a small, stable, auditable surface. It does **not** eliminate
   the SBOM obligation — it makes the SBOM maintainable and keeps app churn out
   of the certified floor.

Both reduce to the same organizing principle: **minimize the native trusted
computing base.** This is the microkernel / capability lineage (seL4 user-space
drivers, QNX resource managers). The OS already has the capability primitive it
needs — the whitelist-only, receiver-opens mailbox (`SYS_WHITELIST_ADD` 1075,
`SYS_SEND` 1072 rejects non-whitelisted senders with `-EPERM`). Privilege rings
are a possible future add.

## The three layers

```
  RV32 guest apps          <- same ELFs everywhere the VM runs
  --------------------
  shim / ecall boundary    <- SYS_* : stable ABI, this is the contract
  --------------------
  hardware floor           <- allowed to be driver-ish; per-target; may be non-CC0
  --------------------
  BCM2835 silicon
```

The line John drew: **the floor is allowed to be as driver-ish as the silicon
forces** (boot, cache, SDIO, USB, mailbox, PWM). Everything above it is shims /
ecalls to the VM. "No drivers" was always about not growing a general-purpose
*feature*-driver model — one contained backend under an existing abstraction is
the opposite of that.

### Drivers as VM processes

A driver's *control plane* can run as a sandboxed RV guest with only low-level
native hooks; the *hot bus path* stays in the floor. WiFi is the worked
example (below): association / config / policy in a guest, SDIO burst DMA + IRQ
+ firmware upload in the floor, bulk buffers handed over the existing
[[stream-arbiter]] / backend-op ring pattern. Control in the VM, DMA in the
floor. Not driverspace.

## Target hardware realities (Pi Zero / Zero W, BCM2835)

Things that shape the plan, in order of how much they matter.

1. **You don't own cold boot — the VideoCore does.** The GPU runs first
   (`bootcode.bin` / `start.elf` from the SD FAT partition), then launches your
   ARM `kernel.img`. That closed blob is ~1-2 s before your code executes.
   "A few seconds" cold is realistic; sub-second is not without hacking GPU
   firmware. Trim with `boot_delay=0`, `disable_splash=1`, `start_cd.elf`,
   `gpu_mem=16`, minimal `config.txt`. **Don't init WiFi on the boot path.**
2. **"No MMU" on ARM1176 secretly means "no caches."** L1/L2 caches only work
   with the MMU enabled and a page table present (attributes are per-page). Run
   truly MMU-off and everything is uncached — the interpreter loop and any
   framebuffer blit crawl. Fix: a **trivial flat 1:1 page table used only for
   memory attributes** (normal-cacheable RAM, device-nGnRnE for MMIO,
   non-cacheable for the framebuffer region). No virtual remap, no protection,
   no per-process spaces — "not using the MMU" in the sense that matters, while
   still getting cache. **This is the single biggest perf lever; do it first.**
3. **Framebuffer is the easy win.** Request a linear framebuffer via the
   **mailbox property interface** — driverless, ~a page of code, HDMI +
   composite. The PPU / r3d staging output writes straight into it.
4. **PWM audio** on the GPIO/headphone path, fed from the mixer's staging
   buffer. Small, solved. (HDMI audio is harder — skip.)
5. **GPIO** — direct MMIO, trivial. This is most of the costume interactivity.
6. **Storage** — SD via the EMMC controller (ARM owns it after boot). trashfs
   RAM disk lives in the 512 MB RAM (enormous headroom for the "large temp
   trashdrive"); persistent/external is SD FAT via the existing `/host`-style
   bridge (see [[filesystem-direction]]). Same two FS roles, verbatim.
7. **USB (DWC OTG)** — complex; deferred (see below). The Zero has **no
   built-in hub**, so one device on the OTG port unless you add an external hub.
8. **WiFi (Zero W)** — CYW43438 on the second SD controller as SDIO. The
   biggest single chunk of the port (see below).

## Networking — WiFi is distributed mailbox IPC

WiFi is for **communication between interactive elements** (costume ↔ costume),
not internet. So it is not a general network stack — it is the VM's mailbox IPC
extended across the wire. A costume addresses another costume the same way a VM
task addresses a sibling task locally. The guest ABI does not change; only the
transport's far end differs.

Consequences:

- **UDP broadcast / multicast, not TCP.** No connections, no per-peer state —
  right for "announce state / fire an event to the group." Configure lwIP for
  UDP only; skip the TCP surface.
- **One node runs SoftAP** (CYW43 supports it); everyone else joins + multicast
  among them. Ad-hoc / IBSS on this Broadcom part is historically unreliable —
  do not design on it. Alternative: carry a pocket travel router. Decide
  star-around-one-node vs. external router early — it affects whether every
  node needs AP-capable init or just station.

Implementation: vendor the proven stack rather than write it cold — **pico-sdk
`cyw43-driver` + lwIP**. Reuse their control/data layers; re-implement only the
**SDIO bus shim** (Pico W is gSPI to the chip; Zero W is SDIO — the bus HAL
differs, the WHD/control layers don't). The Broadcom **firmware blob
(`brcmfmac43430-sdio.bin` + NVRAM `.txt`) ships on the SD partition** and is
uploaded at runtime, exactly like `start.elf` — data on a card, not a linked
artifact.

## USB host — "if possible", deferred

Second-biggest chunk after WiFi. Treat as a later phase, not an MVP gate. Two
use cases:

- **CDC-ACM** (serial gadgets) → a class driver over the host stack; maps to
  another transport backend.
- **Mass storage (thumb drive)** → MSC class driver **+ a FAT32 driver** —
  which is exactly the "SD is FAT32, its own world / external-storage bridge"
  role already scoped. A thumb drive is that same world on a different bus.

The maturity spike that decides scope: **TinyUSB (MIT) host mode on
dwc2 / BCM2835.** TinyUSB is the permissive host stack, but its host side on
the Pi's controller is less battle-tested than its device side. That check
decides "vendor a stack" (easy) vs. "port/write the HCD" (a project). Spike it
before promising CDC/MSC. Use **Circle (GPLv3) as reading/reference only** — it
is the most complete bare-metal Pi USB/WiFi reference in existence, but
*linking* it makes the whole firmware GPLv3.

## Licensing model

- **Core stays CC0.** The VM, shims, trashfs, mixer, r3d, the whole above-floor
  world never links vendored code.
- **Permissive** (MIT/BSD — `cyw43-driver`, lwIP, TinyUSB) is vendored into a
  restored `third_party/` (bring back the `setup_licenses.sh` / manifest
  pattern that was purged), with each component's upstream LICENSE intact and an
  SPDX/`THIRD_PARTY.md` roster. It links **only into the Pi target's floor.**
- **GPL** (Circle, GCC) is used as **tools / reference, never linked** into the
  shipped firmware.
- **Firmware blobs** ship on the SD partition, not compiled in.

Net: the CC0 streak is preserved *as a property of the core*; the non-CC0
surface is a fenced, per-target, license-tracked corner of the floor. That same
segregated manifest **is the raw material for the CRA SBOM** — segregating for
license hygiene is also how you generate the attestation.

## The dev kit — on-device C compiler

The goal: edit → compile → run a guest **with direct GPIO/sensor access** →
iterate, all on the device, then deploy the **identical rv32 `.elf`** to the
shipping low-power chip with only the shims changed. This kills the "USB dongle
+ big Python stack just to read a temperature value" workflow.

### Not writing a compiler — using stock GCC

The scan of the current tree settled this: the repo already cross-builds guests
with stock GCC (`examples/common/vm_objs.sh` drives `riscv64-unknown-elf-gcc`
etc. to `-march=rv32imc -mabi=ilp32 -nostdlib -nostartfiles -ffreestanding
-Wl,-T guest.ld`), and the ecall ABI (`include/vm/vm_ecall.h`) was
*deliberately* built to host a real libc — low numbers are the Linux RV32
numbers (`openat 56, close 57, lseek 62, read 63, write 64, exit 93,
mkdirat 34, unlinkat 35`) and the header comments target **picolibc** wrappers.
So "implement a compiler" is really two smaller jobs: a frontend, and a
decision about where the industry compiler runs.

### `rvcc` — the frontend (Phase 1)

A spec-freezing driver, not a compiler. Inputs: **source path, `-O` level,
`-g` on/off** — nothing else. Hard-codes `-march=rv32imc -mabi=ilp32` +
`guest.ld` + the `_start` crt0 (`examples/common/guest/vm_runtime.c`) + the SDK
libc, orchestrates compile → assemble → link, scratches on the trashfs ramdisk,
discards artifacts. This is essentially productizing `vm_objs.sh` +
`guest_template/build.sh` into one stable two-knob command. It is agnostic to
which GCC sits underneath (native cross vs. in-VM), so it is the stable seam
across every host.

Desktop / CI path: `rvcc` over a **version-pinned native cross-GCC**. Parity
across Windows / Linux / Pi = same GCC version + same flags + reproducible-build
hygiene (no timestamps, sorted inputs) → identical `.elf`.

### Guest-hosted compiler (Phase 2, the endgame)

Run the industry compiler **as an rv32 guest** — dropped into the vmos
filesystem and loaded like any `SYS_SPAWN_AND_WAIT` guest. Why this beats a
native ARM GCC on the floor:

- A native GCC on *bare-metal* mgos would need a second mini-POSIX layer built
  in the floor just to host it (files, heap, `fork`/`exec`/`wait`, env,
  signals) — a big native GPL surface against the TCB thesis. The **guest**
  path instead **reuses mgos's own OS services** (trashfs, `SYS_ALLOC` heap,
  `SYS_SPAWN`, the capability mailbox). The VM *is* the hosted environment GCC
  assumes; picolibc + the ecalls make the compiler think it's on a small normal
  POSIX system.
- **Portability / parity for free:** the identical `cc1.elf` runs on the Pi,
  the Windows sandbox, and the Linux sandbox — bit-identical, because the VM is
  identical.
- **Sandboxed** — fits the drivers/tools-as-VM-processes model.

Cost: **interpreter speed** (RV32 interpreted on ARM11, no JIT — see
`src/vm/vm_core.c`). Small TUs are seconds; a future JIT is the lever if it
bites. The interpreter costs speed, not RAM.

### GCC vs Clang, and sizing

- **Image budget: ≤ 50 MB** (John's line). C-only GCC (`cc1` + `gas` + `ld`,
  single-target, stripped, picolibc) lands ~30-40 MB — comfortable. Clang is one
  binary and permissive, but ~50-150 MB with a higher RAM floor; its footprint
  fights the ramdisk/fast goal. **GCC wins for "smallest thing that's still an
  industry compiler."**
- **Minimize by build config, not source surgery:** `--enable-languages=c`,
  `--disable-multilib` (exactly rv32imc/ilp32), `--disable-nls/plugins/libssp`,
  no docs, `--disable-checking`, compiler built `-Os -s`, picolibc-nano as its
  own libc. Realistic endpoint `cc1` ~15-25 MB.
- **The compiler is a file on storage, not in the firmware image** — loaded on
  demand, discarded after. Firmware stays minimal. Baking apps in is a
  *no-storage-micro* workaround; the compiler is simply **not a lesser-micro
  feature.** Lower embedded devices are not dev kits.

### RAM tiering

Peak `cc1` working set: ~15-30 MB baseline even for a tiny file, ~30-80 MB for a
normal app TU.

- **Pi Zero (512 MB): trivial.** Give the compiler-guest a 128-256 MB region.
- **PSRAM MCU (H745 + QSPI PSRAM): marginal, per-file.** Back the heap with
  `SYS_L2_ALLOC` / the upper-half PSRAM region (`0xE000_0000`). Fits small TUs
  only.
- **Bare micro (KB-few-MB): no.** Cross-compile on desktop, ship the `.elf`.

### Phase-2 gaps (all bounded)

1. **mini-libc → picolibc.** Fill the few missing syscall stubs — mainly
   `fstat`/`stat`, a real `sbrk`-backed heap, `getcwd`/`environ`. Because the
   low ecall numbers *are* the Linux RV32 numbers, this is wiring, not
   invention.
2. **Bigger DATA region + heap.** Default is 16 KB (fine for apps); a
   compiler-guest asks for a large region + real `sbrk`. Already per-load
   configurable (`data_region_size` / `spawn_data_kb`).
3. **argv on `SYS_SPAWN_AND_WAIT`** — today `path → exit_code`, `argc=0`. `rvcc`
   needs "cc1 /td0/foo.c -o /td0/foo.o -O2". Add an argv-carrying spawn (small,
   generally useful) or have `rvcc` drive stages and pass args via a scratch
   file.
4. **libstdc++ on rv32/picolibc** — GCC is C++, so `cc1` needs a C++ runtime on
   the target. **This is the main headache** (embedded libstdc++ builds are
   finicky). Weeks of fiddling, a known exercise, not research.

Soft-float and static linking are non-issues: GCC's internal math is host-FP
independent, so rv32imc-no-`F` is fine for the binary; `-static` suits the
loader (which rejects dynamic anyway).

### `tcc` — the back-pocket

If image/RAM pressure ever outranks the industry-compiler requirement (compile
on modest micros), **tcc** is the escape: ~100 KB-1 MB, blazing, single binary,
no libstdc++, has a RISC-V backend. Not GCC codegen, not "industry standard" —
a second-tier-target complement, not a replacement.

## Hardware-IO ecall family (new subsystem)

The dev-kit workflow requires a surface the current tree does not have. Existing
ecall families: audio, fs, stream, mailbox, video/PPU, input (pads/mouse), TUI —
but **no general GPIO.** The dev kit needs a new family:

- `SYS_GPIO_*`, `SYS_I2C_*`, `SYS_SPI_*`, `SYS_ADC_*`, likely 1-wire (temp
  sensors). Fresh number range, opt-in installer like `vm_host_install_fs`.

**Design rule: expose transactions, not pin-wiggles.** `SYS_I2C_XFER`,
`SYS_ADC_READ`, `SYS_SENSOR_READ` — one ecall does a whole bus transaction
natively in the floor. Do **not** make the guest toggle pins per-cycle over
ecalls: an interpreted VM plus an ecall trap per edge is far too slow for
timing-critical work (WS2812, tight 1-wire). Same control-in-VM / hot-path-in-
floor split as WiFi — the timing-sensitive part lives in the shim, which is
exactly the part allowed to differ per target. This keeps the guest portable
*and* fast.

Shims land in the `src/host/platform_stub.c` weak-symbol seam per target
(`platform_rpi.c`, `platform_stm32.c`, ...).

**Future: per-app ecall-capability manifest.** Emit which `SYS_*` an `.elf`
touches, check it against a target's shim profile before deploy — catches
"worked on the Pi, dead on the micro", and a capability list per app is halfway
to a CRA attestation.

## Relationship to existing work — no disturbance to the SNES cart

This port is **purely additive** and must not perturb the SNES coprocessor /
dev-cart work (mgapi / copro / PPU / FMV / r3d / the emitter kernel — see
[[snes-cart-kernel]], `docs/emitter-kernel.md`).

- New platform target `src/host/platform_rpi.c` sits **beside**
  `platform_win.c` / `platform_posix.c` / `platform_stub.c`; the existing ones
  are untouched.
- New ecalls (GPIO/I2C, any Pi handlers) go in **fresh number ranges** as opt-in
  installers — no collision with the `SYS_MG_*` / `SYS_COPRO_*` / `SYS_FMV_*`
  graphics surface. The only shared edit is **adding** numbers to `vm_ecall.h`
  (+ the guest `vm_runtime.h` mirror) — no renumbering, no behavior change.
- The shared VM core / mailbox / scheduler do not change for the MVP. The one
  core touch is Phase 2 (argv on spawn + a larger default data region), both
  backward-compatible.
- Exercising the same VM on a second real target **hardens** the runtime the
  cart depends on.

**Discipline that guarantees it:** do the work on a `feat/rpi-port` branch off
`dumpsterfire`. The emitter-kernel SNES work stays on mainline; the Pi work
merges only when wanted.

## Roadmap

Each phase isolates one class of risk.

0. **ARM11 bring-up** — boot handoff, stack/BSS, UART debug, the **flat cache
   page table**. Substrate decision: roll-your-own CC0 floor (small surface:
   mailbox FB, PWM, GPIO, UART, page table are each a few hundred lines of MMIO)
   vs. Circle (GPLv3, fast but license-infecting — reference only). Lean
   roll-your-own.
1. **First light** — mailbox framebuffer; get an existing demo onto a TV.
2. **Audio** — PWM out from the mixer staging buffer.
3. **GPIO / hardware-IO ecalls** — the transaction-oriented family; the dev-kit
   input surface.
4. **`rvcc` Phase 1** — frontend over a native cross-GCC; the on-device edit/
   compile/run loop (compiler runs native at first, or cross on desktop).
5. **Network** — CYW43 SDIO shim + `cyw43-driver` + lwIP-UDP; WiFi mailbox
   transport with a SoftAP node.
6. **USB host** — after the TinyUSB-host spike; CDC then MSC.
7. **Guest-hosted compiler (Phase 2)** — picolibc floor, spawn-argv, big heap,
   libstdc++ on rv32; the drop-in-the-filesystem self-hosted toolchain.

MVP (0-3) is unblocked without WiFi or USB.

## Open questions

- Network topology: star-around-one-SoftAP-node vs. external pocket router.
- USB: does any costume actually need it, or is GPIO + framebuffer + audio the
  real set? (Decides whether "no drivers" stays literally true.)
- Substrate: confirm roll-your-own CC0 floor over Circle-as-reference.
- Whether the desktop sandbox also runs the guest-hosted compiler (bit-identical
  parity) or stays on native cross for speed (version-pinned parity).
