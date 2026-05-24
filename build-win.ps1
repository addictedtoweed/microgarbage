<#
.SYNOPSIS
    Build the microgarbage shell host as a NATIVE Windows .exe.

.DESCRIPTION
    Produces a self-contained native Windows host (build\host.exe for
    the 05_shell example) using mingw-w64. The resulting binary does
    NOT depend on the Cygwin DLL and uses the Win32 code paths
    (WinSock2, SetConsoleMode, etc.) — i.e. the `_WIN32 && !__CYGWIN__`
    branches in the source.

    This is the canonical *shippable* build. The Cygwin build.sh
    remains for fast local iteration and for running the unit-test
    suites (which are platform-neutral); it produces a Cygwin-linked
    binary that exercises the POSIX paths instead.

    You can run this from PowerShell directly, OR from a Cygwin shell
    via:  powershell.exe -ExecutionPolicy Bypass -File build-win.ps1
    The compiler — not the shell you launch from — decides the target,
    and mingw-w64 always emits native Windows binaries.

.PARAMETER Clean
    Remove the build output and exit.

.PARAMETER NoGuest
    Skip building the RISC-V guest ELFs (host only). Useful if you
    don't have the RISC-V cross-compiler installed and just want to
    relink the host.

.PARAMETER Cc
    Override the host C compiler. Default: x86_64-w64-mingw32-gcc.
    Must be a mingw-w64 (native Windows) compiler for a native build.

.PARAMETER GuestCc
    Override the RISC-V guest cross-compiler. Default: auto-detect
    among riscv-none-elf-gcc, riscv64-unknown-elf-gcc, etc.

.EXAMPLE
    .\build-win.ps1
    Build host.exe and the guest ELFs.

.EXAMPLE
    .\build-win.ps1 -NoGuest
    Relink just the native host.

.EXAMPLE
    .\build-win.ps1 -Clean
    Remove build artifacts.
#>

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$NoGuest,
    [switch]$NoWerror,
    [switch]$Release,
    [string]$Cc = "x86_64-w64-mingw32-gcc",
    [string]$GuestCc = ""
)

$ErrorActionPreference = "Stop"

# ----------------------------------------------------------------
# Locate repo root (this script lives at the repo root) and the
# 05_shell example.
# ----------------------------------------------------------------
$RepoRoot   = Split-Path -Parent $MyInvocation.MyCommand.Path
$ExampleDir = Join-Path $RepoRoot "examples\05_shell"
$BuildDir   = Join-Path $ExampleDir "build"
$HostFiles  = Join-Path $ExampleDir "host_files"
$FatfsDir   = Join-Path $RepoRoot "third_party\fatfs"
$FatfsSrc   = Join-Path $FatfsDir "source"
$IncludeDir = Join-Path $RepoRoot "include"

function Write-Step($msg) { Write-Host "build-win: $msg" -ForegroundColor Cyan }
function Write-Warn($msg) { Write-Host "build-win: $msg" -ForegroundColor Yellow }
function Die($msg) { Write-Host "build-win: ERROR - $msg" -ForegroundColor Red; exit 1 }

# ----------------------------------------------------------------
# Clean
# ----------------------------------------------------------------
if ($Clean) {
    if (Test-Path $BuildDir)  { Remove-Item -Recurse -Force $BuildDir }
    if (Test-Path $HostFiles) { Remove-Item -Recurse -Force $HostFiles }
    Write-Step "cleaned"
    exit 0
}

