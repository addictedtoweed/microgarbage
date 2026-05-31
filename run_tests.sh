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
# Real-ELF suites skip themselves when prerequisite guest ELFs are absent.

set -u
CC="${CC:-cc}"
CFLAGS="-Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude"
OUT="build/tests"
FILTER="${1:-}"
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
run tree             containers  src/containers/tree.c src/containers/avl_core.c
run avlhash          containers  src/containers/avlhash.c src/containers/avl_core.c
run bitset           containers  src/containers/bitset.c
run spsc_ring        containers  src/containers/spsc_ring.c
run spsc_ring_stress containers  src/containers/spsc_ring.c -lpthread

# ---- math ----
run bits             math
run fixed_point      math  src/math/fixed_point.c
run fast_div         math  src/math/fast_div.c
run vec3_q16         math                       # header-only (fixed_point.h + vec3_q16.h)
run trig_q16         math  src/math/trig_q16.c
run mat_q16          math  src/math/trig_q16.c   # mat rotation ctors call CORDIC trig

# ---- memory ----
run bump             memory  src/memory/bump.c
run slab_stack       memory  src/memory/slab_stack.c
run bump_on_slab     memory  src/memory/bump.c src/memory/slab_stack.c

# ---- util ----
run inicfg           util  src/util/inicfg.c

# ---- storage ----
run trashdrive       storage  src/storage/trashdrive.c
run trashfs          storage  src/storage/trashfs.c src/storage/trashdrive.c
run trashfs_p2       storage  src/storage/trashfs.c src/storage/trashdrive.c
run trashfs_p3       storage  src/storage/trashfs.c src/storage/trashdrive.c
run trashfs_dirs     storage  src/storage/trashfs.c src/storage/trashdrive.c

# ---- video ----
# ppu is headless (renders to a memory framebuffer); the present shim
# (present_gl_win32.c) and the r3d polygon renderer are GUI/visual layers
# with no unit test — exercised by demo_canyon and demo_canyon4.
run ppu              video  src/video/ppu.c

# ---- audio ----
RB=src/containers/ring_buffer.c
BITSET=src/containers/bitset.c          # audio_pool's block free-map
run audio_mixer      audio  src/audio/audio_mixer.c $RB
run music_player     audio  src/audio/music_player.c src/audio/audio_mixer.c $RB
run audio_pool       audio  -DAUDIO_POOL_BLOCK_SIZE=64 src/audio/audio_pool.c $BITSET
run audio_arbiter    audio  -DAUDIO_POOL_BLOCK_SIZE=64 -DAUDIO_ARBITER_MAX_TRACKS=4 src/audio/audio_arbiter.c src/audio/audio_pool.c $BITSET
run audio_pool_stream audio -DAUDIO_POOL_BLOCK_SIZE=64 src/audio/audio_pool_stream.c src/audio/audio_pool.c $BITSET
run audio_pool_stream_integration audio src/audio/audio_pool_stream.c src/audio/audio_pool.c src/audio/music_player.c src/audio/audio_mixer.c $RB $BITSET
run audio_fft        audio  src/audio/audio_fft.c src/audio/audio_fft_kernel.c
run audio_wav_read   audio  src/audio/audio_wav_read.c
run audio_file_stream audio src/audio/audio_file_stream.c src/audio/audio_wav_read.c
# The sink dispatcher references the waveOut backend whenever
# _WIN32 || __CYGWIN__ (native Windows + Cygwin), so those link it
# (+winmm); a pure POSIX/Linux build does not.
case "$MACHINE" in
    *mingw*|*cygwin*) run audio_sink_wav audio src/audio/audio_sink_wav.c src/audio/audio_sink_waveout.c -lwinmm ;;
    *)               run audio_sink_wav audio src/audio/audio_sink_wav.c ;;
esac
run audio_service    audio  src/audio/audio_service.c src/audio/audio_arbiter.c src/audio/audio_pool.c \
    src/audio/audio_pool_stream.c src/audio/audio_mixer.c src/audio/music_player.c src/audio/audio_fft.c \
    src/audio/audio_fft_kernel.c src/audio/audio_wav_read.c src/audio/audio_file_stream.c \
    src/vm/service_channel.c src/vm/channel_thread.c $RB $BITSET src/containers/spsc_ring.c -lpthread

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
run vm_host_platform vm $VM_CORE
run vm_host_tui      vm $VM_CORE
# vm_host_stdio drives real guest ELFs.
if [ -f examples/02_counter/build/guest.elf ] && [ -f examples/04_keydump/build/guest.elf ]; then
    run vm_host_stdio vm $VM_CORE
else
    skip vm_host_stdio "needs examples/02_counter + 04_keydump guest.elf (run their build.sh)"
fi

# vm_host_fs: trashfs RAM disk + host-passthrough backends (no FatFs).
# vm_host_fs_spawn.c holds SYS_SPAWN_AND_WAIT (split for the size cap).
run vm_host_fs   vm  $VM_CORE src/vm/vm_host_fs.c src/vm/vm_host_fs_spawn.c src/storage/trashfs.c

# vm_real_elf needs prebuilt guest ELFs from examples/01_hello/build/.
if [ -f examples/01_hello/build/guest_minimal.elf ]; then
    run vm_real_elf  vm  $VM_CORE
else
    skip vm_real_elf "needs examples/01_hello/build/guest_minimal.elf (run its build.sh first)"
fi

echo "-----------------------------------------------"
echo "PASS=$pass  FAIL=$fail  BUILD-FAIL=$bfail  SKIP=$skipped   (cc=$MACHINE)"
if [ $((fail + bfail)) -gt 0 ]; then printf 'failing: %s\n' "${FAILED[*]:-}"; exit 1; fi
exit 0
