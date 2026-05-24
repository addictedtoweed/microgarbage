#!/bin/bash
# 07_vm_task/build.sh — build the "VM as a preemptive task" demo.
#
# Builds a host that runs a real RV32 guest as a preemptive scheduler
# task alongside a native task. The guest.elf is checked in; it is only
# rebuilt if a RISC-V cross-compiler is available.
#
# Usage:
#   ./build.sh           # build
#   ./build.sh clean     # remove build/
#   ./build.sh run       # build and run
#
# Output:
#   build/vm_task        the host application
#
# Requirements:
#   - A C compiler (cc/gcc/clang; set $CC to override)
#   - POSIX threads + realtime timer (-lpthread -lrt) on Linux; on native
#     Windows the scheduler uses Win32 threads (build with mingw).
#   - To rebuild guest.elf: a RISC-V cross-compiler (riscv64-unknown-elf-gcc
#     or riscv-none-elf-gcc). If absent, the checked-in guest.elf is used.

set -e

EXAMPLE_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$EXAMPLE_DIR"
REPO_ROOT=$(cd "$EXAMPLE_DIR/../.." && pwd)
BUILD_DIR="$EXAMPLE_DIR/build"
CC="${CC:-cc}"

case "${1:-build}" in
    clean) rm -rf "$BUILD_DIR"; echo "07_vm_task: cleaned"; exit 0 ;;
esac

mkdir -p "$BUILD_DIR"

# ---- optional guest rebuild ----
GUEST_CC=""
for c in riscv64-unknown-elf-gcc riscv-none-elf-gcc riscv32-unknown-elf-gcc; do
    if command -v "$c" >/dev/null 2>&1; then GUEST_CC="$c"; break; fi
done
if [ -n "$GUEST_CC" ]; then
    echo "07_vm_task: rebuilding guest.elf with $GUEST_CC..."
    "$GUEST_CC" -march=rv32imc -mabi=ilp32 -nostdlib -nostartfiles -ffreestanding -O2 \
        -Wl,-T,"$REPO_ROOT/examples/common/guest.ld" \
        -o "$EXAMPLE_DIR/guest.elf" "$EXAMPLE_DIR/guest.c"
elif [ -f "$EXAMPLE_DIR/guest.elf" ]; then
    echo "07_vm_task: no RISC-V cross-compiler; using existing guest.elf"
else
    echo "07_vm_task: WARNING — no RISC-V cross-compiler and no guest.elf."
    echo "07_vm_task:   install riscv64-unknown-elf-gcc (or riscv-none-elf-gcc),"
    echo "07_vm_task:   or place a prebuilt guest.elf in this directory."
fi
# make the elf available next to the binary (host defaults to ./guest.elf)
[ -f "$EXAMPLE_DIR/guest.elf" ] && cp "$EXAMPLE_DIR/guest.elf" "$BUILD_DIR/guest.elf"

# ---- host ----
echo "07_vm_task: compiling host..."
LINK_FLAGS="-lpthread"
case "$(uname -s 2>/dev/null)" in
    *NT*|*MINGW*|*MSYS*|*CYGWIN*) ;;
    *) LINK_FLAGS="$LINK_FLAGS -lrt" ;;
esac

"$CC" -std=c11 -Wall -Wextra \
    -I"$REPO_ROOT/include" \
    -o "$BUILD_DIR/vm_task" \
    "$EXAMPLE_DIR/host.c" \
    "$REPO_ROOT/src/vm/presched.c" \
    "$REPO_ROOT/src/vm/vm_core.c" \
    "$REPO_ROOT/src/vm/vm_loader.c" \
    "$REPO_ROOT/src/memory/slab_stack.c" \
    $LINK_FLAGS

echo "07_vm_task: built. To run:"
echo "    (cd $BUILD_DIR && ./vm_task)"

if [ "${1:-}" = "run" ]; then
    echo
    echo "07_vm_task: ===== running ====="
    (cd "$BUILD_DIR" && ./vm_task)
fi