# ----------------------------------------------------------------
# Verify the host compiler exists and IS native (mingw).
# ----------------------------------------------------------------
$ccPath = Get-Command $Cc -ErrorAction SilentlyContinue
if (-not $ccPath -and $Cc -eq "x86_64-w64-mingw32-gcc") {
    # The prefixed name isn't installed, but MSYS2's mingw-w64-x86_64-gcc
    # also provides a plain 'gcc' that targets x86_64-w64-mingw32. Fall
    # back to it for the DEFAULT only (an explicit -Cc is honoured as-is).
    $alt = Get-Command "gcc" -ErrorAction SilentlyContinue
    if ($alt) {
        $altMachine = (& gcc -dumpmachine 2>$null)
        if ($altMachine -match "mingw|w64.*windows") {
            Write-Step "host compiler '$Cc' not found; using native 'gcc' (target $altMachine)"
            $Cc = "gcc"
            $ccPath = $alt
        }
    }
}
if (-not $ccPath) {
    Die ("host compiler '$Cc' not found on PATH.`n" +
         "      Install mingw-w64 (e.g. via MSYS2: 'pacman -S " +
         "mingw-w64-x86_64-gcc',`n" +
         "      or the Cygwin package 'mingw64-x86_64-gcc-core'), " +
         "or pass -Cc <compiler>.")
}

$machine = (& $Cc -dumpmachine 2>$null)
if ($machine -notmatch "mingw|w64.*windows") {
    Write-Warn ("compiler '$Cc' targets '$machine', which does not look " +
                "like native Windows.")
    Write-Warn ("a native build wants a mingw-w64 compiler. Continuing, " +
                "but the result may link the Cygwin DLL or POSIX paths.")
} else {
    Write-Step "host compiler: $Cc (target $machine) - native Windows OK"
}

# ----------------------------------------------------------------
# FatFs presence check (the shell host links f_open/f_read/etc.).
# ----------------------------------------------------------------
$fatfsFfC = Join-Path $FatfsSrc "ff.c"
if (-not (Test-Path $fatfsFfC)) {
    Die ("FatFs source not found at $fatfsFfC.`n" +
         "      This example needs FatFs downloaded and extracted.`n" +
         "      See third_party\fatfs\PLACEHOLDER.md for instructions.")
}

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# ----------------------------------------------------------------
# Host source set — mirrors examples/common/vm_objs.sh VM_CORE_SRCS
# plus the shell host's extra deps (fs, trashdrive, FatFs, inicfg).
# ----------------------------------------------------------------
$vmCore = @(
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
    "src\vm\vm_host_tui.c",
    "src\memory\bump.c",
    "src\memory\slab_stack.c",
    "src\containers\fifo_queue.c",
    "src\containers\ring_buffer.c"
) | ForEach-Object { Join-Path $RepoRoot $_ }

$hostExtra = @(
    "src\host\platform_win.c",
    "src\vm\vm_host_fs.c",
    "src\storage\trashdrive.c",
    "src\storage\trashfs.c",
    "src\storage\trashdrive_fatfs.c",
    "src\util\inicfg.c",
    # Audio (native Windows): channel_win32.c is the Win32 transport
    # (CreateThread + CONDITION_VARIABLE), NOT channel_thread.c (pthreads).
    # Gives a standalone .exe live audio with no pthread/Cygwin dep.
    # NOTE: this .ps1 is MIRROR-ONLY of build-win.sh and is UNVERIFIED by
    # the author's toolchain — the .sh is the verified build path.
    "src\vm\channel_win32.c",
    "src\vm\service_channel.c",
    "src\vm\vm_host_audio.c",
    "src\containers\spsc_ring.c",
    "src\audio\audio_service.c",
    "src\audio\audio_arbiter.c",
    "src\audio\audio_pool.c",
    "src\audio\audio_pool_stream.c",
    "src\audio\audio_mixer.c",
    "src\audio\music_player.c",
    "src\audio\audio_fft.c",
    "src\audio\audio_fft_kernel.c",
    "src\audio\audio_wav_read.c",
    "src\audio\audio_file_stream.c",
    "src\audio\audio_sink_wav.c",
    "src\audio\audio_sink_waveout.c"
) | ForEach-Object { Join-Path $RepoRoot $_ }

$fatfs = @(
    (Join-Path $FatfsDir "ff_wrapped.c"),
    (Join-Path $FatfsSrc "ffsystem.c")
)

$hostMain = Join-Path $ExampleDir "host.c"
$hostExe  = Join-Path $BuildDir "host.exe"

