#!/bin/bash
# package-guest-sdk.sh — assemble a standalone, copyable guest SDK bundle.
#
# Vendors the shared guest runtime + host-hook headers + mini-libc +
# guest.ld + the guest-safe containers/math into one self-contained
# directory, alongside a skeleton main.c and a cross-only build script.
# The result can be copied anywhere and built with just a RISC-V cross.
#
#   ./package-guest-sdk.sh [OUT_DIR]      (default: build/guest_sdk)
#
# Single source of truth: everything is COPIED from the repo, so the
# bundle never drifts — re-run this to refresh it.
set -e
REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
SDK="$REPO_ROOT/examples/common/guest"
TMPL="$REPO_ROOT/examples/common/guest_template"
OUT="${1:-$REPO_ROOT/build/guest_sdk}"

rm -rf "$OUT"
mkdir -p "$OUT/include/containers" "$OUT/include/math" \
         "$OUT/src/containers" "$OUT/src/math"

# Template: skeleton main.c, build scripts, README.
cp "$TMPL/main.c" "$TMPL/build.sh" "$TMPL/build.ps1" "$TMPL/README.md" "$OUT/"
chmod +x "$OUT/build.sh"

# Linker script.
cp "$REPO_ROOT/examples/common/guest.ld" "$OUT/guest.ld"

# SDK: host-hook headers + mini-libc headers + runtime sources.
cp "$SDK/vm_runtime.h" "$SDK/audio.h" "$SDK/tui.h" "$SDK/fs.h" "$OUT/include/"
cp "$SDK"/include/*.h "$OUT/include/"
cp "$SDK/vm_runtime.c" "$SDK/tui.c" "$OUT/src/"

# Guest-safe tools: containers + math (headers + sources).
cp "$REPO_ROOT"/include/containers/*.h "$OUT/include/containers/"
cp "$REPO_ROOT"/include/math/*.h       "$OUT/include/math/"
cp "$REPO_ROOT"/src/containers/*.c     "$OUT/src/containers/"
cp "$REPO_ROOT"/src/math/*.c           "$OUT/src/math/"

echo "package-guest-sdk: bundle ready -> $OUT"
echo "  copy it anywhere, then:  cd '$OUT' && ./build.sh"
