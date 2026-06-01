# ============================================================
#  run-bsnes.ps1 -- start bsnes-plus on a microgarbage SNES ROM.
#
#  Defaults to snes_smoke.sfc -- the self-contained 65816 menu that
#  boots in any SNES emulator. -Boot flag points at snes_boot.bin
#  instead (only useful once the mgapi.dll cart mapper is wired
#  into the bsnes-plus build -- until then the boot blob is just
#  the cart-window image and won't run on its own).
#
#  Usage:
#    .\tools\run-bsnes.ps1                       # smoke ROM, search PATH
#    .\tools\run-bsnes.ps1 -BsnesDir C:\bsnes    # explicit bsnes location
#    .\tools\run-bsnes.ps1 -Boot                 # load snes_boot.bin
#    .\tools\run-bsnes.ps1 -Rom path\to\rom.sfc  # arbitrary ROM
#    .\tools\run-bsnes.ps1 -NoBuild              # skip the snes\build.ps1 step
#
#  Resolution order for bsnes:
#    1. -BsnesDir argument (must contain bsnes.exe or bsnes-plus.exe)
#    2. $env:BSNES_HOME
#    3. Common install paths (bsnes-plus\, ..\bsnes-plus\, %ProgramFiles%)
#    4. bsnes.exe on PATH
#  Sets the working directory to the bsnes dir before launching so
#  bsnes finds its own config / cheats / save-states relative to its
#  own install.
#
#  Public domain (CC0). No warranty.
# ============================================================

