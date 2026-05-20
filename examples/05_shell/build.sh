#!/bin/bash
# 05_shell/build.sh — build the host and guest for the shell example.
#
# Unlike the other examples, this one needs FatFs to be extracted
# into third_party/fatfs/source/. If FatFs is missing, the host
# can't link (it pulls in f_open/f_read/etc.) and the script will
# fail with a clear message.

set -e

EXAMPLE_DIR=$(cd "$(dirname "$0")" && pwd)
cd "$EXAMPLE_DIR"
. ../common/vm_objs.sh

BUILD_DIR="$EXAMPLE_DIR/build"

case "${1:-build}" in
    clean)
        rm -rf "$BUILD_DIR"
        echo "05_shell: cleaned"
        exit 0
        ;;
esac

# Check that FatFs is present.
FATFS_DIR="$REPO_ROOT/third_party/fatfs"
FATFS_SOURCE="$FATFS_DIR/source"
FATFS_FF_C="$FATFS_SOURCE/ff.c"

if [ ! -f "$FATFS_FF_C" ]; then
    echo "05_shell: ERROR — FatFs source not found at $FATFS_FF_C"
    echo ""
    echo "  This example needs FatFs to be downloaded and extracted."
    echo "  See third_party/fatfs/PLACEHOLDER.md for instructions."
    echo ""
    echo "  Other examples (01_hello, 02_counter, 03_mailbox,"
    echo "  04_keydump) don't need FatFs and will build fine without it."
    exit 1
fi

mkdir -p "$BUILD_DIR"

# Build the host. We need:
#   - The VM library (VM_CORE_SRCS already includes vm_host_stdio,
#     vm_host_fs is added below)
#   - trashdrive.c and trashdrive_fatfs.c
#   - FatFs's ff.c and ffsystem.c
echo "05_shell: compiling host (with FatFs)..."
"$CC" "${CFLAGS[@]}" \
    -DHAVE_FATFS \
    -I"$FATFS_DIR" -I"$FATFS_SOURCE" \
    -o "$BUILD_DIR/host" \
    "$EXAMPLE_DIR/host.c" \
    "${VM_CORE_SRCS[@]}" \
    "$REPO_ROOT/src/vm/vm_host_fs.c" \
    "$REPO_ROOT/src/storage/trashdrive.c" \
    "$REPO_ROOT/src/storage/trashdrive_fatfs.c" \
    "$FATFS_SOURCE/ff.c" \
    "$FATFS_SOURCE/ffsystem.c"

# Build the guest.
if have_guest_cc; then
    echo "05_shell: compiling guest (RV32IMC)..."
    "$GUEST_CC" "${GUEST_CFLAGS[@]}" \
        -Wl,-T,"$GUEST_LD" \
        -o "$BUILD_DIR/shell.elf" \
        "$EXAMPLE_DIR/shell.c"
else
    if [ -f "$BUILD_DIR/shell.elf" ]; then
        echo "05_shell: $GUEST_CC not found, using existing shell.elf"
    else
        echo "05_shell: WARNING — $GUEST_CC not found and no shell.elf exists"
        echo "05_shell:   install gcc-riscv64-unknown-elf or copy a prebuilt"
        echo "05_shell:   shell.elf into $BUILD_DIR"
    fi
fi

echo "05_shell: built. To run:"
echo "    $BUILD_DIR/host"
echo ""
echo "  Type 'help' once inside the shell for a command list."

if [ "${1:-}" = "run" ]; then
    echo ""
    echo "05_shell: ===== running ====="
    "$BUILD_DIR/host"
fi
