#!/usr/bin/env bash
# build.sh — build game.elf for the microgarbage RV32IMC VM.
#
# Pipeline (when the asset tools are in place):
#
#   assets/*.png  --tools/png_to_chr-->  generated/*.chr + .pal
#   .chr + .pal   --tools/bin2c-->        generated/*_chr.c + *_pal.c
#   game.c + generated/*.c
#                 --examples/common/vm_objs.sh-->  build/game.elf
#
# Until tools/png_to_chr lands, only the game.c -> game.elf step
# runs. game.c's boot() has the asset uploads commented out and
# paints a solid amber palette entry as a stand-in.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
BUILD="$HERE/build"
GEN="$HERE/generated"

mkdir -p "$BUILD" "$GEN"

# Pull in the shared guest toolchain settings (GUEST_CC, GUEST_CFLAGS,
# GUEST_LD).
# shellcheck source=../common/vm_objs.sh
source "$REPO/examples/common/vm_objs.sh"

if ! command -v "$GUEST_CC" >/dev/null 2>&1; then
    echo "build.sh: $GUEST_CC not on PATH; install the riscv32-unknown-elf"
    echo "          (or riscv64-unknown-elf) cross-compiler and retry." >&2
    exit 1
fi

# Asset pipeline — runs if tools/png_to_chr exists.
if [ -x "$REPO/tools/png_to_chr" ]; then
    for png in "$HERE"/assets/*.png; do
        [ -f "$png" ] || continue   # no assets yet, skip cleanly
        n=$(basename "$png" .png)
        "$REPO/tools/png_to_chr" --bpp=4 --palette=multi \
            "$png" "$GEN/${n}.chr" "$GEN/${n}.pal"
        "$REPO/tools/bin2c" "${n}_chr" "$GEN/${n}.chr" > "$GEN/${n}_chr.c"
        "$REPO/tools/bin2c" "${n}_pal" "$GEN/${n}.pal" > "$GEN/${n}_pal.c"
        cat > "$GEN/${n}_chr.h" <<EOF
#pragma once
#include <stdint.h>
extern const uint8_t  ${n}_chr[];
extern const unsigned ${n}_chr_len;
EOF
        cat > "$GEN/${n}_pal.h" <<EOF
#pragma once
#include <stdint.h>
extern const uint8_t  ${n}_pal[];
extern const unsigned ${n}_pal_len;
EOF
    done
else
    echo "build.sh: tools/png_to_chr not built yet; skipping asset pipeline."
fi

# Collect sources: game.c + every mg_*.c the guest library exposes +
# anything in generated/ from the asset pipeline (if any).
GUEST_LIB="$REPO/examples/common/guest"

MG_SRCS=(
    "$GUEST_LIB/mg_panic.c"
    "$GUEST_LIB/mg_frame.c"
    "$GUEST_LIB/mg_input.c"
    "$GUEST_LIB/mg_sprite.c"
    "$GUEST_LIB/mg_actor.c"
    "$GUEST_LIB/mg_gfx.c"
    "$GUEST_LIB/mg_bg.c"
    "$GUEST_LIB/mg_mode7.c"
    "$GUEST_LIB/mg_hdma.c"
    "$GUEST_LIB/mg_audio.c"
)

GEN_SRCS=()
for c in "$GEN"/*.c; do
    [ -f "$c" ] && GEN_SRCS+=("$c")
done

echo "build.sh: compiling game.elf..."
"$GUEST_CC" "${GUEST_CFLAGS[@]}" \
    -I "$GUEST_LIB" \
    -T "$GUEST_LD" \
    -o "$BUILD/game.elf" \
    "$HERE/game.c" \
    "${MG_SRCS[@]}" \
    ${GEN_SRCS[@]+"${GEN_SRCS[@]}"}

echo "build.sh: done. Output:"
echo "          $BUILD/game.elf"
