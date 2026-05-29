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

```
(NMI just returned from vblank DMA -- we're at the start of active display)
read joypads     -> bit-bang $4016/$4017 (4 pads, multitap-capable)
post 4 pads      -> 8 read-strobes at JOYPORT_P[0..3]_LO/HI  (acks prev frame)
wai              -> sleep until next vblank
(at vblank) NMI:
    read COPRO_FRAME_RDY -- if 0, RTI (previous frame stays on screen)
    walk COPRO_DMA_LIST (8 slots * 8 bytes):
        if slot.bbus == 0: skip
        else:
            program channel 0: BBAD0/DMAP0/A1T0/DAS0  (A1B0=COPRO_BANK preset)
            prep (based on bbus):
                $22 (CGDATA)  -> write CGADD = slot.prep low byte
                $18 (VMDATAL) -> VMAIN=$80; VMADDL/H = slot.prep
                $04 (OAMDATA) -> OAMADDL/H = slot.prep
            sta MDMAEN          ; fire channel 0; CPU paused until slot done
```

The protocol is **frame-ready flag + DMA descriptor list**: the copro stages the
per-frame payload at `COPRO_DATA`, fills `COPRO_DMA_LIST` with up to 8 descriptors
(bbus / dmap / src / size / prep), then writes `COPRO_FRAME_RDY` to a non-zero
value to signal "list is complete, go." Each descriptor names one DMA -- the
copro composes whatever combination of CGRAM / VRAM / OAM transfers it needs
each frame. The kernel marshalls the list every vblank; channel 0 is reused
across slots (SNES DMA channels never run in parallel anyway, so reusing is
functionally identical to using all 8). Reads are idempotent: if the copro
doesn't update before the next vblank, the kernel re-runs the same list and
the picture is unchanged.

See `copro.inc` for the 8-byte descriptor layout.

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

This is a **stub**: control flow, handshake, joypad path, and the full DMA
dispatch (CGRAM / VRAM / OAM, generic per-slot from a copro-staged list) are
real; the higher-level frame composition is what's left.

- Addresses in `copro.inc` (data-window bank, port/status offsets) are
  **placeholders** — confirm against the mapper's decode.
- *(done)* Manual joypad read in active display (`read_joypads` in `kernel.s`)
  with auto-read disabled — the full 54-line forced-blank window is now
  available for the DMA burst.
- *(done)* Per-frame DMA dispatch via the **frame-ready flag + 8-slot list**
  protocol (`COPRO_FRAME_RDY` + `COPRO_DMA_LIST`). Main loop is `wai`-driven;
  NMI walks the slots and programs channel 0 from each. Supersedes the earlier
  bitmask `COPRO_DMACTRL` design.
- **Double-buffering** is TODO: the copro currently puts each frame's data at
  the same VRAM addresses; flipping `BG1SC` / `BG12NBA` between two banks each
  frame is a small extension on top of the list (the copro just varies the
  slot `prep` values).
- **Letterbox forced-blank extension** is TODO — needed for the full 26.8 KB
  FMV block (which doesn't fit a standard 38-line vblank). The DMA dispatch is
  in place; this just needs an IRQ at line ~209 to assert `INIDISP.7` and a
  matching clear before line 9 of the next frame.
- Read-as-signal relies on the copro only acting on reads in the port region —
  safe because the kernel runs from WRAM (prefetch never hits the cart) and the
  DMA-source range is kept disjoint from the ports.
