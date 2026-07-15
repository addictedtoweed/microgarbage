#!/bin/bash
# 11_run/build.sh — the universal guest runner + a libc sample built with rvcc.
#
#   ./build.sh          build the runner (build/run) and hello.elf (via rvcc)
#   ./build.sh clean    remove build/
#   ./build.sh run      build, then run hello.elf in the runner
#
# The runner links the sim hardware-IO backend + handlers (not in
# VM_CORE_SRCS) and the host platform layer (time/sleep). The guest is
# compiled with tools/rvcc — dogfooding the dev-kit compiler.

set -e
EXAMPLE_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$EXAMPLE_DIR"
. ../common/vm_objs.sh

BUILD_DIR="$EXAMPLE_DIR/build"
case "${1:-build}" in
    clean) rm -rf "$BUILD_DIR"; echo "11_run: cleaned"; exit 0 ;;
esac
mkdir -p "$BUILD_DIR"

echo "11_run: compiling runner..."
"$CC" "${CFLAGS[@]}" \
    -o "$BUILD_DIR/run" \
    "$EXAMPLE_DIR/host.c" \
    "${VM_CORE_SRCS[@]}" \
    "${REPO_ROOT}/src/vm/vm_host_hwio.c" \
    "${REPO_ROOT}/src/host/platform_hwio_sim.c" \
    "$HOST_PLATFORM_SRC"

if have_guest_cc; then
    echo "11_run: compiling hello.elf with rvcc..."
    bash "${REPO_ROOT}/tools/rvcc" -o "$BUILD_DIR/hello.elf" "$EXAMPLE_DIR/hello.c"
else
    echo "11_run: WARNING — no RISC-V cross; cannot build hello.elf"
fi

echo "11_run: built. To run:"
echo "    $BUILD_DIR/run $BUILD_DIR/hello.elf"

if [ "${1:-}" = "run" ]; then
    echo
    echo "11_run: ===== running ====="
    "$BUILD_DIR/run" "$BUILD_DIR/hello.elf"
fi
