#!/bin/bash
# 10_hwio_sim/build.sh — the hardware-IO ecall family, end to end, on the
# desktop simulation backend.
#
# Usage:
#   ./build.sh           # build host + guest
#   ./build.sh clean     # remove build/
#   ./build.sh run       # build and run
#
# Outputs:
#   build/host           the embedding host (links the sim hwio backend)
#   build/guest.elf      the RV32IMC guest that pokes GPIO/I2C/ADC
#
# Unlike most examples this host links two extra sources that are NOT in
# VM_CORE_SRCS (so the core stays untouched for other builds):
#   src/vm/vm_host_hwio.c        the ecall handlers + vm_host_install_hwio
#   src/host/platform_hwio_sim.c the fake GPIO bank + I2C temp sensor

set -e
EXAMPLE_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$EXAMPLE_DIR"
. ../common/vm_objs.sh

BUILD_DIR="$EXAMPLE_DIR/build"

case "${1:-build}" in
    clean) rm -rf "$BUILD_DIR"; echo "10_hwio_sim: cleaned"; exit 0 ;;
esac

mkdir -p "$BUILD_DIR"

HWIO_HOST_SRCS=(
    "${REPO_ROOT}/src/vm/vm_host_hwio.c"
    "${REPO_ROOT}/src/host/platform_hwio_sim.c"
)

echo "10_hwio_sim: compiling host..."
"$CC" "${CFLAGS[@]}" \
    -o "$BUILD_DIR/host" \
    "$EXAMPLE_DIR/host.c" \
    "${VM_CORE_SRCS[@]}" \
    "${HWIO_HOST_SRCS[@]}"

if have_guest_cc; then
    echo "10_hwio_sim: compiling guest (RV32IMC)..."
    "$GUEST_CC" "${GUEST_CFLAGS[@]}" \
        -I"${REPO_ROOT}/examples/common/guest" \
        -I"${REPO_ROOT}/examples/common/guest/include" \
        -Wl,-T,"$(guest_path "$GUEST_LD")" \
        -o "$(guest_path "$BUILD_DIR/guest.elf")" \
        "$(guest_path "$EXAMPLE_DIR/guest.c")"
else
    echo "10_hwio_sim: WARNING — $GUEST_CC not found; cannot build guest.elf"
fi

echo "10_hwio_sim: built. To run:"
echo "    $BUILD_DIR/host"

if [ "${1:-}" = "run" ]; then
    echo
    echo "10_hwio_sim: ===== running ====="
    "$BUILD_DIR/host"
fi
