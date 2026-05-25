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

.PARAMETER Release
    Size-optimized, stripped distributable: host.exe built -Os -DNDEBUG
    and stripped (-s; drops ~180 KB of DWARF mingw emits by default),
    spawnable guests built -Os and stripped. This is the DEFAULT mode.

.PARAMETER DebugBuild
    Debug build: host.exe built -Og -g3 -DDEBUG with full symbols and
    assertions on (unstripped), guest ELFs built -Og -g (unstripped).
    (Named -DebugBuild, not -Debug, because -Debug is a reserved
    PowerShell common parameter.) Wins if both -Release and -DebugBuild
    are passed. The embedded shell is size-built regardless of mode.

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
    .\build-win.ps1 -DebugBuild
    Build a debuggable host.exe (-Og -g3, symbols + asserts).

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
    [switch]$DebugBuild,
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

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# ----------------------------------------------------------------
# Host source set — read from the SHARED manifest (host_sources.txt) so
# build-win.ps1 and build-win.sh can never drift. One list, both scripts.
# ----------------------------------------------------------------
$manifest = Join-Path $ExampleDir "host_sources.txt"
$hostSrcs = Get-Content $manifest |
    ForEach-Object { ($_ -replace '#.*$', '').Trim() } |
    Where-Object   { $_ -ne '' } |
    ForEach-Object { Join-Path $RepoRoot $_ }

$hostMain = Join-Path $ExampleDir "host.c"
$hostExe  = Join-Path $BuildDir "host.exe"

$cflags = @(
    "-Wall", "-Wextra", "-Wpedantic", "-std=c11",
    # msvcrt's printf doesn't understand C99 %z/%ll length modifiers;
    # this makes mingw use its own C99-compliant stdio so size_t
    # format specifiers (%zu) compile clean. Without it, native
    # builds warn on every %zu in the host.
    "-D__USE_MINGW_ANSI_STDIO=1",
    "-I$IncludeDir"
)
# Warnings-as-errors, ON by default (enforces zero-warning state).
# Pass -NoWerror if a stricter mingw flags something unexpected.
if (-not $NoWerror) { $cflags += "-Werror" }

# Build mode: release (default) or debug. -DebugBuild flips to debug and
# wins over -Release if both are passed. release: -Os -DNDEBUG + strip
# host.exe (-s drops ~180 KB of DWARF mingw emits by default) + size-built
# stripped guests. debug: -Og -g3 -DDEBUG, symbols + asserts, guests -Og -g
# unstripped. The embedded shell is size-built regardless (baked image).
$mode = if ($DebugBuild) { "debug" } else { "release" }
if ($mode -eq "debug") {
    Write-Step "DEBUG build - host -Og -g3 (symbols + asserts), guests -Og -g"
    $cflags += @("-Og", "-g3", "-DDEBUG")
    $guestOpt   = @("-Og", "-g")
    $guestStrip = @()
} else {
    Write-Step "RELEASE build - host -Os -DNDEBUG -s (stripped), guests -Os stripped"
    $cflags += @("-Os", "-DNDEBUG", "-s")
    $guestOpt   = @("-Os")
    $guestStrip = @("-Wl,-s")
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

Write-Step "compiling native host.exe..."
$allSrc = @($hostMain) + @($shellDataC) + $hostSrcs
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
        # Opt level ($guestOpt) and strip ($guestStrip) come from the build
        # mode above: release -> -Os + -Wl,-s; debug -> -Og -g, unstripped.
        $guestCflags = @("-march=rv32imc","-mabi=ilp32",
                         "-nostdlib","-nostartfiles","-ffreestanding")
        $gcCflags  = @("-ffunction-sections","-fdata-sections")
        $gcLdflags = @("-Wl,--gc-sections","-Wl,-z,max-page-size=4")

        # shell.elf was already built for size and embedded earlier;
        # don't rebuild it here (would overwrite the size-optimized
        # on-disk copy with the plain one). Spawnable demos below only.

        # Spawnable guests under host_files_src/*.c, linked against the
        # shared guest SDK (examples\common\guest). Output to host_files/.
        New-Item -ItemType Directory -Force -Path $HostFiles | Out-Null
        $guestSdk = Join-Path $RepoRoot "examples\common\guest"
        $libInc   = Join-Path $ExampleDir "host_files_src"
        $libInc2  = Join-Path $guestSdk "include"
        $libSrcs  = Get-ChildItem -Path $guestSdk -Filter *.c | ForEach-Object { $_.FullName }
        Get-ChildItem -Path (Join-Path $ExampleDir "host_files_src") -Filter *.c |
        ForEach-Object {
            $name = $_.BaseName
            $outElf = Join-Path $HostFiles "$name.elf"
            Write-Step "compiling host_files\$name.elf (spawnable)..."
            $gargs = $guestCflags + $guestOpt + $gcCflags +
                     @("-I$libInc","-I$guestSdk","-I$libInc2","-Wl,-T,$guestLd") +
                     $gcLdflags + $guestStrip + @("-o",$outElf,$_.FullName) + $libSrcs
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
