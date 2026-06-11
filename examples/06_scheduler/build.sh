#!/bin/bash
# 06_scheduler/build.sh — build the preemptive-scheduler demo.
#
# Unlike examples 01-05, this demo runs the NATIVE preemptive scheduler
# directly (no guest VM, no cross-compiler needed). It just compiles the
# demo host against the scheduler source.
#
# Usage:
#   ./build.sh           # build
#   ./build.sh clean     # remove build/
#   ./build.sh run       # build and run
#
# Output:
#   build/scheduler_demo
#
# Requirements:
#   - A C compiler (cc/gcc/clang; set $CC to override)
#   - POSIX threads + realtime timer (-lpthread -lrt) on Linux.
#     On native Windows the scheduler uses Win32 threads instead; build
#     there with the mingw toolchain (the link flags differ — no -lrt).

set -e

EXAMPLE_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$EXAMPLE_DIR"

# repo root is two levels up (examples/06_scheduler -> repo)
REPO_ROOT=$(cd "$EXAMPLE_DIR/../.." && pwd)
BUILD_DIR="$EXAMPLE_DIR/build"

CC="${CC:-cc}"

case "${1:-build}" in
    clean)
        rm -rf "$BUILD_DIR"
        echo "06_scheduler: cleaned"
        exit 0
        ;;
esac

mkdir -p "$BUILD_DIR"

echo "06_scheduler: compiling scheduler demo..."

# Platform link flags: POSIX needs pthread + rt; detect a non-Windows host.
LINK_FLAGS="-lpthread"
case "$(uname -s 2>/dev/null)" in
    *NT*|*MINGW*|*MSYS*) ;;   # Windows-ish: Win32 threads, no -lrt
    *) LINK_FLAGS="$LINK_FLAGS -lrt" ;; # Linux/BSD: realtime timer
esac

"$CC" -std=c11 -Wall -Wextra \
    -I"$REPO_ROOT/include" \
    -o "$BUILD_DIR/scheduler_demo" \
    "$EXAMPLE_DIR/host.c" \
    "$REPO_ROOT/src/vm/presched.c" \
    $LINK_FLAGS

echo "06_scheduler: built. To run:"
echo "    $BUILD_DIR/scheduler_demo"

if [ "${1:-}" = "run" ]; then
    echo
    echo "06_scheduler: ===== running ====="
    "$BUILD_DIR/scheduler_demo"
fi