param(
    [string]$BsnesDir,
    [string]$Rom,
    [switch]$Boot,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"

function Write-Step($msg) { Write-Host "run-bsnes: $msg" -ForegroundColor Cyan }
function Die($msg) { Write-Host "run-bsnes: ERROR -- $msg" -ForegroundColor Red; exit 1 }

# Repo root = parent of the directory holding this script.
$RepoRoot = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Write-Step "repo: $RepoRoot"

# ---- 1. Pick the ROM ----------------------------------------
if (-not $Rom) {
    $romName = if ($Boot) { "snes_boot.bin" } else { "snes_smoke.sfc" }
    $Rom = Join-Path $RepoRoot "snes\build\$romName"
}
if (-not (Test-Path $Rom)) {
    if ($NoBuild) {
        Die "ROM not found: $Rom  (and -NoBuild was set)"
    }
    Write-Step "ROM missing; running snes\build.ps1 to produce it..."
    $snesBuild = Join-Path $RepoRoot "snes\build.ps1"
    if ($Boot) { & $snesBuild }
    else       { & $snesBuild -Smoke }
    if ($LASTEXITCODE -ne 0) { Die "snes\build.ps1 failed" }
    if (-not (Test-Path $Rom)) { Die "snes\build.ps1 ran but $Rom still missing" }
}
Write-Step "ROM: $Rom"

# ---- 2. Locate bsnes ---------------------------------------
# Probe the given directory itself AND the standard from-source layout
# (<bsnes-plus-checkout>/bsnes/out/) so pointing at a checkout root
# Just Works -- the source build drops the .exe in bsnes/out/.
function Find-Bsnes($dir) {
    if (-not $dir) { return $null }
    $subdirs = @("", "bsnes\out")
    foreach ($sub in $subdirs) {
        $d = if ($sub) { Join-Path $dir $sub } else { $dir }
        foreach ($exe in @("bsnes.exe", "bsnes-plus.exe")) {
            $p = Join-Path $d $exe
            if (Test-Path $p) { return $p }
        }
    }
    return $null
}

$BsnesExe = $null
$BsnesHome = $null

if ($BsnesDir) {
    $BsnesExe = Find-Bsnes $BsnesDir
    if (-not $BsnesExe) { Die "no bsnes(-plus).exe in $BsnesDir (also checked bsnes\out)" }
}
elseif ($env:BSNES_HOME) {
    $BsnesExe = Find-Bsnes $env:BSNES_HOME
    if (-not $BsnesExe) { Die "BSNES_HOME=$($env:BSNES_HOME) has no bsnes(-plus).exe (also checked bsnes\out)" }
}
else {
    # Common locations checked in order. <repo>\..\bsnes-plus matches
    # the layout where bsnes-plus is cloned next to microgarbage --
    # which is John's setup and probably anyone else's working from
    # the same Source/ directory.
    $candidates = @(
        (Join-Path $RepoRoot "bsnes-plus"),
        (Join-Path (Split-Path -Parent $RepoRoot) "bsnes-plus"),
        "$env:ProgramFiles\bsnes-plus",
        "${env:ProgramFiles(x86)}\bsnes-plus"
    )
    foreach ($d in $candidates) {
        $p = Find-Bsnes $d
        if ($p) { $BsnesExe = $p; break }
    }
    # Last resort: anything on PATH.
    if (-not $BsnesExe) {
        $onPath = Get-Command "bsnes.exe","bsnes-plus.exe" -ErrorAction SilentlyContinue |
                  Select-Object -First 1
        if ($onPath) { $BsnesExe = $onPath.Source }
    }
}

# Working directory is wherever the .exe actually lives -- bsnes-plus
# from-source builds load cheats / save-states / config relative to
# the exe, which is in bsnes\out, NOT the checkout root.
if ($BsnesExe) { $BsnesHome = Split-Path -Parent $BsnesExe }

# Refresh mgapi.dll into bsnes-out if the build copy is newer. The
# bsnes-plus cart class LoadLibrary's mgapi.dll from alongside
# bsnes.exe; without this step, every rebuild needs a manual copy or
# bsnes runs against the previous build's DLL and the symptoms look
# bizarre (PuTTY silent, no shell, mgapi banner from the wrong version).
if ($BsnesHome) {
    $srcDll = Join-Path $RepoRoot "build\mgapi\mgapi.dll"
    $dstDll = Join-Path $BsnesHome "mgapi.dll"
    if (Test-Path $srcDll) {
        $needCopy = -not (Test-Path $dstDll)
        if (-not $needCopy) {
            $needCopy = (Get-Item $srcDll).LastWriteTime -gt
                        (Get-Item $dstDll).LastWriteTime
        }
        if ($needCopy) {
            try {
                Copy-Item -Force $srcDll $dstDll -ErrorAction Stop
                Write-Step "refreshed $dstDll"
            } catch {
                # Copy fails when a running bsnes still holds the DLL
                # (Windows file locking). Loud failure prevents the
                # user from relaunching against yesterday's DLL.
                Die "cannot refresh $dstDll -- another bsnes instance still has it loaded? close it and re-run."
            }
        } else {
            Write-Step "mgapi.dll already current"
        }
    }
}

if (-not $BsnesExe) {
    Write-Host "" -ForegroundColor Red
    Write-Host "run-bsnes: couldn't find bsnes(-plus).exe." -ForegroundColor Red
    Write-Host "  Tried -BsnesDir, `$env:BSNES_HOME, common paths, and PATH." -ForegroundColor Red
    Write-Host "  Pass -BsnesDir <path> or set BSNES_HOME, e.g.:" -ForegroundColor Red
    Write-Host "    `$env:BSNES_HOME = 'C:\bsnes-plus'" -ForegroundColor Yellow
    exit 1
}
Write-Step "bsnes: $BsnesExe"

# ---- 3. Launch ---------------------------------------------
# Probe for an MSYS2 Qt5 install and prepend its bin to PATH if found.
# A from-source bsnes-plus built against MSYS2 Qt links Qt5Widgets.dll
# dynamically; without the path the .exe fails to launch with
# "Qt5Widgets.dll: cannot open shared object file." A prebuilt nightly
# that bundles its DLLs alongside the .exe doesn't need this -- the
# extra PATH entry is harmless if Qt isn't installed.
$qtCandidates = @(
    "C:\msys64\mingw64\bin",
    "C:\msys64\clang64\bin",
    "$env:MSYS2_HOME\mingw64\bin"
) | Where-Object { $_ -and (Test-Path (Join-Path $_ "Qt5Widgets.dll")) }
if ($qtCandidates) {
    $qtBin = $qtCandidates[0]
    $env:PATH = "$qtBin;$env:PATH"
    Write-Step "Qt5 bin: $qtBin"
}

# Use Start-Process so bsnes's working dir is its own install (it
# looks for cheats / save-states / config there). The ROM path stays
# absolute so the cwd swap is invisible to the load.
$RomAbs = (Resolve-Path $Rom).Path
Write-Step "launching..."
Start-Process -FilePath $BsnesExe `
              -ArgumentList @("`"$RomAbs`"") `
              -WorkingDirectory $BsnesHome
Write-Step "done. (close the bsnes window to exit)"
