#!/usr/bin/env bash
# build-bsnes-integration.sh
#
# Build mgapi.dll, stage the runtime DLLs into bsnes-plus/out/, and
# build bsnes-plus. Assumes:
#
#   <repos>/microgarbage/          (this script lives here)
#   <repos>/bsnes-plus/bsnes/      (bsnes-plus clone)
#
# Run from MSYS2 MinGW64 shell. Override paths with:
#   MGAPI_ROOT=/path/to/microgarbage BSNES_ROOT=/path/to/bsnes-plus/bsnes \
#     ./build-bsnes-integration.sh
#
# Usage:
#   ./build-bsnes-integration.sh           # build + stage + build bsnes
#   ./build-bsnes-integration.sh --run     # also launch bsnes at the end
#   ./build-bsnes-integration.sh --no-bsnes        # skip the bsnes build
#   ./build-bsnes-integration.sh --no-mgapi        # skip the mgapi.dll build
#   ./build-bsnes-integration.sh --clean   # clean before building
#
# Combine flags freely, e.g.:
#   ./build-bsnes-integration.sh --clean --run

set -e

# ----------------------------------------------------------------
# Resolve paths relative to this script (the microgarbage root).
# ----------------------------------------------------------------
script_dir="$(cd "$(dirname "$0")" && pwd)"
MGAPI_ROOT="${MGAPI_ROOT:-$script_dir}"
BSNES_ROOT="${BSNES_ROOT:-$script_dir/../bsnes-plus/bsnes}"

# ----------------------------------------------------------------
# Args
# ----------------------------------------------------------------
do_run=0
do_clean=0
skip_bsnes=0
skip_mgapi=0
for arg in "$@"; do
    case "$arg" in
        --run)        do_run=1 ;;
        --clean)      do_clean=1 ;;
        --no-bsnes)   skip_bsnes=1 ;;
        --no-mgapi)   skip_mgapi=1 ;;
        --help|-h)
            sed -n '2,/^$/p' "$0"
            exit 0
            ;;
        *)
            echo "unknown arg: $arg (try --help)" >&2
            exit 1
            ;;
    esac
done

step() { echo; echo "== $* =="; }

# ----------------------------------------------------------------
# Sanity-check the toolchain.
# ----------------------------------------------------------------
step "Checking toolchain"
for tool in powershell.exe mingw32-make gcc; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "missing tool: $tool" >&2
        echo "are you in the MSYS2 MinGW64 shell?" >&2
        exit 1
    fi
done
echo "ok: powershell.exe, mingw32-make, gcc on PATH"

# ----------------------------------------------------------------
# Build mgapi.dll
# ----------------------------------------------------------------
if [ "$skip_mgapi" = 0 ]; then
    step "Building mgapi.dll"
    cd "$MGAPI_ROOT"
    if [ "$do_clean" = 1 ]; then
        powershell.exe -ExecutionPolicy Bypass -File build-mgapi.ps1 -Clean
    fi

    powershell.exe -ExecutionPolicy Bypass -File build-mgapi.ps1

    # Hard fail if the build didn't produce a DLL — powershell.exe doesn't
    # reliably propagate build-mgapi.ps1's exit code through MSYS's
    # `set -e`, so a hard build failure (toolchain missing, etc.) used
    # to let the `cp` step below ship a stale build/mgapi/mgapi.dll into
    # bsnes-plus without complaint. (We don't flag "DLL exists but mtime
    # unchanged" — that's the normal no-op case when sources haven't
    # changed.)
    if [ ! -f "$MGAPI_ROOT/build/mgapi/mgapi.dll" ]; then
        echo "ERROR: build-mgapi.ps1 didn't produce build/mgapi/mgapi.dll" >&2
        echo "  check that the MSYS2 mingw64 cross toolchain is on PATH" >&2
        exit 1
    fi
else
    step "Skipping mgapi.dll build (--no-mgapi)"
fi

