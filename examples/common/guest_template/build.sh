#!/bin/bash
# build.sh — build this guest app into an RV32IMC ELF.
#
# Self-contained: needs ONLY a RISC-V cross compiler. No microgarbage
# repo, no host toolchain. Everything it links (the runtime, host-hook
# headers, mini-libc, optional containers/math) is vendored in this dir.
#
#   ./build.sh                 -> app.elf
#   OUT=foo.elf ./build.sh     -> foo.elf
#   GUEST_CC=riscv-none-elf-gcc ./build.sh   (override the cross)
set -e
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${OUT:-app.elf}"

# --- locate a RISC-V cross compiler ---
CC="${GUEST_CC:-}"
if [ -z "$CC" ]; then
    for c in riscv64-unknown-elf-gcc riscv-none-elf-gcc \
             riscv32-unknown-elf-gcc riscv64-elf-gcc; do
        command -v "$c" >/dev/null 2>&1 && { CC="$c"; break; }
    done
fi
[ -n "$CC" ] || { echo "build.sh: no RISC-V cross found; set GUEST_CC=" >&2; exit 1; }

# --- opt-in tools: module paths under src/ (no .c), e.g.
#       MODULES=(containers/ring_buffer math/fixed_point)
#     (mind inter-deps: fifo_queue needs ring_buffer, etc.) ---
MODULES=()

# --- path helper: a native-Windows cross under MSYS2/Cygwin needs
#     Windows-style paths; cygpath does that, and is absent on Linux. ---
p() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }

SRCS=("$HERE/main.c" "$HERE/src/vm_runtime.c" "$HERE/src/tui.c")
for m in "${MODULES[@]}"; do SRCS+=("$HERE/src/$m.c"); done
PSRCS=(); for s in "${SRCS[@]}"; do PSRCS+=("$(p "$s")"); done

# Size flags: -Os + function/data sections + --gc-sections drop unused
# SDK code per build; max-page-size=4 removes LOAD-segment alignment
# padding (no MMU); -Wl,-s strips symbols. Drop -Wl,-s if you want symbols
# for debugging (costs ~0.5 KB).
"$CC" -march=rv32imc -mabi=ilp32 -nostdlib -nostartfiles -ffreestanding -Os \
    -ffunction-sections -fdata-sections \
    -I"$(p "$HERE/include")" \
    -Wl,--gc-sections -Wl,-z,max-page-size=4 -Wl,-s -Wl,-T,"$(p "$HERE/guest.ld")" \
    -o "$(p "$OUT")" "${PSRCS[@]}"
echo "build.sh: built $OUT  (cross: $CC)"
