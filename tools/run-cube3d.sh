#!/usr/bin/env bash
# ============================================================
#  run-cube3d.sh -- launch bsnes-plus straight into the 60-colour 3D cube
#  (demo_cube3d) on the framebuffer VECTOR-SWAP kernel path (dead-simple-kernel).
#
#  Autostarts /td0/demos/cube3d.elf and turns on the r3d DMA trace so the band
#  deliveries land in mgdma.log (useful to confirm the copro keeps feeding even
#  if the screen is black -> isolates a kernel-side vs copro-side bug).
#
#  Assumes the DLL is already built (build-mgapi.ps1). Pass --no-build here to
#  skip the redundant kernel reassemble too; drop it to let run-bsnes rebuild
#  snes_boot.bin (the DLL still uses its own baked copy either way).
#
#  Usage:
#    ./tools/run-cube3d.sh                 # autostart cube3d + trace
#    ./tools/run-cube3d.sh --no-build      # skip snes/build.ps1
#    ./tools/run-cube3d.sh --bsnes-dir /c/bsnes-plus
#  Any extra args pass straight through to run-bsnes.sh.
#
#  Public domain (CC0). No warranty.
# ============================================================
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

export MGAPI_AUTOSTART="/td0/demos/cube3d.elf"
# MGAPI_AUTOSTART is a VM (trashfs) path, not a Windows path. Without this,
# MSYS rewrites the leading-slash value to a Windows path when it spawns the
# Windows bsnes.exe, so the autostart file is never found -> blank screen.
export MSYS2_ENV_CONV_EXCL="MGAPI_AUTOSTART;MG_ISR;MG_FB"
export MSYS_NO_PATHCONV=1

# Full-emitter ISR path (docs/emitter-kernel.md): the coprocessor bakes the whole
# H/V virtual-NMI and the SNES kernel just runs it. Set MG_ISR=0 to fall back to
# the state-machine path, or MG_FB=1 for the (abandoned) descriptor fb mode.
export MG_ISR="${MG_ISR:-1}"

echo "run-cube3d: MGAPI_AUTOSTART=$MGAPI_AUTOSTART"
echo "run-cube3d: trace -> mgdma.log (tail it for [r3d] deliver lines)"
exec "$here/run-bsnes.sh" --trace "$@"
