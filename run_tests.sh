#!/bin/bash
# run_tests.sh — build and run every unit-test suite.
#
# Tests live next to the code they cover, under src/<module>/tests/.
# This script knows each suite's link dependencies and builds + runs
# them with the host cc.
#
#   ./run_tests.sh            build + run everything
#   ./run_tests.sh audio      only suites whose name matches "audio"
#   CC=gcc ./run_tests.sh     override the compiler (default: cc)
#
# Binaries + logs land in build/tests/ (gitignored). Exit status is
# non-zero if any suite fails to build or fails at runtime.
#
# Toolchain note: the canonical test compiler is Cygwin/POSIX `cc`
# (links cygwin1.dll, exercises the POSIX host paths). A native
# mingw-w64 cc also works for the platform-neutral majority; a few
# suites are POSIX-only (pipe/fsync, /tmp) and are SKIPPED with a
# reason under a native-Windows toolchain, while the native build adds
# the Windows-only shim sources (vm_host_stdio_win32.c, waveout).
# FatFs and real-ELF suites skip themselves when prerequisites are absent.

set -u
CC="${CC:-cc}"
CFLAGS="-Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude"
OUT="build/tests"
FILTER="${1:-}"
FATFS_DIR="third_party/fatfs"
FATFS_SRC="$FATFS_DIR/source"
mkdir -p "$OUT"

MACHINE=$($CC -dumpmachine 2>/dev/null || echo unknown)
case "$MACHINE" in *mingw*) NATIVE_WIN=1;; *) NATIVE_WIN=0;; esac

pass=0; fail=0; bfail=0; skipped=0
declare -a FAILED=()

# run <name> <module> <extra cc args: -D..., extra .c sources, -l libs>
run() {
    local name="$1" mod="$2"; shift 2
    if [ -n "$FILTER" ] && [[ "$name" != *"$FILTER"* ]]; then return; fi
    local testc="src/$mod/tests/test_$name.c"
    if [ ! -f "$testc" ]; then echo "MISSING   $name ($testc)"; FAILED+=("$name"); fail=$((fail+1)); return; fi
    if ! $CC $CFLAGS -o "$OUT/$name" "$testc" "$@" >"$OUT/$name.build.log" 2>&1; then
        echo "BUILD-FAIL  $name   (see $OUT/$name.build.log)"; FAILED+=("$name"); bfail=$((bfail+1)); return
    fi
    if "$OUT/$name" >"$OUT/$name.run.log" 2>&1; then
        echo "PASS  $name"; pass=$((pass+1))
    else
        echo "FAIL  $name   (see $OUT/$name.run.log)"; FAILED+=("$name"); fail=$((fail+1))
    fi
}
skip() {
    if [ -n "$FILTER" ] && [[ "$1" != *"$FILTER"* ]]; then return; fi
    echo "SKIP  $1   ($2)"; skipped=$((skipped+1));
}

# ---- containers ----
run hashtable        containers  src/containers/hashtable.c
run ring_buffer      containers  src/containers/ring_buffer.c
run fifo_queue       containers  src/containers/fifo_queue.c src/containers/ring_buffer.c
run stack            containers  src/containers/stack.c
run slist            containers  src/containers/slist.c
run dlist            containers  src/containers/dlist.c
run tree             containers  src/containers/tree.c
run spsc_ring        containers  src/containers/spsc_ring.c
run spsc_ring_stress containers  src/containers/spsc_ring.c -lpthread

# ---- math ----
run fixed_point      math  src/math/fixed_point.c
run fast_div         math  src/math/fast_div.c

# ---- memory ----
run bump             memory  src/memory/bump.c
run slab_stack       memory  src/memory/slab_stack.c
run bump_on_slab     memory  src/memory/bump.c src/memory/slab_stack.c

# ---- util ----
if [ "$NATIVE_WIN" = 1 ]; then
    skip inicfg "hardcoded /tmp path — Cygwin/POSIX cc only"
else
    run inicfg       util  src/util/inicfg.c
fi

# ---- storage ----
run trashdrive       storage  src/storage/trashdrive.c
run trashfs          storage  src/storage/trashfs.c src/storage/trashdrive.c
run trashfs_p2       storage  src/storage/trashfs.c src/storage/trashdrive.c
run trashfs_p3       storage  src/storage/trashfs.c src/storage/trashdrive.c
if [ -f "$FATFS_SRC/ff.c" ]; then
    run trashdrive_fatfs storage -DHAVE_FATFS -I"$FATFS_DIR" -I"$FATFS_SRC" \
        src/storage/trashdrive_fatfs.c src/storage/trashdrive.c \
        "$FATFS_DIR/ff_wrapped.c" "$FATFS_SRC/ffsystem.c"
else
    skip trashdrive_fatfs "FatFs not present at $FATFS_SRC/ff.c"
fi

# ---- audio ----
RB=src/containers/ring_buffer.c
run audio_mixer      audio  src/audio/audio_mixer.c $RB
run music_player     audio  src/audio/music_player.c src/audio/audio_mixer.c $RB
run audio_pool       audio  -DAUDIO_POOL_BLOCK_SIZE=64 src/audio/audio_pool.c
run audio_arbiter    audio  -DAUDIO_POOL_BLOCK_SIZE=64 -DAUDIO_ARBITER_MAX_TRACKS=4 src/audio/audio_arbiter.c src/audio/audio_pool.c
run audio_pool_stream audio -DAUDIO_POOL_BLOCK_SIZE=64 src/audio/audio_pool_stream.c src/audio/audio_pool.c
run audio_pool_stream_integration audio src/audio/audio_pool_stream.c src/audio/audio_pool.c src/audio/music_player.c src/audio/audio_mixer.c $RB
run audio_fft        audio  src/audio/audio_fft.c src/audio/audio_fft_kernel.c
run audio_wav_read   audio  src/audio/audio_wav_read.c
run audio_file_stream audio src/audio/audio_file_stream.c src/audio/audio_wav_read.c
# audio_sink_wav writes to a hardcoded /tmp path (absent on native Win).
# The sink dispatcher also references the waveOut backend whenever
# _WIN32 || __CYGWIN__, so Cygwin must link it (+winmm); Linux need not.
if [ "$NATIVE_WIN" = 1 ]; then
    skip audio_sink_wav "/tmp output path — Cygwin/POSIX cc only"
