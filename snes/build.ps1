<#
  build.ps1 — assemble + link the SNES image with cc65 (ca65/ld65).

  Needs ca65 and ld65 on PATH (cc65 suite).

    .\snes\build.ps1            # real kernel  -> build/snes_boot.bin
    .\snes\build.ps1 -Smoke     # smoke test   -> build/snes_smoke.sfc

  The smoke build links smoke.s instead of kernel.s and defines SMOKE_TEST
  (boot.s then skips the copro handshake), giving a standalone HiROM that runs
  in stock bsnes: hold a button and the backdrop cycles colours.
#>
param([switch]$Smoke)
$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$out  = Join-Path $here "build"
New-Item -ItemType Directory -Force $out | Out-Null

if (-not (Get-Command ca65 -ErrorAction SilentlyContinue)) {
    throw "ca65 not found on PATH. Install the cc65 suite (ca65 + ld65)."
}

if ($Smoke) {
    $kernelSrc = "smoke.s";  $bootDef = @("-D", "SMOKE_TEST"); $outName = "snes_smoke.sfc"
} else {
    $kernelSrc = "kernel.s"; $bootDef = @();                   $outName = "snes_boot.bin"
}

ca65 --cpu 65816 @bootDef -o "$out\boot.o"   "$here\boot.s"
ca65 --cpu 65816          -o "$out\kernel.o" "$here\$kernelSrc"
ld65 -C "$here\snes.cfg" -o "$out\$outName" "$out\boot.o" "$out\kernel.o"

Write-Host "wrote $out\$outName ($((Get-Item "$out\$outName").Length) bytes)"
