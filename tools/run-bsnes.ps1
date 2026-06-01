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
function Find-Bsnes($dir) {
    if (-not $dir) { return $null }
    foreach ($exe in @("bsnes.exe", "bsnes-plus.exe")) {
        $p = Join-Path $dir $exe
        if (Test-Path $p) { return $p }
    }
    return $null
}

$BsnesExe = $null
$BsnesHome = $null

if ($BsnesDir) {
    $BsnesExe = Find-Bsnes $BsnesDir
    if (-not $BsnesExe) { Die "no bsnes(-plus).exe in $BsnesDir" }
    $BsnesHome = $BsnesDir
}
elseif ($env:BSNES_HOME) {
    $BsnesExe = Find-Bsnes $env:BSNES_HOME
    if (-not $BsnesExe) { Die "BSNES_HOME=$($env:BSNES_HOME) has no bsnes(-plus).exe" }
    $BsnesHome = $env:BSNES_HOME
}
else {
    # Common locations checked in order.
    $candidates = @(
        (Join-Path $RepoRoot "bsnes-plus"),
        (Join-Path (Split-Path -Parent $RepoRoot) "bsnes-plus"),
        "$env:ProgramFiles\bsnes-plus",
        "${env:ProgramFiles(x86)}\bsnes-plus"
    )
    foreach ($d in $candidates) {
        $p = Find-Bsnes $d
        if ($p) { $BsnesExe = $p; $BsnesHome = $d; break }
    }
    # Last resort: anything on PATH.
    if (-not $BsnesExe) {
        $onPath = Get-Command "bsnes.exe","bsnes-plus.exe" -ErrorAction SilentlyContinue |
                  Select-Object -First 1
        if ($onPath) { $BsnesExe = $onPath.Source; $BsnesHome = Split-Path -Parent $onPath.Source }
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
# Use Start-Process so bsnes's working dir is its own install (it
# looks for cheats / save-states / config there). The ROM path stays
# absolute so the cwd swap is invisible to the load.
$RomAbs = (Resolve-Path $Rom).Path
Write-Step "launching..."
Start-Process -FilePath $BsnesExe `
              -ArgumentList @("`"$RomAbs`"") `
              -WorkingDirectory $BsnesHome
Write-Step "done. (close the bsnes window to exit)"
