<#
.SYNOPSIS
    Build mgapi.dll (the cart runtime) and mgapi_host_test.exe.

.DESCRIPTION
    Stage 1 of the cart-runtime work: a minimal Windows DLL that
    exposes the cart-window seam (mgapi_cart_read / mgapi_post_joypads
    / mgapi_step / mgapi_audio_pull) plus a developer back-door
    (mgapi_dev_load_blob) so the test harness can stage a smoke ROM
    into the window without the full VM/ecall machinery being wired
    up yet.

    The DLL is a strict subset of the eventual runtime; later stages
    add audio, VM, shell-over-TCP, etc. — additively, no ABI change.

    Tested with the same mingw-w64 toolchain build-win.ps1 uses.

.PARAMETER Clean
    Remove the build output and exit.

.PARAMETER Cc
    Override the C compiler. Default: x86_64-w64-mingw32-gcc, with
    plain 'gcc' as a fallback if it's a native mingw build.
#>

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$NoGuest,
    [string]$Cc = "x86_64-w64-mingw32-gcc",
    [string]$GuestCc = ""
)

$ErrorActionPreference = "Stop"

# ----------------------------------------------------------------
# Toolchain bootstrap — mirror build-win.ps1's logic so this works
# from a stock PowerShell on a standard MSYS2 install.
# ----------------------------------------------------------------
if (-not (Get-Command $Cc -ErrorAction SilentlyContinue) -and
    -not (Get-Command "gcc" -ErrorAction SilentlyContinue)) {
    $mingw = "C:\msys64\mingw64\bin"
    if (Test-Path (Join-Path $mingw "gcc.exe")) {
        $env:PATH = "$mingw;C:\msys64\usr\bin;" + $env:PATH
    }
}
if ($env:TEMP -match '\s') {
    $mgtmp = Join-Path $env:SystemDrive "mg_tmp"
    New-Item -ItemType Directory -Force $mgtmp | Out-Null
    $env:TMP = $mgtmp; $env:TEMP = $mgtmp; $env:TMPDIR = $mgtmp
}

$RepoRoot  = Split-Path -Parent $MyInvocation.MyCommand.Path
$BuildDir  = Join-Path $RepoRoot "build\mgapi"
$IncludeDir = Join-Path $RepoRoot "include"
$MgapiSrcDir = Join-Path $RepoRoot "src\mgapi"
$ToolsDir  = Join-Path $RepoRoot "tools"

function Write-Step($msg) { Write-Host "build-mgapi: $msg" -ForegroundColor Cyan }
function Die($msg) { Write-Host "build-mgapi: ERROR - $msg" -ForegroundColor Red; exit 1 }

if ($Clean) {
    if (Test-Path $BuildDir) { Remove-Item -Recurse -Force $BuildDir }
    Write-Step "cleaned"; exit 0
}

# ----------------------------------------------------------------
# Compiler — same fallback dance as build-win.ps1.
# ----------------------------------------------------------------
$ccPath = Get-Command $Cc -ErrorAction SilentlyContinue
if (-not $ccPath -and $Cc -eq "x86_64-w64-mingw32-gcc") {
    $alt = Get-Command "gcc" -ErrorAction SilentlyContinue
    if ($alt) {
        $altMachine = (& gcc -dumpmachine 2>$null)
        if ($altMachine -match "mingw|w64.*windows") {
            Write-Step "using 'gcc' (target $altMachine) - native Windows OK"
            $Cc = "gcc"; $ccPath = $alt
        }
    }
}
if (-not $ccPath) { Die "compiler '$Cc' not found on PATH" }

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# ----------------------------------------------------------------
# Build mgapi.dll
# ----------------------------------------------------------------
$dll = Join-Path $BuildDir "mgapi.dll"
$SrcDir = Join-Path $RepoRoot "src"

# Mgapi-side glue.
$mgapiSrcs = @(
    (Join-Path $MgapiSrcDir "mgapi_init.c"),
    (Join-Path $MgapiSrcDir "cart_window.c"),
    (Join-Path $MgapiSrcDir "psram_pool.c"),
    (Join-Path $MgapiSrcDir "audio_init.c"),
    (Join-Path $MgapiSrcDir "cart_volume.c"),
    (Join-Path $MgapiSrcDir "l2_alloc.c"),
    (Join-Path $MgapiSrcDir "l2_init.c")
)

