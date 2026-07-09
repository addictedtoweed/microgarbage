# SNES kernel (cart side)

A small 65816 kernel for the SNES half of the coprocessor system — **no
pvsneslib**. The coprocessor *is* the cartridge ROM; this code boots the SNES,
pulls a kernel the copro stages into WRAM, and then gets out of the way.

The kernel is deliberately **thin — a generic transfer substrate, not frame
logic**. It runs **NMI-off**, driven by a single self-chaining H+V timer IRQ
that force-blanks a **dynamic letterbox** window and runs a **cycle-budgeted DMA
chainer**: it reads the live beam position before each transfer and streams as
many bytes as the remaining blank window can afford (**~160 B/line, measured
safe on real hardware**), deferring the rest to the next frame. Large frames
(e.g. a full-screen CHR upload) simply span several frames.

The design direction is that each RISC-V **application** owns its own per-frame
transfer: rather than tweaking this 65816 kernel, an app composes a tailored
handler on the copro side via the guest **NMI builder** (`examples/common/guest/
mg_nmi.h`), emitting minimal, frame-shape-specific 65816 for lowest overhead and
maximum DMA throughput. One generic kernel; the frame-specific logic lives in
the app.

## Hardware model

The STM32 presents its **64 KB M7 DTCM** as the cart ROM, decoding only a
**16-bit address (A0–A15)**, so that single 64 KB window is **mirrored across
every ROM bank** (bank lines ignored, gated by ROM-select). It maps as **HiROM**:
the full window is flat-addressable at `$0000–$FFFF` of a bank (we use `$C0`),
and `$00:8000–$FFFF` aliases the upper half for the reset/boot path. The bus is
bidirectional by convention:

- **Reads** return whatever the copro currently presents (the "ROM").
- The bus is **read-only**: the SNES talks back to the copro purely by **which
  address it reads**. The copro watches the address bus and decodes the access;
  the returned byte is don't-care. No write/feedback pins.

Auto-joypad read is disabled. The kernel bit-bangs `$4016`/`$4017` itself in
active display (the moment NMI returns from the vblank DMA burst), reading up
to **four pads in parallel** via the standard multitap-capable protocol. Each
pad is then forwarded to the copro through **eight page-aligned 256-byte
ports** (`JOYPORT_P[0..3]_LO/HI`): the kernel reads `base + pad-byte` with an
8-bit index, so the access can never cross the page (no 65816 indexed dummy-
read) and the copro latches exactly one clean address per port. Boot uses a
single read-strobe (`STROBE_BOOTED`).

The copro **holds the SNES in reset** after power-on until the boot window is
primed, so the very first fetch already sees a valid boot image.

## Boot lifecycle

1. **Reset** → `boot.s` (served at `$8000–$FFFF`): native mode, stack, direct
   page, forced blank, IRQ/NMI off.
2. **Handshake**: spin until `COPRO_STATUS & ST_KERNEL_RDY`.
3. **Copy** the kernel blob from the window into low WRAM (`$0400`), via the
   linker's `__KERNEL_LOAD__`/`__KERNEL_RUN__`/`__KERNEL_SIZE__`.
4. Read `STROBE_BOOTED` → copro switches the window to runtime serving.
5. **Hand off**: `jmp` to the WRAM kernel.

The hardware NMI/IRQ vectors point at 3-byte **trampolines** that
`jmp (RAMVEC_*)`, so the RAM kernel installs its own handlers. **The copro must
keep the window's top page `$FF00–$FFFF` static after boot** (trampolines +
vectors) — everything below it is fair game as the data channel.

## Per-frame loop (`kernel.s`, runs from WRAM)

The kernel runs **NMI-off** (the NMI vector is parked at a bare `RTI`). A single
self-chaining H+V timer IRQ (`irq`) drives every frame as a two-state machine —
a "virtual NMI" that also owns the force-blank letterbox:

```
state B  (top-letterbox scanline):     unblank -- INIDISP = brightness
   ...active display...
   joypads bit-banged here ($4016/$4017, 4 pads, multitap; auto-read off)
   post 4 pads -> JOYPORT_P[0..3]_LO/HI read-strobes (acks prev frame)
state A  (bottom-letterbox scanline):  force-blank (INIDISP.7 = 1), then run the
    CYCLE-BUDGETED DMA CHAINER over the blank window (vblank + letterbox):
        read COPRO_FRAME_RDY -- if 0, leave VRAM as-is (last frame persists)
        walk COPRO_DMA_LIST (8 slots x 8 bytes); per slot:
            re-read the live beam (OPVCT); if this slot's bytes won't fit the
              window still remaining, DEFER the rest to the next frame and stop
            else program channel 0 (BBAD0/DMAP0/A1T0/DAS0; A1B0=COPRO_BANK preset),
              prep by bbus ($22 CGDATA / $18 VMDATAL / $04 OAMDATA), fire MDMAEN
        strobe COPRO_FRAME_DONE -- the copro advances to the next sub-frame
    re-arm both IRQ targets for the next field
```

