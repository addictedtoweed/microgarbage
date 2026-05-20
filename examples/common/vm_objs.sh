# Common build settings for VM examples.
#
# Sourced by each example's build.sh. Requires bash; uses arrays
# to support paths with spaces.
#
# Sets these variables (arrays where it matters for spaces):
#
#   REPO_ROOT        Absolute path to repo root (auto-detected
#                    relative to this file's location).
#   CC               C compiler (override via env to use a
#                    different one).
#   CFLAGS           Array of flags (warnings + C11 + -Iinclude).
#                    Use with "${CFLAGS[@]}" — never $CFLAGS.
#   VM_CORE_SRCS     Array of VM library .c files that any example
#                    host must link. Always includes the full VM
#                    core; we don't try to slice it finer because
#                    the linker drops unused objects.
#                    Use with "${VM_CORE_SRCS[@]}".
#   GUEST_LD         Path to the shared guest linker script.
#   GUEST_CC         RISC-V cross-compiler for guest ELFs.
#   GUEST_CFLAGS     Array of guest compilation flags (rv32imc,
#                    no libc, freestanding).
#                    Use with "${GUEST_CFLAGS[@]}".
#
# All paths are absolute so example scripts can be run from
# anywhere. Arrays preserve the spaces in REPO_ROOT correctly;
# the old space-separated-string approach silently broke when
# any path contained whitespace.

# Refuse to run under a non-bash shell. Arrays and BASH_SOURCE
# are bash-isms; sourcing from dash/ash/sh will silently misbehave.
if [ -z "${BASH_VERSION:-}" ]; then
    echo "vm_objs.sh: this file requires bash (was sourced from $0)" >&2
    return 1 2>/dev/null || exit 1
fi

# ---------------------------------------------------------------
# Locate the repo root.
# ---------------------------------------------------------------
# This file lives at examples/common/vm_objs.sh. The repo root is
# two directories up from this file's location.

_VM_OBJS_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd "$_VM_OBJS_DIR/../.." && pwd)
unset _VM_OBJS_DIR

# ---------------------------------------------------------------
# Host compilation.
# ---------------------------------------------------------------

CC=${CC:-cc}

# CFLAGS as an array. Each token is a separate element so spaces
# inside REPO_ROOT don't get word-split when expanded. If the
# caller already set CFLAGS as a string, respect it (note: that
# string still won't survive spaces in $REPO_ROOT — but the
# caller has chosen to override our handling).
if [ -z "${CFLAGS+x}" ]; then
    CFLAGS=(
        -Wall -Wextra -Wpedantic -std=c11 -O2
        -I"${REPO_ROOT}/include"
    )
else
    # Caller-supplied string; convert to a single-element array
    # so "${CFLAGS[@]}" still works in build.sh. This means
    # CFLAGS="..." overrides DO get word-split on spaces in
    # the user's value, which is the historical behavior.
    # shellcheck disable=SC2206
    CFLAGS=($CFLAGS)
fi

# Every VM-library .c file the host needs. Array form survives
# spaces in $REPO_ROOT.
VM_CORE_SRCS=(
    "${REPO_ROOT}/src/vm/vm_core.c"
    "${REPO_ROOT}/src/vm/vm_loader.c"
    "${REPO_ROOT}/src/vm/vm_ecall.c"
    "${REPO_ROOT}/src/vm/vm_ecall_handlers.c"
    "${REPO_ROOT}/src/vm/vm_mailbox.c"
    "${REPO_ROOT}/src/vm/vm_sched.c"
    "${REPO_ROOT}/src/vm/vm_system.c"
    "${REPO_ROOT}/src/vm/vm_host_stdio.c"
    "${REPO_ROOT}/src/memory/bump.c"
    "${REPO_ROOT}/src/memory/slab_stack.c"
    "${REPO_ROOT}/src/containers/fifo_queue.c"
    "${REPO_ROOT}/src/containers/ring_buffer.c"
)

# ---------------------------------------------------------------
# Guest compilation.
# ---------------------------------------------------------------

GUEST_LD="${REPO_ROOT}/examples/common/guest.ld"

# Pick the first available RISC-V cross-compiler. Common names:
#   riscv64-unknown-elf-gcc   — upstream riscv-collab/riscv-gnu-toolchain
#                               and most Linux distro packages
#   riscv-none-elf-gcc        — xPack (xpack-dev-tools/riscv-none-elf-gcc-xpack;
#                               recommended for Windows users)
#   riscv32-unknown-elf-gcc   — some custom 32-bit-only builds
#   riscv64-elf-gcc           — Homebrew's riscv-gnu-toolchain formula
#
# Override by setting GUEST_CC in the environment before sourcing.
# The -march=rv32imc and -mabi=ilp32 flags below ensure the right
# multilib is selected regardless of which prefix is in use.
if [ -z "${GUEST_CC:-}" ]; then
    for _candidate in riscv64-unknown-elf-gcc \
                      riscv-none-elf-gcc \
                      riscv32-unknown-elf-gcc \
                      riscv64-elf-gcc; do
        if command -v "$_candidate" >/dev/null 2>&1; then
            GUEST_CC=$_candidate
            break
        fi
    done
    GUEST_CC=${GUEST_CC:-riscv64-unknown-elf-gcc}   # fallback for the error path
    unset _candidate
fi

# GUEST_CFLAGS as an array (no paths inside currently, but
# consistent style and supports future additions).
if [ -z "${GUEST_CFLAGS+x}" ]; then
    GUEST_CFLAGS=(
        -march=rv32imc -mabi=ilp32
        -nostdlib -nostartfiles
        -ffreestanding -O2
    )
else
    # shellcheck disable=SC2206
    GUEST_CFLAGS=($GUEST_CFLAGS)
fi

# Whether the cross-compiler exists. Examples that need to
# rebuild the .elf will check this and skip rebuild if missing
# (the .elf may be pre-built and checked in).
have_guest_cc() {
    command -v "$GUEST_CC" >/dev/null 2>&1
}