# Stage 2c: trashfs implementation. The /cart/ volume formats + mounts on
# the 1 MB PSRAM slice; stage 3 registers it with the VM fs mount table.
$mgapiSrcs += @(Join-Path $RepoRoot "src\storage\trashfs.c")

# Stage 3a: VM core dependency closure. Mirrors the VM-section of
# examples/05_shell/host_sources.txt. We skip the preemptive scheduler bits
# (presched.c) — the cart runtime is cooperative-only. We also skip
# vm_host_tui.c for now since the cart-side game won't use the terminal
# canvas; add it back if the shell turns out to need it.
$vmSrcs = @(
    "src\vm\vm_core.c",
    "src\vm\vm_loader.c",
    "src\vm\vm_ecall.c",
    "src\vm\vm_ecall_handlers.c",
    "src\vm\vm_mailbox.c",
    "src\vm\vm_sched.c",
    "src\vm\vm_sched_ops_coop.c",
    "src\vm\vm_sched_ops_pre.c",
    "src\vm\vm_system.c",
    "src\vm\vm_host_stdio.c",
    "src\vm\vm_host_stdio_win32.c",
    "src\vm\vm_host_platform.c",
    "src\vm\vm_host_fs.c",
    "src\vm\vm_host_fs_spawn.c",
    "src\vm\vm_host_audio.c",
    "src\memory\bump.c",
    "src\memory\slab_stack.c",
    "src\containers\fifo_queue.c",
    "src\host\platform_win.c"
) | ForEach-Object { Join-Path $RepoRoot $_ }
$mgapiSrcs += $vmSrcs

# Stage 3a: the VM owner module + stage 3b/3c: ecall handlers.
$mgapiSrcs += (Join-Path $MgapiSrcDir "vm_init.c")
$mgapiSrcs += (Join-Path $MgapiSrcDir "copro_ecalls.c")
$mgapiSrcs += (Join-Path $MgapiSrcDir "l2_ecalls.c")

# Stage 4: TCP listener for PuTTY shell sessions.
$mgapiSrcs += (Join-Path $MgapiSrcDir "tcp_listen.c")

