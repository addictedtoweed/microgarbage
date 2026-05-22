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
        rm -rf "$EXAMPLE_DIR/host_files"
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
#   - On native Windows: -lws2_32 for the TCP transport's WinSock
#     calls. Linux/Cygwin pull BSD sockets from libc; no extra
#     library needed.
echo "05_shell: compiling host (with FatFs)..."
HOST_LIBS=()
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*) HOST_LIBS+=(-lws2_32) ;;
esac
"$CC" "${CFLAGS[@]}" \
    -DHAVE_FATFS \
    -I"$FATFS_DIR" -I"$FATFS_SOURCE" \
    -o "$BUILD_DIR/host" \
    "$EXAMPLE_DIR/host.c" \
    "${VM_CORE_SRCS[@]}" \
    "$HOST_PLATFORM_SRC" \
    "$REPO_ROOT/src/vm/vm_host_fs.c" \
    "$REPO_ROOT/src/storage/trashdrive.c" \
    "$REPO_ROOT/src/storage/trashdrive_fatfs.c" \
    "$REPO_ROOT/src/util/inicfg.c" \
    "$FATFS_DIR/ff_wrapped.c" \
    "$FATFS_SOURCE/ffsystem.c" \
    "${HOST_LIBS[@]}"

# Build the guest.
if have_guest_cc; then
    echo "05_shell: compiling guest (RV32IMC)..."
    "$GUEST_CC" "${GUEST_CFLAGS[@]}" \
        -Wl,-T,"$(guest_path "$GUEST_LD")" \
        -o "$(guest_path "$BUILD_DIR/shell.elf")" \
        "$(guest_path "$EXAMPLE_DIR/shell.c")"

    # Build the sample spawnable guests. These get dropped into
    # ./host_files/ so they're accessible from inside the shell
    # via "/host/<name>.elf". The shell's host process mounts
    # ./host_files/ as /host by default, auto-creating the
    # directory if it doesn't exist.
    #
    # Each top-level .c in host_files_src/ becomes one guest ELF.
    # Sources under host_files_src/lib/ are library code linked
    # into every guest (small enough that we don't bother building
    # static-archive form). Guests that don't actually call any
    # library symbols just leave the dead code in place — the
    # linker doesn't strip it but the cost is negligible (a few
    # KB per ELF).
    HOST_FILES_DIR="$EXAMPLE_DIR/host_files"
    mkdir -p "$HOST_FILES_DIR"

    # Gather library sources (host_files_src/lib/*.c).
    #
    # Each guest .c links against all library .c files. Most of
    # the library is small functions and the linker strips unused
    # ones because we add -ffunction-sections / -fdata-sections /
    # -Wl,--gc-sections below — so a guest that never calls
    # tui_init pays only a few bytes overhead instead of the full
    # library's ~6 KB.
    GUEST_LIB_SRCS=()
    GUEST_GC_CFLAGS=(-ffunction-sections -fdata-sections)
    # -z max-page-size=4 collapses LOAD segment alignment from
    # the default 4 KB to effectively none. Our loader has no
    # MMU and doesn't care about page boundaries; the alignment
    # padding was costing ~8 KB per ELF for a 3-segment layout.
    # -s strips the symbol table, which on snake.elf is another
    # ~1.2 KB of debug names we don't need at runtime.
    GUEST_GC_LDFLAGS=(-Wl,--gc-sections -Wl,-z,max-page-size=4 -Wl,-s)
    if [ -d "$EXAMPLE_DIR/host_files_src/lib" ]; then
        for libsrc in "$EXAMPLE_DIR"/host_files_src/lib/*.c; do
            [ -f "$libsrc" ] || continue
            GUEST_LIB_SRCS+=("$(guest_path "$libsrc")")
        done
    fi

    for src in "$EXAMPLE_DIR"/host_files_src/*.c; do
        [ -f "$src" ] || continue
        name=$(basename "$src" .c)
        echo "05_shell: compiling host_files/$name.elf (spawnable)..."
        "$GUEST_CC" "${GUEST_CFLAGS[@]}" "${GUEST_GC_CFLAGS[@]}" \
            -I"$(guest_path "$EXAMPLE_DIR/host_files_src")" \
            -I"$(guest_path "$EXAMPLE_DIR/host_files_src/lib/include")" \
            -Wl,-T,"$(guest_path "$GUEST_LD")" \
            "${GUEST_GC_LDFLAGS[@]}" \
            -o "$(guest_path "$HOST_FILES_DIR/$name.elf")" \
            "$(guest_path "$src")" "${GUEST_LIB_SRCS[@]}"
    done
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
