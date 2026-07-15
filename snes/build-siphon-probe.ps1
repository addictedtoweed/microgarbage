<#
  build-siphon-probe.ps1 — generate the 4bpp dog canvas then assemble the
  interactive H-blank / force-blank timing characterization ROM
  -> snes/gen/siphon_probe.sfc (64 KB).

  The probe decouples the DMA from the display: each in-band scanline the H-IRQ
  does force-blank ON -> GP-DMA (to a WRAM scratch, NOT VRAM) -> force-blank OFF,
  over a static dog image, so on-screen corruption is purely the force-blank
  rendering disturbance. Live controls sweep hdot / DMA width / restore-delay /
  mode; readout on BG2 upper-right. See tools/gen_probe_dog.c + siphon_probe.s.

  Needs: gcc (asset gen) + ca65/ld65 (cc65 suite) on PATH.
  Usage:  powershell -File snes\build-siphon-probe.ps1
          -NoGen   skip regenerating the dog bins (just reassemble the ROM)
#>
[CmdletBinding()]
param([switch]$NoGen)
$ErrorActionPreference = "Stop"
$env:TMP="C:\mg_tmp"; $env:TEMP="C:\mg_tmp"
if(Test-Path "C:\msys64\mingw64\bin\gcc.exe"){
    $env:PATH="C:\msys64\mingw64\bin;C:\msys64\usr\bin;"+$env:PATH
}
$snes = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $snes
$gen  = Join-Path $snes "gen"
New-Item -ItemType Directory -Force $gen | Out-Null

if(-not $NoGen){
    Write-Host "build-siphon-probe: generating dog canvas..." -ForegroundColor Cyan
    $gcc = (Get-Command gcc).Source
    & $gcc -O2 -o "$gen\gen_probe_dog.exe" (Join-Path $root "tools\gen_probe_dog.c") -lm
    if($LASTEXITCODE){ throw "gen_probe_dog compile failed" }
    & "$gen\gen_probe_dog.exe" $gen
    if($LASTEXITCODE){ throw "gen_probe_dog run failed" }
}

Write-Host "build-siphon-probe: assembling ROM..." -ForegroundColor Cyan
Push-Location $snes
try {
    & ca65 --cpu 65816 -o "gen\siphon_probe.o" "siphon_probe.s"
    if($LASTEXITCODE){ throw "ca65 failed" }
    & ld65 -C "siphon_hblank_test.cfg" -o "gen\siphon_probe.sfc" "gen\siphon_probe.o"
    if($LASTEXITCODE){ throw "ld65 failed" }
} finally { Pop-Location }
$sfc = Join-Path $gen "siphon_probe.sfc"
Write-Host ("build-siphon-probe: done -> {0}" -f (Resolve-Path $sfc)) -ForegroundColor Green