# Stage 3a: build the shell guest ELF + bake it into the DLL so the
# embedder doesn't need a separate file. Lifted from build-win.ps1's
# guest-build block. -NoGuest skips this and emits a zero-length stub
# (useful for rebuilds when the cross compiler is missing or the shell
# source hasn't changed).
$genDir = Join-Path $BuildDir "gen"
New-Item -ItemType Directory -Force -Path $genDir | Out-Null
$shellDataC = Join-Path $genDir "shell_elf_data.c"
$baked = $false
if (-not $NoGuest) {
    if (-not $GuestCc) {
        foreach ($cand in @("riscv-none-elf-gcc","riscv64-unknown-elf-gcc",
                             "riscv32-unknown-elf-gcc","riscv64-elf-gcc")) {
            if (Get-Command $cand -ErrorAction SilentlyContinue) { $GuestCc = $cand; break }
        }
    }
    if ($GuestCc -and (Get-Command $GuestCc -ErrorAction SilentlyContinue)) {
        Write-Step "compiling guest shell.elf (RV32IMC)..."
        $exampleDir = Join-Path $RepoRoot "examples\05_shell"
        $guestLd  = Join-Path $RepoRoot "examples\common\guest.ld"
        $shellC   = Join-Path $exampleDir "shell.c"
        $shellElf = Join-Path $BuildDir "shell.elf"
        $gcf = @("-march=rv32imc","-mabi=ilp32","-nostdlib","-nostartfiles",
                 "-ffreestanding","-Os","-ffunction-sections","-fdata-sections")
        $gldf = @("-Wl,--gc-sections","-Wl,-z,max-page-size=4","-Wl,-s")
        & $GuestCc @gcf @gldf "-Wl,-T,$guestLd" "-o" $shellElf $shellC
        if ($LASTEXITCODE -ne 0) { Die "guest shell.elf compile failed" }

        Write-Step "baking shell.elf into mgapi.dll (bin2c)..."
        $bin2c = Join-Path $BuildDir "bin2c.exe"
        & $Cc "-O2" "-o" $bin2c (Join-Path $exampleDir "tools\bin2c.c")
        if ($LASTEXITCODE -ne 0) { Die "bin2c compile failed" }
        & $bin2c $shellElf "shell_elf" $shellDataC
        if ($LASTEXITCODE -ne 0) { Die "bin2c run failed" }
        $baked = $true

        # Build + bake the guest test/demo ELFs. l2_test verifies the
        # stage-3c L2 path; menu drives the SELECT DEMO screen from
        # the copro side (replaces smoke.s's 65816 menu).
        $guestCommon  = Join-Path $RepoRoot "examples\common\guest"
        $guestSources = @(
            @{ src = "tools\guests\l2_test.c"; sym = "l2_test_elf"; out = "l2_test.elf"; gen = "l2_test_elf_data.c" },
            @{ src = "tools\guests\menu.c";    sym = "menu_elf";    out = "menu.elf";    gen = "menu_elf_data.c" }
        )
        foreach ($g in $guestSources) {
            $srcPath = Join-Path $RepoRoot $g.src
            if (-not (Test-Path $srcPath)) { continue }
            Write-Step "compiling $($g.out) (RV32IMC)..."
            $outElf = Join-Path $BuildDir $g.out
            $gflags = $gcf + @("-I$guestCommon")
            & $GuestCc @gflags @gldf "-Wl,-T,$guestLd" "-o" $outElf $srcPath
            if ($LASTEXITCODE -ne 0) { Die "$($g.out) compile failed" }
            $genPath = Join-Path $genDir $g.gen
            & $bin2c $outElf $g.sym $genPath
            if ($LASTEXITCODE -ne 0) { Die "$($g.sym) bin2c failed" }
            $mgapiSrcs += $genPath
        }
    } else {
        Write-Step "RISC-V cross-compiler not found; emitting empty shell stub"
    }
}
if (-not $baked) {
    "#include <stddef.h>`nconst unsigned char shell_elf[] = {0};`nconst size_t shell_elf_len = 0;`nconst unsigned char l2_test_elf[] = {0};`nconst size_t l2_test_elf_len = 0;`nconst unsigned char menu_elf[] = {0};`nconst size_t menu_elf_len = 0;`n" |
        Set-Content -Path $shellDataC -Encoding ASCII
}
$mgapiSrcs += $shellDataC

# Bin2c both SNES window images if pre-built. The DLL has two arrays:
#   smoke_rom[] = boot.s + smoke.s (self-contained "SELECT DEMO" menu in
#                  65816 — runs visibly under bsnes-plus with no copro
#                  guest needed; great for first-light integration tests)
#   boot_rom[]  = boot.s + kernel.s (the runtime kernel; menu logic lives
#                  in a copro guest ELF that uses mg_copro to stage frames)
# Refresh either via:
#   snes\build.ps1 -Smoke   (-> snes_smoke.sfc)
#   snes\build.ps1          (-> snes_boot.bin)
function Bake-Sfc($srcPath, $symbol, $dstC) {
    if ((Test-Path $srcPath) -and (Test-Path (Join-Path $BuildDir "bin2c.exe"))) {
        Write-Step ("baking " + (Split-Path -Leaf $srcPath) +
                    " into mgapi.dll as $symbol[]")
        & (Join-Path $BuildDir "bin2c.exe") $srcPath $symbol $dstC
        if ($LASTEXITCODE -ne 0) { Die "$symbol bin2c failed" }
        return $true
    }
    Write-Step "$symbol stub (no $(Split-Path -Leaf $srcPath))"
    "#include <stddef.h>`nconst unsigned char $symbol`[] = {0};`nconst size_t $symbol`_len = 0;`n" |
        Set-Content -Path $dstC -Encoding ASCII
    return $false
}
$smokeDataC  = Join-Path $genDir "smoke_rom_data.c"
$bootDataC   = Join-Path $genDir "boot_rom_data.c"
Bake-Sfc (Join-Path $RepoRoot "snes\build\snes_smoke.sfc") "smoke_rom" $smokeDataC | Out-Null
Bake-Sfc (Join-Path $RepoRoot "snes\build\snes_boot.bin")  "boot_rom"  $bootDataC  | Out-Null
$mgapiSrcs += $smokeDataC
$mgapiSrcs += $bootDataC

