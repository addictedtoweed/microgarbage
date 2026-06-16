#!/usr/bin/env bash
# ============================================================
#  build-mgapi.sh — bash port of build-mgapi.ps1
#
#  Builds mgapi.dll (the cart runtime) and mgapi_host_test.exe
#  from MSYS2 MinGW64. Mirrors the PowerShell script's logic
#  one-to-one; either script produces the same artifacts in
#  build/mgapi/.
#
#  Usage:
#    ./build-mgapi.sh                 # full build
#    ./build-mgapi.sh --clean         # wipe build/mgapi and exit
#    ./build-mgapi.sh --no-guest      # skip RV32 guest ELF builds (stub them)
#    CC=gcc ./build-mgapi.sh          # override host CC (default: gcc)
#    GUESTCC=riscv-none-elf-gcc ./build-mgapi.sh   # override guest CC
#
#  Run from the MSYS2 MinGW64 shell. Requires:
#    mingw-w64 gcc on PATH (the "MINGW64" MSYS2 environment)
#    optional: a riscv32 cross gcc for guest ELFs
#
#  Public domain (CC0). No warranty.
# ============================================================
set -e

# --- Arg parsing --------------------------------------------------
do_clean=0
no_guest=0
for arg in "$@"; do
    case "$arg" in
        --clean)     do_clean=1 ;;
        --no-guest)  no_guest=1 ;;
        -h|--help)
            sed -n '2,/^# =/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "unknown arg: $arg (try --help)" >&2
            exit 1
            ;;
    esac
done

# --- Paths --------------------------------------------------------
script_dir="$(cd "$(dirname "$0")" && pwd)"
RepoRoot="$script_dir"
BuildDir="$RepoRoot/build/mgapi"
IncludeDir="$RepoRoot/include"
MgapiSrcDir="$RepoRoot/src/mgapi"
ToolsDir="$RepoRoot/tools"
genDir="$BuildDir/gen"

step()  { echo "build-mgapi: $*"; }
die()   { echo "build-mgapi: ERROR - $*" >&2; exit 1; }

# --- TMP defense --------------------------------------------------
# If TMP/TEMP/TMPDIR contains a space (the canonical "IP Freely" home
# path lands here), gcc's temp-file creation will fail with "Cannot
# create temporary file ... Permission denied". Redirect to a
# space-free path. Mirrors build-mgapi.ps1's same defense.
if [ -n "$TMP" ] && echo "$TMP" | grep -q ' '; then
    mgtmp="/c/mg_tmp"
    mkdir -p "$mgtmp"
    export TMP="$mgtmp" TEMP="$mgtmp" TMPDIR="$mgtmp"
    step "redirected TMP -> $mgtmp (original had whitespace)"
fi

if [ "$do_clean" = 1 ]; then
    rm -rf "$BuildDir"
    step "cleaned"
    exit 0
fi

# --- Host CC detect -----------------------------------------------
CC="${CC:-}"
if [ -z "$CC" ]; then
    if command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
        CC=x86_64-w64-mingw32-gcc
    elif command -v gcc >/dev/null 2>&1; then
        triple="$(gcc -dumpmachine 2>/dev/null || true)"
        case "$triple" in
            *mingw*|*w64*windows*) CC=gcc ;;
            *) die "gcc is not a mingw target ($triple); set CC=x86_64-w64-mingw32-gcc" ;;
        esac
    else
        die "no host gcc found (need MSYS2 MinGW64)"
    fi
fi
step "host CC = $CC"

# --- Guest CC detect ----------------------------------------------
GUESTCC="${GUESTCC:-}"
if [ -z "$GUESTCC" ] && [ "$no_guest" = 0 ]; then
    for cand in riscv-none-elf-gcc riscv64-unknown-elf-gcc \
                riscv32-unknown-elf-gcc riscv64-elf-gcc; do
        if command -v "$cand" >/dev/null 2>&1; then
            GUESTCC="$cand"; break
        fi
    done
fi
if [ "$no_guest" = 0 ] && [ -n "$GUESTCC" ]; then
    step "guest CC = $GUESTCC"
elif [ "$no_guest" = 0 ]; then
    step "no riscv guest CC found; will emit zero-length stubs"
fi

mkdir -p "$BuildDir" "$genDir"

