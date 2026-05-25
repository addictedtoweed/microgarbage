#!/bin/bash
# build-win.sh — build the microgarbage shell host as a NATIVE
# Windows .exe from a Cygwin or MSYS2 shell, using mingw-w64.
#
# This is the bash counterpart to build-win.ps1. Both produce the
# SAME artifact: a self-contained native Windows host that does NOT
# depend on cygwin1.dll and uses the Win32 code paths (WinSock2,
# SetConsoleMode, etc.) — i.e. the `_WIN32 && !__CYGWIN__` branches.
#
# Why a separate script from build.sh:
#   build.sh uses Cygwin's own gcc on purpose (POSIX paths, the
#   pty/tty layer for running under Cygwin's sshd, and the unit
#   tests). That binary links cygwin1.dll and exercises the POSIX
#   code paths. This script instead drives mingw-w64 to emit the
#   shippable native binary. The compiler — not the shell — decides
#   the target, so running this from Cygwin still yields native.
#
# Usage:
#   ./build-win.sh            build native host.exe + guest ELFs (RELEASE)
#   ./build-win.sh --release  size-optimized, stripped (default)
#   ./build-win.sh --debug    -Og -g3, symbols + asserts, unstripped
#   ./build-win.sh --no-guest host only (skip RISC-V guests)
#   ./build-win.sh clean      remove build artifacts
#
# Flags combine, e.g.  ./build-win.sh --debug --no-guest
#
# Build modes (host.exe):
#   release  -Os -DNDEBUG -s   smallest distributable (default)
#   debug    -Og -g3 -DDEBUG   full symbols, assertions on, frame ptrs
# Spawnable guest ELFs follow the mode too (release: -Os stripped;
# debug: -Og -g, symbols kept). The embedded shell is always size-built.
#
# Override compilers via env:
#   CC=x86_64-w64-mingw32-gcc        (host; must be mingw-w64)
#   GUEST_CC=riscv-none-elf-gcc      (guest RISC-V cross)

set -e

REPO_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
EXAMPLE_DIR="$REPO_ROOT/examples/05_shell"
BUILD_DIR="$EXAMPLE_DIR/build"
HOST_FILES="$EXAMPLE_DIR/host_files"

step() { echo "build-win: $*"; }
die()  { echo "build-win: ERROR - $*" >&2; exit 1; }

NO_GUEST=0
# Build mode: release (default) or debug. RELEASE=0 in the env flips the
# default to debug (back-compat: RELEASE=1 / unset stays release); an
# explicit --debug/--release flag always wins.
BUILD_MODE="release"
[ "${RELEASE:-1}" = "0" ] && BUILD_MODE="debug"
for arg in "$@"; do
    case "$arg" in
        clean)
            rm -rf "$BUILD_DIR" "$HOST_FILES"
            step "cleaned"
            exit 0
            ;;
        sdk)
            # Generate the standalone, copyable guest SDK bundle and exit.
            step "packaging guest SDK bundle..."
            "$REPO_ROOT/examples/common/package-guest-sdk.sh"
            exit 0
            ;;
        --no-guest)        NO_GUEST=1 ;;
        --debug|-d)        BUILD_MODE="debug" ;;
        --release|-r)      BUILD_MODE="release" ;;
        build|"")          ;;   # no-op (default action)
        *) die "unknown argument '$arg'
      use: --debug | --release | --no-guest | clean | sdk" ;;
    esac
done

# Warnings-as-errors, ON by default (enforces the zero-warning state).
# Opt out with WERROR=0 if a stricter/newer mingw flags something we
# don't see — you'll get a building host.exe and visible warnings to
# report. See CONTRIBUTING.md.
WERROR_FLAG=()
if [ "${WERROR:-1}" != "0" ]; then
    WERROR_FLAG=(-Werror)
fi

# Mode flag sets. release: size-optimized + stripped host.exe (-s removes
# ~180 KB of DWARF that mingw emits by default) + size-built spawnable
# guests. debug: -Og -g3 with symbols, assertions on (no -DNDEBUG), and
# guest ELFs left unstripped with -g. The embedded shell is size-built
# regardless of mode (it's a baked image; size matters, debug it standalone).
if [ "$BUILD_MODE" = "debug" ]; then
    step "DEBUG build — host -Og -g3 (symbols + asserts), guests -Og -g"
    HOST_OPT=(-Og -g3 -DDEBUG)
    HOST_STRIP=()
    GUEST_OPT=(-Og -g)
    GUEST_STRIP=()
else
    step "RELEASE build — host -Os -DNDEBUG -s (stripped), guests -Os stripped"
    HOST_OPT=(-Os -DNDEBUG)
    HOST_STRIP=(-s)
    GUEST_OPT=(-Os)
    GUEST_STRIP=(-Wl,-s)
fi