$cflags = @(
    "-Wall", "-Wextra", "-Wpedantic", "-std=c11", "-Os",
    "-DHAVE_FATFS",
    # msvcrt's printf doesn't understand C99 %z/%ll length modifiers;
    # this makes mingw use its own C99-compliant stdio so size_t
    # format specifiers (%zu) compile clean. Without it, native
    # builds warn on every %zu in the host.
    "-D__USE_MINGW_ANSI_STDIO=1",
    "-I$IncludeDir",
    "-I$FatfsDir",
    "-I$FatfsSrc"
)
# Warnings-as-errors, ON by default (enforces zero-warning state).
# Pass -NoWerror if a stricter mingw flags something unexpected.
if (-not $NoWerror) { $cflags += "-Werror" }

# -Release: size-optimized, stripped distributable. Strips host.exe
# (-s; drops ~180 KB of DWARF mingw emits by default) and builds
# spawnable guests for size (-Os). Embedded shell is size-built
# regardless. Default keeps host symbols for development.
$guestOpt = @()
if ($Release) {
    Write-Step "RELEASE build - stripping host.exe, -Os guests"
    $cflags += "-s"
    $guestOpt = @("-Os")
}

# Native Windows needs WinSock2 for the TCP transport, winmm for the
# waveOut audio backend. -static bundles the mingw runtime so the .exe
# is a single self-contained file for testers (no DLL hunt); the win32
# audio path uses Win32 threads directly, so nothing pulls libwinpthread.
$libs = @("-static", "-lws2_32", "-lwinmm")

# ----------------------------------------------------------------
# Bake the guest shell into host.exe (XIP-executed at runtime). The
# host embeds build\shell.elf via tools\bin2c, so a distributed
# host.exe needs no external .elf. shell.elf must exist before the
# host compile; if guests are skipped or the cross is missing, emit
# an empty stub (host then needs an explicit ELF path).
# ----------------------------------------------------------------
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
        $guestLd  = Join-Path $RepoRoot "examples\common\guest.ld"
        $shellC   = Join-Path $ExampleDir "shell.c"
        $shellElf = Join-Path $BuildDir "shell.elf"
        $gcf = @("-march=rv32imc","-mabi=ilp32","-nostdlib","-nostartfiles","-ffreestanding","-Os","-ffunction-sections","-fdata-sections")
        $gldf = @("-Wl,--gc-sections","-Wl,-z,max-page-size=4","-Wl,-s")
        Write-Step "compiling guest shell.elf for embedding (RV32IMC)..."
        & $GuestCc @gcf @gldf "-Wl,-T,$guestLd" "-o" $shellElf $shellC
        if ($LASTEXITCODE -ne 0) { Die "guest shell.elf compile failed" }
        Write-Step "baking shell.elf into host.exe (bin2c)..."
        # On native Windows $Cc is itself native, so it's fine to build
        # bin2c with it (produces a Windows exe that runs here).
        $bin2c = Join-Path $BuildDir "bin2c.exe"
        & $Cc "-O2" "-o" $bin2c (Join-Path $ExampleDir "tools\bin2c.c")
        if ($LASTEXITCODE -ne 0) { Die "bin2c compile failed" }
        & $bin2c $shellElf "shell_elf" $shellDataC
        if ($LASTEXITCODE -ne 0) { Die "bin2c run failed" }
        $baked = $true
    }
}
if (-not $baked) {
    Write-Step "no embedded shell (-NoGuest or cross missing) - empty stub"
    "#include <stddef.h>`nconst unsigned char shell_elf[] = {0};`nconst size_t shell_elf_len = 0;`n" |
        Set-Content -Path $shellDataC -Encoding ASCII
}

Write-Step "compiling native host.exe (with FatFs)..."
$allSrc = @($hostMain) + @($shellDataC) + $vmCore + $hostExtra + $fatfs
$ccArgs = $cflags + @("-I$ExampleDir") + @("-o", $hostExe) + $allSrc + $libs
& $Cc @ccArgs
if ($LASTEXITCODE -ne 0) { Die "host compile failed (exit $LASTEXITCODE)" }
Write-Step "built $hostExe"

