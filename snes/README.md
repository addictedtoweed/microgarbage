# SNES kernel (cart side)

A small 65816 kernel for the SNES half of the coprocessor system — **no
pvsneslib**. The STM32 coprocessor *is* the cartridge ROM; this code boots the
SNES, pulls a kernel the copro stages into WRAM, and runs a per-frame lockstep
loop that DMAs copro-prepared PPU data each vblank.

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

The joypad is forwarded through **two page-aligned 256-byte ports**
(`JOYPORT_LO`/`HI`): the kernel reads `base + pad-byte` with an 8-bit index, so
the access can never cross the page (no 65816 indexed dummy-read) and the copro
latches exactly one clean address per port. Boot uses a single read-strobe
(`STROBE_BOOTED`).

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

```
post joypad -> read JOYPORT_LO+lo, JOYPORT_HI+hi   (the reads ARE the message,
                                                    and ack the previous frame)
wait for ST_FRAME_RDY                              (copro staged the payload)
arm DMA burst; next vblank NMI runs it (CGRAM/tilemap/CHR -> PPU); repeat
```

The vblank DMA is where the letterbox forced-blank budget applies (54-line
window @208 active → fits a 26,776 B frame over 3 vblanks @20 fps).

## Files

| file        | role |
|-------------|------|
| `boot.s`    | reset, init, handshake, copy-to-RAM, vectors + trampolines |
| `kernel.s`  | RAM-resident main loop + vblank NMI DMA (stub) |
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

## Status / open items

This is a **stub**: control flow, handshake, and one concrete DMA (CGRAM) are
real; the rest is marked `TODO`.

- Addresses in `copro.inc` (data-window bank, port/status offsets) are
  **placeholders** — confirm against the mapper's decode.
- VRAM DMA (tilemap + CHR), OAM, and double-buffered base-flipping are TODO.
- Joypad currently uses SNES auto-read (costs ~3 vblank lines); switch to a
  manual read outside the burst to reclaim the full DMA budget.
- Per-frame lockstep assumed (SNES blocks on the copro each frame). `ST_FRAME_RDY`
  is a single bit; a frame counter would avoid a clear-race if it ever bites.
- Read-as-signal relies on the copro only acting on reads in the port region —
  safe because the kernel runs from WRAM (prefetch never hits the cart) and the
  DMA-source range is kept disjoint from the ports.
