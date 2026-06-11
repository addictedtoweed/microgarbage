#!/bin/bash
# 08_rtos_demo/build.sh — build the host + three guest VMs.
#
#   ./build.sh        build only
#   ./build.sh run    build, then run
#   ./build.sh clean
#
# Host links: presched + vm_core + vm_loader + slab_stack, plus pthread
# (and -lrt on Linux). Guests are built from one guest.c three times
# (-DGUEST_NAME="A"/"B"/"C") against the shared guest SDK.
set -e
EXAMPLE_DIR=$(cd "$(dirname "$0")" && pwd); cd "$EXAMPLE_DIR"
REPO_ROOT=$(cd "$EXAMPLE_DIR/../.." && pwd)
BUILD_DIR="$EXAMPLE_DIR/build"
CC="${CC:-cc}"
SDK="$REPO_ROOT/examples/common/guest"
LD="$REPO_ROOT/examples/common/guest.ld"

case "${1:-build}" in
    clean) rm -rf "$BUILD_DIR"; echo "08_rtos_demo: cleaned"; exit 0 ;;
esac
mkdir -p "$BUILD_DIR"

# ---- guests (RV32, via the shared SDK) ----
GUEST_CC=""
for c in riscv64-unknown-elf-gcc riscv-none-elf-gcc riscv32-unknown-elf-gcc; do
    command -v "$c" >/dev/null 2>&1 && { GUEST_CC="$c"; break; }
done
if [ -n "$GUEST_CC" ]; then
    for nm in A B C; do
        lc=$(printf '%s' "$nm" | tr 'A-Z' 'a-z')
        echo "08_rtos_demo: building guest_$lc.elf..."
        "$GUEST_CC" -march=rv32imc -mabi=ilp32 -nostdlib -nostartfiles -ffreestanding -Os \
            -I"$SDK" -I"$SDK/include" -DGUEST_NAME="\"$nm\"" \
            -Wl,-T,"$LD" \
            -o "$BUILD_DIR/guest_$lc.elf" \
            "$EXAMPLE_DIR/guest.c" "$SDK/vm_runtime.c"
    done
else
    echo "08_rtos_demo: no RISC-V cross compiler found; need guest_{a,b,c}.elf to run."
fi

# ---- host ----
LINK="-lpthread"
case "$(uname -s 2>/dev/null)" in
    *NT*|*MINGW*|*MSYS*) ;;
    *) LINK="$LINK -lrt" ;;
esac
echo "08_rtos_demo: compiling host..."
"$CC" -std=c11 -Wall -Wextra -I"$REPO_ROOT/include" \
    -o "$BUILD_DIR/rtos_demo" \
    "$EXAMPLE_DIR/host.c" \
    "$REPO_ROOT/src/vm/presched.c" \
    "$REPO_ROOT/src/vm/vm_core.c" \
    "$REPO_ROOT/src/vm/vm_loader.c" \
    "$REPO_ROOT/src/memory/slab_stack.c" \
    $LINK

echo "08_rtos_demo: built  ->  $BUILD_DIR/rtos_demo"
[ "${1:-}" = "run" ] && (cd "$BUILD_DIR" && ./rtos_demo)
