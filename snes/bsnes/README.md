# bsnes-plus custom mapper for mgapi.dll

This directory holds reference copies of the bsnes-plus-side glue
that runs the SNES against `mgapi.dll`. The DLL owns the full
microgarbage runtime (VM + audio + trashfs + shell over TCP); the
bsnes-plus side is a thin forwarder, modeled after MSU1.

**As of the current session, these files have been actually copied
into a local bsnes-plus clone at**
`C:\Users\IP Freely\Documents\Source\bsnes-plus\` **and all
cartridge / system / Makefile hooks are wired. The files here are
the canonical copies for re-application or porting to other forks.**

## Files in this directory

| File | Role |
|---|---|
| `Mgapi.hpp` | C++ wrapper around the DLL — `LoadLibrary`, `GetProcAddress`, the seven `mgapi_*` exports. |
| `Mgapi.cpp` | Implementation. |
| `README.md` | This file — where to drop the sources, what to wire into bsnes-plus. |

## Drop-in location in bsnes-plus

Copy `Mgapi.hpp` and `Mgapi.cpp` into `bsnes/snes/chip/mgapi/` of your
bsnes-plus tree, alongside the other custom-chip directories
(`msu1/`, `satellaview/`, `bsx/`, etc.). Then:

1. **Register the chip in the build.** In `bsnes/snes/snes.cpp`
   (or wherever MSU1 is registered), add `mgapi/Mgapi.cpp` to the
   sources list — copy the line for MSU1 and rename.
2. **Make `mgapi` reachable from `cartridge`.** Add
   `#include "chip/mgapi/Mgapi.hpp"` to `snes/cartridge/cartridge.hpp`
   so the cart's `read()` can call into it.
3. **Boot the DLL.** In `Cartridge::load()` (or wherever the cart
   type is decoded from the manifest), call
   `SNES::mgapi.load({...})` once. Match the MSU1 boot pattern.
4. **Cart-bus reads.** In the cart's read decoder, route the address
   ranges the SNES kernel uses through `mgapi.cartRead(addr)`:
       - `$00:8000-$00:FFFF`  (boot blob + RAM kernel + vectors)
       - `$C0:0000-$C0:FFFF`  (full window mirror used at runtime)
       - `$80:8000-$80:FFFF`  if your mapper mirrors the fast bank
   Outside these ranges fall through to whatever the dummy ROM
   loaded for the manifest does (or just return 0).
5. **Audio source.** Add an audio source that pulls from
   `mgapi.audioPull(buf, frames)` each frame. Copy MSU1's audio
   class shape almost verbatim — replace its file-read path with
   the `audioPull` call and the rest stays the same. The DLL outputs
   at exactly 44.1 kHz to match bsnes-plus's native rate; no
   resampling is needed.
6. **Per-frame tick + joypad post.** In whichever bsnes-plus method
   runs at frame boundaries (typically the system's vsync or
   `interface->videoRefresh` callback), call
   `mgapi.postJoypads(pads)` and `mgapi.step(elapsed_ns)`. Compute
   `pads[i]` from bsnes-plus's controller state — pack the buttons
   in the same bit layout as the SNES `$4218` register so the kernel
   sees the same shape it would on real hardware.

7. **Reset hook.** Wrap whatever bsnes-plus calls when the user hits
   Reset (F5 / menu) with the cart-reset marshalling:

   ```cpp
   void MgapiCart::reset() {
       SNES::mgapi.cartResetBegin();
       while (!SNES::mgapi.cartResetReady()) {
           SNES::mgapi.step(1'000'000);   // 1 ms / iteration
       }
       Base::reset();                     // bsnes-plus's own reset
       SNES::mgapi.cartResetEnd();
   }
   ```

   `cartResetBegin` synchronously re-stages the static cart window
   from the chosen ROM (boot blob + vectors + RAM kernel), clears
   the side-effect registers (status, frame-ready, joypad mailbox),
   and bumps the reset counter the copro guest reads via
   `mg_copro_reset_count()`. `cartResetReady` is timer-gated by
   `Config::reset_hold_ms` (default 50 ms; covers the CIC ~20 ms
   hold + cart-window stabilization). The poll loop lets the copro
   guest run during the hold so it can react to the bumped reset
   counter and re-stage its first frame in time.

   On **cold boot** (initial `Cartridge::load`), `mgapi_init`
   already does the equivalent of `reset_begin` — don't call it
   again. The reset hook is only for user-triggered warm resets.