# --- Source list (mirrors build-mgapi.ps1 $mgapiSrcs) -------------
mgapiSrcs=(
    "$MgapiSrcDir/mgapi_init.c"
    "$MgapiSrcDir/cart_window.c"
    "$MgapiSrcDir/psram_pool.c"
    "$MgapiSrcDir/audio_init.c"
    "$MgapiSrcDir/audio_device_win32.c"
    "$MgapiSrcDir/cart_volume.c"
    "$MgapiSrcDir/l2_alloc.c"
    "$MgapiSrcDir/l2_init.c"
    "$RepoRoot/src/storage/trashfs.c"
    "$RepoRoot/src/vm/vm_core.c"
    "$RepoRoot/src/vm/vm_loader.c"
    "$RepoRoot/src/vm/vm_ecall.c"
    "$RepoRoot/src/vm/vm_ecall_handlers.c"
    "$RepoRoot/src/vm/vm_mailbox.c"
    "$RepoRoot/src/vm/vm_sched.c"
    "$RepoRoot/src/vm/vm_sched_ops_coop.c"
    "$RepoRoot/src/vm/vm_sched_ops_pre.c"
    "$RepoRoot/src/vm/vm_system.c"
    "$RepoRoot/src/vm/vm_host_stdio.c"
    "$RepoRoot/src/vm/vm_host_stdio_win32.c"
    "$RepoRoot/src/vm/vm_host_platform.c"
    "$RepoRoot/src/vm/vm_host_fs.c"
    "$RepoRoot/src/vm/vm_host_fs_spawn.c"
    "$RepoRoot/src/vm/vm_host_audio.c"
    "$RepoRoot/src/memory/bump.c"
    "$RepoRoot/src/memory/slab_stack.c"
    "$RepoRoot/src/containers/fifo_queue.c"
    "$RepoRoot/src/host/platform_win.c"
    "$MgapiSrcDir/vm_init.c"
    "$MgapiSrcDir/copro_ecalls.c"
    "$MgapiSrcDir/l2_ecalls.c"
    "$MgapiSrcDir/copro_mg_state.c"
    "$MgapiSrcDir/copro_mg_handlers.c"
    "$MgapiSrcDir/tcp_listen.c"
    "$MgapiSrcDir/worker.c"
)

audioSrcs=(
    "$RepoRoot/src/vm/channel_win32.c"
    "$RepoRoot/src/vm/service_channel.c"
    "$RepoRoot/src/containers/spsc_ring.c"
    "$RepoRoot/src/containers/bitset.c"
    "$RepoRoot/src/containers/ring_buffer.c"
    "$RepoRoot/src/io/stream_arbiter.c"
    "$RepoRoot/src/io/stream_ecalls.c"
    "$RepoRoot/src/audio/audio_service.c"
    "$RepoRoot/src/audio/audio_arbiter.c"
    "$RepoRoot/src/audio/audio_pool.c"
    "$RepoRoot/src/audio/audio_pool_stream.c"
    "$RepoRoot/src/audio/audio_mixer.c"
    "$RepoRoot/src/audio/music_player.c"
    "$RepoRoot/src/audio/audio_fft.c"
    "$RepoRoot/src/audio/audio_fft_kernel.c"
    "$RepoRoot/src/audio/audio_wav_read.c"
    "$RepoRoot/src/audio/audio_file_stream.c"
    "$RepoRoot/src/audio/audio_sink_wav.c"
    "$RepoRoot/src/audio/audio_sink_waveout.c"
    "$RepoRoot/src/audio/audio_sink_wasapi.c"
)

# --- Stub generator (for missing-guestCC fallback) ----------------
# Emit a C file with zero-length array definitions for every symbol
# the DLL link expects.
write_full_stub() {
    local dst="$1"
    {
        echo "#include <stddef.h>"
        for sym in shell_elf l2_test_elf menu_elf \
                   demo_palette_elf demo_letterbox_elf demo_dynamic_letterbox_elf \
                   demo_sprite_elf demo_mode7_elf demo_mode7_3d_elf \
                   demo_audio_mixer_elf demo_pcm_stream_elf demo_fmv_elf \
                   demo_fmv_flip_elf demo_fmv_still_elf demo_nmi_smoke_elf; do
            echo "const unsigned char ${sym}[] = {0};"
            echo "const size_t ${sym}_len = 0;"
        done
    } > "$dst"
}

# --- Build guest ELFs + bake (or fall through to stub) ------------
shellDataC="$genDir/shell_elf_data.c"
baked=0

