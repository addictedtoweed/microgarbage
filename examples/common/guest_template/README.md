# microgarbage guest SDK — app template

A self-contained skeleton for a microgarbage VM guest (RV32IMC). **Copy
this whole directory anywhere**, edit `main.c`, and build — you only need
a RISC-V cross compiler (`riscv64-unknown-elf-gcc` or `riscv-none-elf-gcc`).
No microgarbage repo or host toolchain required; everything is vendored here.

## Build

```sh
./build.sh        # bash: Linux / macOS / MSYS2 / Cygwin
```
```powershell
.\build.ps1       # Windows PowerShell
```

Produces `app.elf`. Copy it where your host loads guests (e.g. a
microgarbage shell's `/host` directory) and `run app.elf`.

## Layout

- `main.c` — your code; execution starts at `int main(void)`.
- `include/` — host hooks + a freestanding mini-libc:
  - `vm_runtime.h` — core syscalls (`SYS_*`), `spawn`, time, rand, exit
  - `audio.h` — mixer / SFX / streaming music / FFT meter
  - `tui.h` — extended-char TUI + PuTTY mouse
  - `<stdio.h> <stdlib.h> <string.h> <time.h>` — routed to the host
  - `containers/`, `math/` — optional data structures + fixed-point
- `src/` — the runtime linked into every build (`vm_runtime.c`, `tui.c`)
  plus the opt-in `containers/` + `math/` sources.
- `guest.ld` — linker script (3 regions + stack; `ENTRY(_start)`).

## Optional tools (containers / math)

They're opt-in so unused code isn't linked. Add module paths to the
`MODULES` line in `build.sh` (or `$Modules` in `build.ps1`):

```sh
MODULES=(containers/ring_buffer math/fixed_point)
```

then `#include "containers/ring_buffer.h"` in your code. Mind the
inter-dependencies (`fifo_queue` needs `ring_buffer`, etc.).

## Regenerating

This bundle is generated from the microgarbage repo by
`examples/common/package-guest-sdk.sh` (or `./build-win.sh sdk`). Re-run
that to refresh it against the latest runtime/hooks.