## Cart-window read decode table

These are the addresses the SNES kernel actually hits — match them
when wiring the cart read decoder. All values are byte offsets
within the 64 KB window (mirrored across every bank by HiROM).

| Offset | Purpose | Side effect |
|---|---|---|
| `$0000-$7DFF` | Per-frame PPU payload (CGRAM, tilemap, CHR) | none |
| `$7000-$77FF` | Joypad mailbox (8 page-aligned ports) | latch pad index from high byte |
| `$7800` | `COPRO_FRAME_RDY` byte | none |
| `$7808-$7847` | DMA descriptor list (8 slots × 8 bytes) | none |
| `$7E00` | `STROBE_BOOTED` | one-shot: status flips to runtime |
| `$7F00` | `COPRO_STATUS` byte | none |
| `$8000-$FFFF` | Boot blob + RAM-kernel blob + vectors | none |

`Mgapi.cartRead` already implements all of these inside the DLL —
the bsnes side just forwards.

## Manifest tag idea (bsnes-plus convention)

Match MSU1's pattern — add a manifest property like:
```
mgapi-dll = "C:/path/to/mgapi.dll"
```
…and read it in `Cartridge::load()` to seed the `Mgapi::Config`.
A missing tag falls back to the default `"mgapi.dll"` next to the
emulator exe.

## Audio sync

The DLL produces audio at 44.1 kHz natively (matches the SNES). Pull
in chunks of whatever your `Audio::Source::step()` wants — the
internal ring (~371 ms cap) absorbs jitter. Underrun → `audioPull`
returns fewer frames than requested; zero-fill or repeat the last
sample per MSU1's idiom.

## DLLs to copy alongside `mgapi.dll`

The mingw build of `mgapi.dll` drags in:

- `libgcc_s_seh-1.dll`
- `libwinpthread-1.dll`

`build-mgapi.ps1` already copies these next to `mgapi.dll`. Make sure
the bsnes-plus binary can find them — same directory as `mgapi.dll`
is the easiest setup.

## What lives where (summary)

- **`mgapi.dll`** = full microgarbage runtime + cart window decode +
  audio service + VM + shell + trashfs + L2 + TCP listener.
  Everything the runtime needs, all behind seven exports.
- **`Mgapi.hpp/.cpp`** (this directory) = C++ wrapper, no SNES
  knowledge, just `LoadLibrary` + thin call forwarding.
- **bsnes-plus cart + audio + input classes** (your work) = subclass
  the existing extension points (MSU1 is the template) and call into
  `SNES::mgapi`.

## Why MSU1 is the template

MSU1 is the closest existing precedent in bsnes-plus to what we're
doing:

- A custom on-cart chip with memory-mapped registers (matches our
  cart-window decode table).
- An external audio source that mixes with the SNES SMP output
  (matches our pull-from-DLL audio path).
- A boot-time loaded file (.msu) with runtime state (matches our
  `LoadLibrary("mgapi.dll")` + `mgapi_init`).

Copy MSU1's class skeleton, replace the file-IO with the
corresponding `Mgapi::` call, and you're 90% of the way there.

## TCP shell while the emulator runs

Once `mgapi_init` returns with a non-zero `tcp_listen_port` in the
config, the DLL is listening. Point PuTTY (Raw or Telnet) at
`localhost:<port>` and you'll get the VM shell prompt:

```
VM shell -- type 'help' for commands

[/td0]
$
```

…running alongside bsnes-plus. Spawn guest ELFs interactively:

```
$ run /cart/game.elf
```

This is how you develop and debug game code without restarting
the emulator.