if [ "$no_guest" = 0 ] && [ -n "$GUESTCC" ]; then
    step "compiling guest shell.elf (RV32IMC)..."
    exampleDir="$RepoRoot/examples/05_shell"
    guestLd="$RepoRoot/examples/common/guest.ld"
    shellC="$exampleDir/shell.c"
    shellElf="$BuildDir/shell.elf"
    gcf=(-march=rv32imc -mabi=ilp32 -nostdlib -nostartfiles
         -ffreestanding -Os -ffunction-sections -fdata-sections)
    gldf=(-Wl,--gc-sections -Wl,-z,max-page-size=4 -Wl,-s)
    "$GUESTCC" "${gcf[@]}" "${gldf[@]}" "-Wl,-T,$guestLd" \
        -o "$shellElf" "$shellC" \
        || die "guest shell.elf compile failed"

    bin2c="$BuildDir/bin2c.exe"
    step "compiling bin2c..."
    "$CC" -O2 -o "$bin2c" "$exampleDir/tools/bin2c.c" \
        || die "bin2c compile failed"

    step "baking shell.elf -> $shellDataC"
    "$bin2c" "$shellElf" "shell_elf" "$shellDataC" \
        || die "bin2c run failed"
    baked=1

    guestCommon="$RepoRoot/examples/common/guest"
    guestInclude="$RepoRoot/include"
    mgGuestImpls=(
        "$guestCommon/mg_bg.c"     "$guestCommon/mg_frame.c"
        "$guestCommon/mg_gfx.c"    "$guestCommon/mg_hdma.c"
        "$guestCommon/mg_input.c"  "$guestCommon/mg_mode7.c"
        "$guestCommon/mg_panic.c"  "$guestCommon/mg_sprite.c"
        "$guestCommon/mg_audio.c"  "$guestCommon/mg_actor.c"
        "$guestCommon/mg_stream.c" "$guestCommon/mg_nmi.c"
        "$RepoRoot/src/math/trig_q16.c"
    )

    # Each entry: SRC|SYM|OUT|GEN|EXTRA
    #   EXTRA: "mg" -> include mgGuestImpls; "" -> none
    guests=(
        "tools/guests/l2_test.c|l2_test_elf|l2_test.elf|l2_test_elf_data.c|"
        "tools/guests/menu.c|menu_elf|menu.elf|menu_elf_data.c|"
        "tools/guests/demos/demo_palette.c|demo_palette_elf|demo_palette.elf|demo_palette_elf_data.c|mg"
        "tools/guests/demos/demo_letterbox.c|demo_letterbox_elf|demo_letterbox.elf|demo_letterbox_elf_data.c|mg"
        "tools/guests/demos/demo_dynamic_letterbox.c|demo_dynamic_letterbox_elf|demo_dynamic_letterbox.elf|demo_dynamic_letterbox_elf_data.c|mg"
        "tools/guests/demos/demo_sprite.c|demo_sprite_elf|demo_sprite.elf|demo_sprite_elf_data.c|mg"
        "tools/guests/demos/demo_mode7.c|demo_mode7_elf|demo_mode7.elf|demo_mode7_elf_data.c|mg"
        "tools/guests/demos/demo_mode7_3d.c|demo_mode7_3d_elf|demo_mode7_3d.elf|demo_mode7_3d_elf_data.c|mg"
        "tools/guests/demos/demo_audio_mixer.c|demo_audio_mixer_elf|demo_audio_mixer.elf|demo_audio_mixer_elf_data.c|mg"
        "tools/guests/demos/demo_pcm_stream.c|demo_pcm_stream_elf|demo_pcm_stream.elf|demo_pcm_stream_elf_data.c|mg"
        "tools/guests/demos/demo_fmv.c|demo_fmv_elf|demo_fmv.elf|demo_fmv_elf_data.c|mg"
        "tools/guests/demos/demo_fmv_flip.c|demo_fmv_flip_elf|demo_fmv_flip.elf|demo_fmv_flip_elf_data.c|mg"
        "tools/guests/demos/demo_fmv_still.c|demo_fmv_still_elf|demo_fmv_still.elf|demo_fmv_still_elf_data.c|mg"
        "tools/guests/demos/demo_nmi_smoke.c|demo_nmi_smoke_elf|demo_nmi_smoke.elf|demo_nmi_smoke_elf_data.c|mg"
    )

    for entry in "${guests[@]}"; do
        IFS='|' read -r srcRel sym out gen extra <<< "$entry"
        srcPath="$RepoRoot/$srcRel"
        if [ ! -f "$srcPath" ]; then
            step "skipping $out (no $srcRel)"
            continue
        fi
        step "compiling $out (RV32IMC)..."
        outElf="$BuildDir/$out"
        sources=("$srcPath")
        if [ "$extra" = "mg" ]; then
            sources+=("${mgGuestImpls[@]}")
        fi
        "$GUESTCC" "${gcf[@]}" "${gldf[@]}" \
            "-I$guestCommon" "-I$guestInclude" \
            "-Wl,-T,$guestLd" -o "$outElf" "${sources[@]}" \
            || die "$out compile failed"
        genPath="$genDir/$gen"
        "$bin2c" "$outElf" "$sym" "$genPath" \
            || die "$sym bin2c failed"
        mgapiSrcs+=("$genPath")
    done
