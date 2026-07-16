#!/bin/bash
# platform/rpi/build.sh — build + run microgarbage on bare-metal ARM in QEMU.
#
#   ./build.sh          build kernel.elf (RV32 VM + embedded RISC-V guest)
#   ./build.sh run      build, then boot it in QEMU raspi0 (Pi Zero / arm1176)
#   ./build.sh clean
#
# Requires: arm-none-eabi-gcc (+ newlib/rdimon), tools/rvcc's RISC-V cross,
# and (for `run`) qemu-system-arm. Output is via ARM semihosting.
set -e

HERE=$(cd "$(dirname "$0")" && pwd); cd "$HERE"
REPO=$(cd "$HERE/../.." && pwd)
BUILD="$HERE/build"
ACC="${ARM_CC:-arm-none-eabi-gcc}"
CPU="-mcpu=arm1176jzf-s"

case "${1:-build}" in
    clean) rm -rf "$BUILD"; echo "rpi: cleaned"; exit 0 ;;
esac
mkdir -p "$BUILD"

# 1. Build the RISC-V guest with rvcc (the dev-kit compiler).
echo "rpi: compiling guest with rvcc..."
bash "$REPO/tools/rvcc" -o "$BUILD/guest.elf" "$HERE/guest_hello.c"

# 2. Embed the guest ELF as a C array (no filesystem on bare metal yet).
#    Portable od-based bin2c (controls the symbol names).
echo "rpi: embedding guest ELF..."
{
    printf 'unsigned char g_guest_elf[] = {\n'
    od -An -v -tu1 "$BUILD/guest.elf" | tr -s ' ' | sed 's/^ //; s/ /,/g; s/$/,/'
    printf '};\nunsigned int g_guest_elf_len = %s;\n' "$(wc -c < "$BUILD/guest.elf")"
} > "$BUILD/guest_elf.h"

# 3. Compile the ARM image: the portable, cooperative VM subset + main +
#    the embedded guest. (No win32/pthread/tui/fs/audio sources.)
# Minimal portable subset: printf goes via SYS_FORMAT_AND_WRITE
# (vm_host_platform), so vm_host_stdio (raw SYS_WRITE, needs termios) is
# omitted — bare metal has no TTY.
VM=( vm_core vm_loader vm_ecall vm_ecall_handlers vm_mailbox
     vm_sched vm_sched_ops_coop vm_system vm_mem_protect
     vm_host_platform )
SRCS=( "$HERE/main.c" "$HERE/mmu.c" "$HERE/uart.c" )
for s in "${VM[@]}"; do SRCS+=( "$REPO/src/vm/$s.c" ); done
SRCS+=( "$REPO/src/memory/bump.c" "$REPO/src/memory/slab_stack.c" )
SRCS+=( "$REPO/src/containers/fifo_queue.c" "$REPO/src/containers/ring_buffer.c" )
SRCS+=( "$REPO/src/host/platform_stub.c" )   # host_platform_* time/sleep/stop

echo "rpi: compiling ARM image (arm1176, rdimon semihosting)..."
"$ACC" $CPU -marm -specs=rdimon.specs -O2 -std=c11 -ffreestanding \
    -Wall -Wno-unused-parameter \
    -I"$REPO/include" -I"$BUILD" \
    -Wl,-Ttext-segment=0x8000 \
    -o "$BUILD/kernel.elf" "${SRCS[@]}"

echo "rpi: built $BUILD/kernel.elf"

if [ "${1:-}" = "run" ]; then
    echo "rpi: ===== booting QEMU raspi0 ====="
    exec qemu-system-arm -M raspi0 -cpu arm1176 -nographic -semihosting \
        -kernel "$BUILD/kernel.elf"
fi
