#!/bin/bash
# 02_counter/build.sh — build the host and guest for this example.
#
# Usage:
#   ./build.sh           # build everything
#   ./build.sh clean     # remove the build/ directory
#   ./build.sh run       # build and run
#
# Outputs:
#   build/host           the host application
#   build/guest.elf      the guest program (RV32IMC)
#
# Requirements:
#   - A C compiler (cc, gcc, clang — set $CC to override)
#   - For rebuilding guest.elf: gcc-riscv64-unknown-elf
#     (Ubuntu: sudo apt install gcc-riscv64-unknown-elf)
#     If the cross-compiler is missing, the script skips the
#     guest rebuild — a pre-built guest.elf is checked in.

set -e

# Locate this script's directory (the example dir).
EXAMPLE_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$EXAMPLE_DIR"

# Pull in shared config (CC, CFLAGS, VM_CORE_SRCS, GUEST_*, etc).
. ../common/vm_objs.sh

BUILD_DIR="$EXAMPLE_DIR/build"

case "${1:-build}" in
    clean)
        rm -rf "$BUILD_DIR"
        echo "02_counter: cleaned"
        exit 0
        ;;
esac

mkdir -p "$BUILD_DIR"

# ---------------------------------------------------------------
# Build the host application.
# ---------------------------------------------------------------
echo "02_counter: compiling host..."
"$CC" "${CFLAGS[@]}" \
    -o "$BUILD_DIR/host" \
    "$EXAMPLE_DIR/host.c" \
    "${VM_CORE_SRCS[@]}"

# ---------------------------------------------------------------
# Build the guest ELF (if the cross-compiler is available).
# ---------------------------------------------------------------
if have_guest_cc; then
    echo "02_counter: compiling guest (RV32IMC)..."
    "$GUEST_CC" "${GUEST_CFLAGS[@]}" \
        -Wl,-T,"$GUEST_LD" \
        -o "$BUILD_DIR/guest.elf" \
        "$EXAMPLE_DIR/guest.c"
else
    if [ -f "$BUILD_DIR/guest.elf" ]; then
        echo "02_counter: $GUEST_CC not found, using existing guest.elf"
    else
        echo "02_counter: WARNING — $GUEST_CC not found and no guest.elf exists"
        echo "02_counter:   install gcc-riscv64-unknown-elf or copy a prebuilt"
        echo "02_counter:   guest.elf into $BUILD_DIR"
    fi
fi

echo "02_counter: built. To run:"
echo "    $BUILD_DIR/host"

# Optional run.
if [ "${1:-}" = "run" ]; then
    echo
    echo "02_counter: ===== running ====="
    "$BUILD_DIR/host"
fi
