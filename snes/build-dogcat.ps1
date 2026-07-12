<#
  build-dogcat.ps1 — synthesize the dog/cat images then assemble the standalone
  rolling+siphon flip test -> snes/gen/dogcat_test.sfc (128 KB HiROM).

  Needs: gcc (image gen) + ca65/ld65 (cc65 suite) on PATH.
  Usage:  powershell -File snes\build-dogcat.ps1
          -NoGen   skip regenerating the images (just reassemble the ROM)
#>
[CmdletBinding()]
param([switch]$NoGen)
$ErrorActionPreference = "Stop"
$env:TMP="C:\mg_tmp"; $env:TEMP="C:\mg_tmp"
# mingw64 for gcc (image gen); harmless if already present.
if(Test-Path "C:\msys64\mingw64\bin\gcc.exe"){
    $env:PATH="C:\msys64\mingw64\bin;C:\msys64\usr\bin;"+$env:PATH
}
$snes = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $snes
$gen  = Join-Path $snes "gen\dogcat"
New-Item -ItemType Directory -Force $gen | Out-Null

if(-not $NoGen){
    Write-Host "build-dogcat: generating dog/cat images..." -ForegroundColor Cyan
    $gcc = (Get-Command gcc).Source
    & $gcc -O2 -o "$gen\gen_dogcat.exe" (Join-Path $root "tools\gen_dogcat.c") -lm
    if($LASTEXITCODE){ throw "gen_dogcat compile failed" }
    & "$gen\gen_dogcat.exe" $gen
    if($LASTEXITCODE){ throw "gen_dogcat run failed" }
}

Write-Host "build-dogcat: assembling ROM..." -ForegroundColor Cyan
Push-Location $snes
try {
    & ca65 --cpu 65816 -o "gen\dogcat_test.o" "dogcat_test.s"
    if($LASTEXITCODE){ throw "ca65 failed" }
    & ld65 -C "dogcat_test.cfg" -o "gen\dogcat_test.sfc" "gen\dogcat_test.o"
    if($LASTEXITCODE){ throw "ld65 failed" }
} finally { Pop-Location }
$sfc = Join-Path $gen "..\dogcat_test.sfc"
Write-Host ("build-dogcat: done -> {0}" -f (Resolve-Path $sfc)) -ForegroundColor Green