# ----------------------------------------------------------------
# Stage the runtime DLLs into bsnes-plus/out/.
# ----------------------------------------------------------------
if [ "$skip_bsnes" = 0 ]; then
    step "Staging runtime DLLs into $BSNES_ROOT/out"
    mkdir -p "$BSNES_ROOT/out"
    cp -v "$MGAPI_ROOT/build/mgapi/mgapi.dll"           "$BSNES_ROOT/out/"
    cp -v "$MGAPI_ROOT/build/mgapi/libgcc_s_seh-1.dll"  "$BSNES_ROOT/out/"
    cp -v "$MGAPI_ROOT/build/mgapi/libwinpthread-1.dll" "$BSNES_ROOT/out/"

    # Sync the Mgapi cart class sources into the bsnes-plus tree. The
    # source-of-truth lives in microgarbage/snes/bsnes/ so it travels
    # with the rest of the SNES integration; bsnes-plus expects them
    # at snes/chip/mgapi/ (lowercase). Without this sync, edits to
    # Mgapi.cpp/hpp don't reach the bsnes-plus build and the runtime
    # silently regresses (e.g., a try_load() failure with no diagnostic
    # produced the "snes_smoke.sfc shows the 65816 menu instead of the
    # mgapi runtime" symptom during the v1.83 session). See
    # [[bsnes-plus-rebuild]] memory.
    step "Syncing Mgapi cart-class sources into bsnes-plus tree"
    mkdir -p "$BSNES_ROOT/snes/chip/mgapi"
    cp -v "$MGAPI_ROOT/snes/bsnes/Mgapi.cpp" "$BSNES_ROOT/snes/chip/mgapi/mgapi.cpp"
    cp -v "$MGAPI_ROOT/snes/bsnes/Mgapi.hpp" "$BSNES_ROOT/snes/chip/mgapi/mgapi.hpp"
fi

# ----------------------------------------------------------------
# Build bsnes-plus.
# ----------------------------------------------------------------
if [ "$skip_bsnes" = 0 ]; then
    step "Building bsnes-plus"
    cd "$BSNES_ROOT"
    if [ "$do_clean" = 1 ]; then
        mingw32-make clean || true
    fi
    mingw32-make -j4
else
    step "Skipping bsnes-plus build (--no-bsnes)"
fi

# ----------------------------------------------------------------
# Launch (optional).
# ----------------------------------------------------------------
if [ "$do_run" = 1 ]; then
    step "Launching bsnes-plus"
    cd "$BSNES_ROOT/out"
    # MGAPI_ROM_SELECT picks which baked-in ROM mgapi serves through the
    # cart bus. "boot" = boot.s + kernel.s runtime; this is what the
    # guest-VM demos (audio_mixer.elf, mode7_3d.elf, ...) need — the
    # kernel walks the DMA slot list mgapi stages each vblank and
    # forwards it to the PPU. "smoke" = the standalone 65816 SELECT
    # DEMO menu (no copro path; ignores everything the guest stages).
    # Run-bsnes.ps1 sets the same default. Override before invoking the
    # script if you want the smoke menu.
    : "${MGAPI_ROM_SELECT:=boot}"
    export MGAPI_ROM_SELECT
    echo "MGAPI_ROM_SELECT=$MGAPI_ROM_SELECT"
    ./bsnes.exe
fi

# ----------------------------------------------------------------
# Done.
# ----------------------------------------------------------------
echo
echo "Done."
echo "  mgapi.dll:   $MGAPI_ROOT/build/mgapi/mgapi.dll"
echo "  bsnes.exe:   $BSNES_ROOT/out/bsnes.exe"
echo
echo "To run:"
echo "  cd \"$BSNES_ROOT/out\" && ./bsnes.exe"
echo
echo "To capture stderr to a log (helps diagnose any hangs):"
echo "  cd \"$BSNES_ROOT/out\" && ./bsnes.exe 2> mgapi.log"