# Host compiler: default to mingw-w64. Must target native Windows.
CC="${CC:-x86_64-w64-mingw32-gcc}"
if ! command -v "$CC" >/dev/null 2>&1; then
    die "host compiler '$CC' not found.
      Install mingw-w64:
        - Cygwin: setup package 'mingw64-x86_64-gcc-core'
        - MSYS2:  pacman -S mingw-w64-x86_64-gcc
      or set CC=<compiler>."
fi
MACHINE=$("$CC" -dumpmachine 2>/dev/null || echo unknown)
case "$MACHINE" in
    *mingw*|*w64*windows*)
        step "host compiler: $CC (target $MACHINE) - native Windows OK" ;;
    *)
        step "WARNING: $CC targets '$MACHINE', not obviously native Windows."
        step "         a native build wants mingw-w64; continuing anyway." ;;
esac

mkdir -p "$BUILD_DIR"

# Host source set — read from the shared manifest (host_sources.txt) so
# build-win.sh and build-win.ps1 can never drift. One list, both scripts.
HOST_SRCS=()
while IFS= read -r _line || [ -n "$_line" ]; do
    _line="${_line%%#*}"                                  # strip comment
    _line="$(printf '%s' "$_line" | tr -d '[:space:]')"   # trim whitespace
    [ -n "$_line" ] || continue
    HOST_SRCS+=("$REPO_ROOT/$_line")
done < "$EXAMPLE_DIR/host_sources.txt"

# ------------------------------------------------------------------
# Bake the guest shell into host.exe (XIP-executed at runtime). The
# host embeds build/shell.elf as a C array via tools/bin2c, so a
# distributed host.exe needs no external .elf. shell.elf must exist
# before the host compile; if guests are skipped or the cross isn't
# present, emit an empty stub (host then needs an explicit ELF path).
# ------------------------------------------------------------------
GEN_DIR="$BUILD_DIR/gen"
mkdir -p "$GEN_DIR"
SHELL_DATA_C="$GEN_DIR/shell_elf_data.c"
_baked=0
if [ "$NO_GUEST" != "1" ]; then
    # Resolve the guest cross (sourcing vm_objs.sh sets GUEST_CC and
    # guest_path()); host vars were already set above and we don't use
    # them here.
    # shellcheck disable=SC1091
    . "$REPO_ROOT/examples/common/vm_objs.sh" >/dev/null 2>&1 || true
    if command -v "$GUEST_CC" >/dev/null 2>&1; then
        step "compiling guest shell.elf for embedding (RV32IMC)..."
        _GLD="$REPO_ROOT/examples/common/guest.ld"
        # Build the embedded shell for size: -Os + gc-sections strips
        # unused guest-runtime code, page-size=4 drops segment-align
        # padding, -s strips symbols. Safe for XIP (segments stay
        # >=4-byte aligned; bin2c aligns the array to 4). Mirrors the
        # POSIX build.sh shell flags.
        _GCF=(-march=rv32imc -mabi=ilp32 -nostdlib -nostartfiles
              -ffreestanding -Os -ffunction-sections -fdata-sections)
        _GLDF=(-Wl,--gc-sections -Wl,-z,max-page-size=4 -Wl,-s)
        "$GUEST_CC" "${_GCF[@]}" "${_GLDF[@]}" -Wl,-T,"$(guest_path "$_GLD")" \
            -o "$(guest_path "$BUILD_DIR/shell.elf")" \
            "$(guest_path "$EXAMPLE_DIR/shell.c")"
        step "baking shell.elf into host.exe (bin2c)..."
        # bin2c is a BUILD-TIME tool — it must run on the build
        # machine, so compile it with a native host compiler, NOT $CC
        # (which here is the mingw cross and would produce a Windows
        # exe that can't run during a Linux/Cygwin build). Prefer an
        # explicit BUILD_CC, else cc, else gcc.
        BUILD_CC="${BUILD_CC:-cc}"
        command -v "$BUILD_CC" >/dev/null 2>&1 || BUILD_CC=gcc
        "$BUILD_CC" -O2 -o "$BUILD_DIR/bin2c" "$EXAMPLE_DIR/tools/bin2c.c"
        "$BUILD_DIR/bin2c" "$BUILD_DIR/shell.elf" shell_elf "$SHELL_DATA_C"
        _baked=1
    fi
fi
if [ "$_baked" != "1" ]; then
    step "no embedded shell (--no-guest or cross missing) — empty stub"
    printf '#include <stddef.h>\nconst unsigned char shell_elf[] = {0};\nconst size_t shell_elf_len = 0;\n' \
        > "$SHELL_DATA_C"
fi

# Preemptive scheduler build (opt-in): GARBAGE_PREEMPTIVE=1 selects the
# preemptive backend — adds the mode define, links presched.c, and pulls
# in pthreads for the mailbox/slab mutex lockers. Default is cooperative.
PREEMPT_CFLAGS=()
PREEMPT_SRCS=()
PREEMPT_LIBS=()
if [ "${GARBAGE_PREEMPTIVE:-0}" = "1" ]; then
    step "PREEMPTIVE backend (GARBAGE_SCHED_MODE=1, +presched.c +pthread)"
    PREEMPT_CFLAGS=(-DGARBAGE_SCHED_MODE=1)
    PREEMPT_SRCS=("$REPO_ROOT/src/vm/presched.c")
    PREEMPT_LIBS=(-lpthread)