fi

if [ "$baked" = 0 ]; then
    step "writing zero-length guest stub -> $shellDataC"
    write_full_stub "$shellDataC"
fi
mgapiSrcs+=("$shellDataC")

# --- Bake SNES ROM images -----------------------------------------
# Same Bake-Sfc logic as the PS1: if pre-built .sfc / .bin exists,
# bin2c it; otherwise emit a zero-length stub so the link still
# succeeds.
bake_sfc() {
    local srcPath="$1" sym="$2" dstC="$3"
    if [ -f "$srcPath" ] && [ -x "$BuildDir/bin2c.exe" ]; then
        step "baking $(basename "$srcPath") -> $sym[]"
        "$BuildDir/bin2c.exe" "$srcPath" "$sym" "$dstC" \
            || die "$sym bin2c failed"
    else
        step "$sym stub (no $(basename "$srcPath"))"
        {
            echo "#include <stddef.h>"
            echo "const unsigned char ${sym}[] = {0};"
            echo "const size_t ${sym}_len = 0;"
        } > "$dstC"
    fi
}

smokeDataC="$genDir/smoke_rom_data.c"
bootDataC="$genDir/boot_rom_data.c"
bake_sfc "$RepoRoot/snes/build/snes_smoke.sfc" "smoke_rom" "$smokeDataC"
bake_sfc "$RepoRoot/snes/build/snes_boot.bin"  "boot_rom"  "$bootDataC"
mgapiSrcs+=("$smokeDataC" "$bootDataC")

# --- Append audio sources -----------------------------------------
mgapiSrcs+=("${audioSrcs[@]}")

# --- Compile + link mgapi.dll -------------------------------------
dll="$BuildDir/mgapi.dll"

cflags=(
    -Wall -Wextra -Wpedantic -Werror -std=c11
    -D__USE_MINGW_ANSI_STDIO=1
    -DMGAPI_BUILDING_DLL=1
    -I"$IncludeDir"
    -I"$MgapiSrcDir"
)

step "compiling mgapi.dll..."
"$CC" "${cflags[@]}" -shared -o "$dll" "${mgapiSrcs[@]}" \
    -lws2_32 -lwinmm -lole32 \
    || die "mgapi.dll link failed"
step "built $dll"

# --- Build mgapi_host_test.exe ------------------------------------
testSrc="$ToolsDir/mgapi_host_test.c"
testExe="$BuildDir/mgapi_host_test.exe"
if [ -f "$testSrc" ]; then
    step "compiling mgapi_host_test.exe..."
    "$CC" -Wall -Wextra -Wpedantic -Werror -std=c11 \
          -D__USE_MINGW_ANSI_STDIO=1 \
          -I"$IncludeDir" \
          -o "$testExe" "$testSrc" \
        || die "mgapi_host_test compile failed"
    step "built $testExe"
else
    step "tools/mgapi_host_test.c not present yet - skipping exerciser"
fi

# --- Bundle mingw runtime DLLs ------------------------------------
mingwBin="/mingw64/bin"
[ -d "$mingwBin" ] || mingwBin="/c/msys64/mingw64/bin"
for r in libgcc_s_seh-1.dll libwinpthread-1.dll; do
    src="$mingwBin/$r"
    if [ -f "$src" ]; then
        cp -f "$src" "$BuildDir/$r"
        step "bundled $r"
    fi
done

echo
step "done. Artifacts in $BuildDir"
