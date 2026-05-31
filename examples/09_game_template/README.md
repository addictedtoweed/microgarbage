# Game template

Working skeleton for a microgarbage cart game. Copy this directory,
edit `game.c`, drop assets into `assets/`, run `build.sh`.

## Prerequisites

- A RISC-V cross-compiler (`riscv64-unknown-elf-gcc` or
  `riscv32-unknown-elf-gcc`). On MSYS2: `pacman -S
  mingw-w64-x86_64-riscv64-unknown-elf-gcc`.
- For asset baking: `tools/png_to_chr` and `tools/bin2c` (planned;
  the template still builds without them — `game.c` has the asset
  uploads commented out and paints a fallback solid color).

## Build

```sh
./build.sh
```

Output: `build/game.elf`. The script reuses
`examples/common/vm_objs.sh` for the RV32IMC toolchain settings.

## Run (under bsnes-plus, dev workflow)

1. Drop `build/game.elf` into the trashfs slice the mgapi cart
   runtime serves (the exact path depends on the runtime's cart
   layout — see [[mgapi-cart-runtime]] for current status).
2. Build the bsnes-plus mgapi-mapper fork (see `snes/bsnes/`).
3. Load the boot ROM stub; the runtime autostarts your `game.elf`.

## Run (on hardware, when the board lands)

1. Build the cart runtime for the H745 target.
2. Use the dev USB bridge to push `game.elf` into PSRAM trashfs.
3. SNES reset → runtime autostarts the ELF.

## Project shape

```
game.c            the game itself. Edit this.
assets/           art + audio. Drop new PNG / WAV here.
generated/        build-time output. Ignored by git.
build/            ELF + intermediates. Ignored by git.
```

## Adding an asset (once png_to_chr lands)

1. Save PNG to `assets/foo.png` (any size, RGB or RGBA).
2. `./build.sh` — `png_to_chr` quantizes to 4bpp + paired palette;
   `bin2c` embeds both into the ELF.
3. In `game.c`:

   ```c
   #include "generated/foo_chr.h"
   #include "generated/foo_pal.h"
   /* ... in boot(): */
   MG_OR_PANIC(mg_chr_upload(VRAM_ADDR, foo_chr, foo_chr_len));
   mg_palette_load(START_IDX, (const uint16_t *)foo_pal,
                   foo_pal_len / 2);
   ```

## Adding a source file

Add it to the `MG_SRCS` array in `build.sh`, or list it after
`game.c` directly in the gcc invocation. The template's link uses
`*.c` glob over `examples/common/guest/mg_*.c` so library additions
land automatically.

## API reference

The full `mg_*` surface is documented at `docs/game-api.md`. Quick
pointers to the per-concern headers:

```
examples/common/guest/mg_frame.h    wait_frame / frame_commit
examples/common/guest/mg_input.h    pads + edge detect
examples/common/guest/mg_sprite.h   OAM
examples/common/guest/mg_actor.h    sub-pixel parent of sprites
examples/common/guest/mg_bg.h       BG layers + tilemap
examples/common/guest/mg_mode7.h    Mode 7 (separate header)
examples/common/guest/mg_hdma.h     per-scanline tables
examples/common/guest/mg_gfx.h      CHR upload + palette + bgr555
examples/common/guest/mg_audio.h    SFX + streaming
examples/common/guest/mg_panic.h    error path + MG_OR_PANIC
```

Single include: `#include "mg_game.h"` drags in all of the above.

## Entry point

Freestanding RV32IMC ELFs enter at `_start`, not `main`. `game.c`
defines `void _start(void)` with the canonical frame loop. Returning
from `_start` is undefined in this runtime version; the contract is
`for (;;)`. A future runtime with a "return to launcher" path will
define `mg_exit_to_launcher()`.