The protocol is still **frame-ready flag + DMA descriptor list** (`COPRO_FRAME_RDY`
+ `COPRO_DMA_LIST`; see `copro.inc` for the 8-byte descriptor layout). What changed
from the original design is that the transfer is now **cycle-budgeted and
letterbox-widened**: the chainer spends the whole vblank + force-blank window at
~160 B/line and defers overflow across frames, and `COPRO_FRAME_DONE` lets the copro
drive **multi-sub-frame delivery** of a payload too big for one window (a full 240×208
frame streams over ~4 frames). Channel 0 is reused across slots (SNES DMA channels
never run in parallel). Reads are idempotent — if the copro doesn't update, the last
picture persists.

## Files

| file        | role |
|-------------|------|
| `boot.s`    | reset, init, handshake, copy-to-RAM, vectors + trampolines |
| `kernel.s`  | RAM-resident kernel: virtual-NMI timer IRQ + cycle-budgeted DMA chainer + force-blank letterbox |
| `smoke.s`   | standalone smoke-test kernel (cycle backdrop on button) |
| `copro.inc` | the SNES↔copro interface: window/read-ports/status/payload map |
| `snes.inc`  | the SNES registers used |
| `snes.cfg`  | ld65 layout: kernel LOAD in ROM, RUN in WRAM |
| `build.ps1` | assemble + link with ca65/ld65 → `build/snes_boot.bin` |

## Build

Needs the **cc65** suite (`ca65`, `ld65`) on PATH:

```powershell
.\snes\build.ps1     # -> snes\build\snes_boot.bin (64 KB HiROM window image)
```

## Smoke test

```powershell
.\snes\build.ps1 -Smoke      # -> snes\build\snes_smoke.sfc
```

A standalone HiROM that needs **no coprocessor** — `boot.s` is built with
`SMOKE_TEST` (skips the copro handshake), copies `smoke.s` into WRAM, and jumps.
The smoke kernel enables no BG layers, so the whole screen is the backdrop
(`CGRAM[0]`); each vblank it reads the joypad and, **while any button is held,
advances the backdrop colour** (release to freeze). It runs in stock bsnes as a
plain HiROM, yet still exercises WRAM execution, the ROM→RAM NMI trampoline,
vblank timing, auto-joypad read, and CGRAM writes — and stays valid once the
custom mapper is wired in.

## Status

**Live, not a stub.** `demo_fmv` streams full-motion video at 15–20 fps on real
hardware and in bsnes-plus over this kernel, and the 3D renderer stages frames
through the same path. Control flow, handshake, joypad path, the cycle-budgeted
DMA chainer, and the dynamic force-blank letterbox are all real and verified.

Done since the original design:

- **Virtual-NMI kernel** — NMI off; a single self-chaining H+V timer IRQ drives
  the frame, runs a live-beam (OPVCT) cycle-budgeted DMA chainer with
  defer/resume, and strobes `COPRO_FRAME_DONE`.
- **Force-blank letterbox** — the IRQ asserts/clears `INIDISP.7` at the letterbox
  lines, widening the DMA window on demand. ~160 B/line measured safe on hardware,
  so a full-screen frame streams over ~4 frames.
- **Multi-sub-frame delivery + double-buffering** — a payload too big for one
  window streams across several frames; the copro pipelines depth-2 (commit-ahead)
  so the kernel never idles between frames.
- **Manual joypad read** in active display (auto-read off), freeing the whole
  force-blank window for the DMA burst.

Open / direction:

- **App-composed transfer via the NMI builder** — the direction (see the intro)
  is for each RISC-V app to emit its own frame-shape-tailored transfer handler on
  the copro side (`mg_nmi`) rather than leaning on the generic kernel path, for
  lowest overhead + maximum DMA. Reconciling the builder with the virtual-NMI
  (timer-IRQ) kernel is the active work.
- Read-as-signal relies on the copro only acting on reads in the port region —
  safe because the kernel runs from WRAM (prefetch never hits the cart) and the
  DMA-source range is kept disjoint from the ports.
