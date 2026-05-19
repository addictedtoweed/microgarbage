#!/bin/bash
# 03_mailbox/build.sh — builds the host and TWO guests.
#
# Usage:
#   ./build.sh           # build everything
#   ./build.sh clean     # remove the build/ directory
#   ./build.sh run       # build and run

set -e

EXAMPLE_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$EXAMPLE_DIR"
. ../common/vm_objs.sh

BUILD_DIR="$EXAMPLE_DIR/build"

case "${1:-build}" in
    clean)
        rm -rf "$BUILD_DIR"
        echo "03_mailbox: cleaned"
        exit 0
        ;;
esac

mkdir -p "$BUILD_DIR"

echo "03_mailbox: compiling host..."
$CC $CFLAGS \
    -o "$BUILD_DIR/host" \
    "$EXAMPLE_DIR/host.c" \
    $VM_CORE_SRCS

if have_guest_cc; then
    echo "03_mailbox: compiling producer (RV32IMC)..."
    $GUEST_CC $GUEST_CFLAGS -Wl,-T,"$GUEST_LD" \
        -o "$BUILD_DIR/producer.elf" "$EXAMPLE_DIR/producer.c"

    echo "03_mailbox: compiling consumer (RV32IMC)..."
    $GUEST_CC $GUEST_CFLAGS -Wl,-T,"$GUEST_LD" \
        -o "$BUILD_DIR/consumer.elf" "$EXAMPLE_DIR/consumer.c"
else
    if [ -f "$BUILD_DIR/producer.elf" ] && [ -f "$BUILD_DIR/consumer.elf" ]; then
        echo "03_mailbox: $GUEST_CC not found, using existing .elf files"
    else
        echo "03_mailbox: WARNING — $GUEST_CC not found"
    fi
fi

echo "03_mailbox: built. To run:"
echo "    $BUILD_DIR/host"

if [ "${1:-}" = "run" ]; then
    echo
    echo "03_mailbox: ===== running ====="
    "$BUILD_DIR/host"
fi