else
    case "$MACHINE" in
        *cygwin*) run audio_sink_wav audio src/audio/audio_sink_wav.c src/audio/audio_sink_waveout.c -lwinmm ;;
        *)        run audio_sink_wav audio src/audio/audio_sink_wav.c ;;
    esac
fi
run audio_service    audio  src/audio/audio_service.c src/audio/audio_arbiter.c src/audio/audio_pool.c \
    src/audio/audio_pool_stream.c src/audio/audio_mixer.c src/audio/music_player.c src/audio/audio_fft.c \
    src/audio/audio_fft_kernel.c src/audio/audio_wav_read.c src/audio/audio_file_stream.c \
    src/vm/service_channel.c src/vm/channel_thread.c $RB src/containers/spsc_ring.c -lpthread

# ---- vm ----
# Base sources most VM suites link against (cooperative build). The
# native-Windows stdio shim is added only under a mingw toolchain;
# Cygwin/POSIX uses the POSIX paths inside vm_host_stdio.c directly.
VM_CORE="src/vm/vm_core.c src/vm/vm_loader.c src/vm/vm_ecall.c src/vm/vm_ecall_handlers.c \
         src/vm/vm_mailbox.c src/vm/vm_sched.c src/vm/vm_sched_ops_coop.c src/vm/vm_sched_ops_pre.c \
         src/vm/vm_system.c src/vm/vm_host_stdio.c src/vm/vm_host_platform.c src/vm/vm_host_tui.c \
         src/memory/bump.c src/memory/slab_stack.c \
         src/containers/fifo_queue.c src/containers/ring_buffer.c"
if [ "$NATIVE_WIN" = 1 ]; then VM_CORE="$VM_CORE src/vm/vm_host_stdio_win32.c"; fi

for s in vm_core vm_core_alu vm_core_c vm_core_m vm_core_memctl vm_core_system \
         vm_ecall vm_ecall_handlers vm_loader vm_mailbox vm_sched vm_system; do
    run "$s" vm $VM_CORE
done

run service_channel  vm  src/vm/service_channel.c src/vm/channel_thread.c src/containers/spsc_ring.c -lpthread
run presched         vm  -DGARBAGE_SCHED_MODE=1 src/vm/presched.c -lpthread
run presched_spawn   vm  -DGARBAGE_SCHED_MODE=1 $VM_CORE src/vm/presched.c -lpthread

# Host-shim suites that use POSIX pipe()/fsync() in the TEST itself —
# build under Cygwin/POSIX, not native mingw.
if [ "$NATIVE_WIN" = 1 ]; then
    skip vm_host_stdio    "POSIX pipe() — Cygwin/POSIX cc only"
    skip vm_host_platform "POSIX fsync() — Cygwin/POSIX cc only"
    skip vm_host_tui      "POSIX fsync() — Cygwin/POSIX cc only"
else
    run vm_host_platform vm $VM_CORE
    run vm_host_tui      vm $VM_CORE
    # vm_host_stdio drives real guest ELFs (counter + keydump).
    if [ -f examples/02_counter/build/counter.elf ] && [ -f examples/04_keydump/build/keydump.elf ]; then
        run vm_host_stdio vm $VM_CORE
    else
        skip vm_host_stdio "needs examples/02_counter + 04_keydump ELFs (run their build.sh)"
    fi
fi

# vm_host_fs pulls in FatFs (ff.h) + the trashfs/trashdrive backends.
# The test uses POSIX mkdir(2)/paths, so it builds under Cygwin/POSIX.
if [ "$NATIVE_WIN" = 1 ]; then
    skip vm_host_fs "POSIX mkdir(2)/paths — Cygwin/POSIX cc only"
elif [ -f "$FATFS_SRC/ff.c" ]; then
    run vm_host_fs   vm  -DHAVE_FATFS -I"$FATFS_DIR" -I"$FATFS_SRC" $VM_CORE \
        src/vm/vm_host_fs.c src/storage/trashfs.c src/storage/trashdrive.c \
        src/storage/trashdrive_fatfs.c "$FATFS_DIR/ff_wrapped.c" "$FATFS_SRC/ffsystem.c"
else
    skip vm_host_fs "needs FatFs at $FATFS_SRC/ff.c"
fi

# vm_real_elf needs prebuilt guest ELFs from examples/*/build/.
if [ -f examples/01_hello/build/hello.elf ]; then
    run vm_real_elf  vm  $VM_CORE
else
    skip vm_real_elf "needs examples/01_hello/build/hello.elf (run its build.sh first)"
fi

echo "-----------------------------------------------"
echo "PASS=$pass  FAIL=$fail  BUILD-FAIL=$bfail  SKIP=$skipped   (cc=$MACHINE)"
if [ $((fail + bfail)) -gt 0 ]; then printf 'failing: %s\n' "${FAILED[*]:-}"; exit 1; fi
exit 0