# ----------------------------------------------------------------
# Guest ELFs (RISC-V). These are platform-neutral artifacts — the
# same .elf works whether the host is native or Cygwin — so we use
# the RISC-V cross-compiler directly. Skippable with -NoGuest.
# ----------------------------------------------------------------
if ($NoGuest) {
    Write-Step "skipping guest ELFs (-NoGuest)"
} else {
    if (-not $GuestCc) {
        foreach ($cand in @("riscv-none-elf-gcc","riscv64-unknown-elf-gcc",
                             "riscv32-unknown-elf-gcc","riscv64-elf-gcc")) {
            if (Get-Command $cand -ErrorAction SilentlyContinue) {
                $GuestCc = $cand; break
            }
        }
    }
    if (-not $GuestCc -or -not (Get-Command $GuestCc -ErrorAction SilentlyContinue)) {
        Write-Warn ("RISC-V cross-compiler not found; skipping guest ELFs. " +
                    "Existing ELFs (if any) are left in place.")
        Write-Warn ("Install xPack riscv-none-elf-gcc and re-run, or pass " +
                    "-GuestCc <compiler>.")
    } else {
        Write-Step "guest compiler: $GuestCc"
        $guestLd  = Join-Path $RepoRoot "examples\common\guest.ld"
        $shellC   = Join-Path $ExampleDir "shell.c"
        $shellElf = Join-Path $BuildDir "shell.elf"
        $guestCflags = @("-march=rv32imc","-mabi=ilp32",
                         "-nostdlib","-nostartfiles","-ffreestanding","-O2")
        $gcCflags  = @("-ffunction-sections","-fdata-sections")
        $gcLdflags = @("-Wl,--gc-sections","-Wl,-z,max-page-size=4","-Wl,-s")

        # shell.elf was already built for size and embedded earlier;
        # don't rebuild it here (would overwrite the size-optimized
        # on-disk copy with the plain one). Spawnable demos below only.

        # Spawnable guests under host_files_src/*.c, linked against
        # everything in host_files_src/lib/*.c. Output to host_files/.
        New-Item -ItemType Directory -Force -Path $HostFiles | Out-Null
        $libInc  = Join-Path $ExampleDir "host_files_src"
        $libInc2 = Join-Path $ExampleDir "host_files_src\lib\include"
        $libSrcs = @()
        $libDir  = Join-Path $ExampleDir "host_files_src\lib"
        if (Test-Path $libDir) {
            $libSrcs = Get-ChildItem -Path $libDir -Filter *.c | ForEach-Object { $_.FullName }
        }
        Get-ChildItem -Path (Join-Path $ExampleDir "host_files_src") -Filter *.c |
        ForEach-Object {
            $name = $_.BaseName
            $outElf = Join-Path $HostFiles "$name.elf"
            Write-Step "compiling host_files\$name.elf (spawnable)..."
            $gargs = $guestCflags + $guestOpt + $gcCflags +
                     @("-I$libInc","-I$libInc2","-Wl,-T,$guestLd") +
                     $gcLdflags + @("-o",$outElf,$_.FullName) + $libSrcs
            & $GuestCc @gargs
            if ($LASTEXITCODE -ne 0) { Die "guest $name.elf compile failed" }
        }
    }
}

Write-Host ""
Write-Step "done. Native host:"
Write-Host "    $hostExe"
Write-Host ""
Write-Host "  Run it from PowerShell or cmd:" -ForegroundColor Green
Write-Host "    $hostExe                 # single stdio session"
Write-Host "    $hostExe --tcp=5000      # one TCP session on :5000"
Write-Host "    $hostExe --tcp=5000 --tcp=5001   # two sessions"
Write-Host ""
Write-Host "  Connect with PuTTY (Raw or Telnet), or: nc localhost 5000"