fi

step "compiling native host.exe..."
# -D__USE_MINGW_ANSI_STDIO=1: msvcrt's printf doesn't understand C99
# %z/%ll length modifiers, so mingw warns on every %zu (size_t). This
# selects mingw's own C99-compliant stdio so those format specifiers
# compile clean. (Cygwin/Linux libc handle %z natively; only the
# native msvcrt-linked build needs this.)
"$CC" -Wall -Wextra -Wpedantic -std=c11 "${HOST_OPT[@]}" \
    -D__USE_MINGW_ANSI_STDIO=1 "${WERROR_FLAG[@]}" "${HOST_STRIP[@]}" \
    "${PREEMPT_CFLAGS[@]}" \
    -I"$REPO_ROOT/include" \
    -I"$EXAMPLE_DIR" \
    -o "$BUILD_DIR/host.exe" \
    "$EXAMPLE_DIR/host.c" \
    "$SHELL_DATA_C" \
    "${HOST_SRCS[@]}" "${PREEMPT_SRCS[@]}" \
    -static -lws2_32 -lwinmm "${PREEMPT_LIBS[@]}"
# -static: bundle the mingw runtime so the .exe is a single self-
#   contained file for testers (no libgcc_s/libwinpthread DLL hunt).
#   The win32 audio path uses Win32 threads directly (not the pthread
#   shim), so nothing drags in libwinpthread.
# -lwinmm: the waveOut audio backend (live output).
step "built $BUILD_DIR/host.exe"

# Guest ELFs (platform-neutral). The RISC-V cross-compiler may be a
# native-Windows .exe under Cygwin, in which case it needs Windows
# paths — reuse the path-translation logic from vm_objs.sh by sourcing
# it just for guest_path()/GUEST_CC. (Host vars are overridden above.)
if [ "$NO_GUEST" = "1" ]; then
    step "skipping guest ELFs (--no-guest)"
else
    # shellcheck disable=SC1091
    . "$REPO_ROOT/examples/common/vm_objs.sh" >/dev/null 2>&1 || true
    if ! command -v "$GUEST_CC" >/dev/null 2>&1; then
        step "WARNING: RISC-V cross '$GUEST_CC' not found; skipping guests."
    else
        step "guest compiler: $GUEST_CC"
        GUEST_LD="$REPO_ROOT/examples/common/guest.ld"
        # Opt level (GUEST_OPT) and strip (GUEST_STRIP) come from the build
        # mode set above: release -> -Os + -Wl,-s; debug -> -Og -g, unstripped.
        GCFLAGS=(-march=rv32imc -mabi=ilp32 -nostdlib -nostartfiles -ffreestanding)
        GC=(-ffunction-sections -fdata-sections)
        GLD=(-Wl,--gc-sections -Wl,-z,max-page-size=4)

        # NOTE: shell.elf was already built (for size) and embedded
        # earlier, before the host compile. We don't rebuild it here —
        # doing so with the plain spawnable flags would overwrite the
        # size-optimized on-disk copy and make it inconsistent with the
        # embedded image. The spawnable demos below still build here.

        mkdir -p "$HOST_FILES"
        # Guest SDK (shared runtime + host hooks + mini-libc) lives at
        # examples/common/guest/; link its .c and add its include dirs.
        GUEST_SDK="$REPO_ROOT/examples/common/guest"
        LIB_SRCS=()
        for libsrc in "$GUEST_SDK"/*.c; do
            [ -f "$libsrc" ] || continue
            LIB_SRCS+=("$(guest_path "$libsrc")")
        done
        for src in "$EXAMPLE_DIR"/host_files_src/*.c; do
            [ -f "$src" ] || continue
            name=$(basename "$src" .c)
            step "compiling host_files/$name.elf (spawnable)..."
            "$GUEST_CC" "${GCFLAGS[@]}" "${GUEST_OPT[@]}" "${GC[@]}" \
                -I"$(guest_path "$EXAMPLE_DIR/host_files_src")" \
                -I"$(guest_path "$GUEST_SDK")" \
                -I"$(guest_path "$GUEST_SDK/include")" \
                -Wl,-T,"$(guest_path "$GUEST_LD")" "${GLD[@]}" "${GUEST_STRIP[@]}" \
                -o "$(guest_path "$HOST_FILES/$name.elf")" \
                "$(guest_path "$src")" "${LIB_SRCS[@]}"
        done
    fi
fi

echo ""
step "done. Native host: $BUILD_DIR/host.exe"
echo "  Run from PowerShell/cmd:"
echo "    host.exe                       # single stdio session"
echo "    host.exe --tcp=5000            # one TCP session"
echo "    host.exe --tcp=5000 --tcp=5001 # two sessions"
echo "  Connect with PuTTY (Raw or Telnet), or: nc localhost 5000"