# Stage 2b1: audio dep closure. Mirrors examples/05_shell/host_sources.txt
# lines 41-57 minus the two sink backends — we drain the mixer into our
# own ring instead of pushing to waveOut/wav. file_stream is here because
# audio_service.h includes it (the AudioFileReader vtable type); we leave
# the reader pointers NULL in stage 2b1 so STREAM_WAV requests get rejected
# cleanly until stage 3+ wires real readers.
$audioSrcs = @(
    "src\vm\channel_win32.c",
    "src\vm\service_channel.c",
    "src\containers\spsc_ring.c",
    "src\containers\bitset.c",
    "src\containers\ring_buffer.c",
    "src\audio\audio_service.c",
    "src\audio\audio_arbiter.c",
    "src\audio\audio_pool.c",
    "src\audio\audio_pool_stream.c",
    "src\audio\audio_mixer.c",
    "src\audio\music_player.c",
    "src\audio\audio_fft.c",
    "src\audio\audio_fft_kernel.c",
    "src\audio\audio_wav_read.c",
    "src\audio\audio_file_stream.c"
) | ForEach-Object { Join-Path $RepoRoot $_ }

$mgapiSrcs += $audioSrcs

$cflags = @(
    "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-std=c11",
    "-D__USE_MINGW_ANSI_STDIO=1",
    "-DMGAPI_BUILDING_DLL=1",
    "-I$IncludeDir",
    "-I$MgapiSrcDir"
)

Write-Step "compiling mgapi.dll..."
# No --out-implib: embedders use LoadLibrary + GetProcAddress, and the
# default mingw import-lib path would contain a space (the repo root has
# one) which the linker can't quote inside the -Wl, comma list.
$dllArgs = $cflags + @(
    "-shared",
    "-o", $dll
) + $mgapiSrcs + @("-lws2_32", "-lwinmm")
& $Cc @dllArgs
if ($LASTEXITCODE -ne 0) { Die "mgapi.dll link failed (exit $LASTEXITCODE)" }
Write-Step "built $dll"

# ----------------------------------------------------------------
# Build the test harness — links against mgapi.dll via its import
# library; LoadLibrary path is exercised by a separate test mode.
# ----------------------------------------------------------------
$testSrc = Join-Path $ToolsDir "mgapi_host_test.c"
$testExe = Join-Path $BuildDir "mgapi_host_test.exe"
if (Test-Path $testSrc) {
    Write-Step "compiling mgapi_host_test.exe..."
    $testArgs = @(
        "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-std=c11",
        "-D__USE_MINGW_ANSI_STDIO=1",
        "-I$IncludeDir",
        "-o", $testExe,
        $testSrc
    )
    & $Cc @testArgs
    if ($LASTEXITCODE -ne 0) { Die "mgapi_host_test compile failed" }
    Write-Step "built $testExe"
} else {
    Write-Step "tools\mgapi_host_test.c not present yet - skipping exerciser"
}

# Copy the mingw runtime DLLs next to the artifacts so LoadLibrary
# finds them without modifying the user's PATH. The DLL drags in
# libgcc_s_seh-1.dll for arithmetic helpers (64-bit divides, etc.),
# which transitively depends on libwinpthread-1.dll. bsnes-plus
# integrators get the same auto-bundled set.
$mingwBin = "C:\msys64\mingw64\bin"
$runtimes = @("libgcc_s_seh-1.dll", "libwinpthread-1.dll")
foreach ($r in $runtimes) {
    $src = Join-Path $mingwBin $r
    if (Test-Path $src) {
        Copy-Item -Force $src (Join-Path $BuildDir $r)
        Write-Step "bundled $r alongside artifacts"
    }
}

Write-Host ""
Write-Step "done. Artifacts in $BuildDir"
