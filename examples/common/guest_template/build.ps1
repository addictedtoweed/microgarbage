# build.ps1 — build this guest app into an RV32IMC ELF (Windows PowerShell).
#
# Self-contained: needs ONLY a RISC-V cross compiler. Everything it links
# is vendored in this directory.
#
#   .\build.ps1                              -> app.elf
#   .\build.ps1 -Out foo.elf
#   .\build.ps1 -GuestCc riscv-none-elf-gcc
param([string]$Out = "app.elf", [string]$GuestCc = "")
$ErrorActionPreference = "Stop"
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path

$cc = $GuestCc
if (-not $cc) {
    foreach ($c in @("riscv64-unknown-elf-gcc","riscv-none-elf-gcc",
                     "riscv32-unknown-elf-gcc","riscv64-elf-gcc")) {
        if (Get-Command $c -ErrorAction SilentlyContinue) { $cc = $c; break }
    }
}
if (-not $cc) { Write-Error "no RISC-V cross found; pass -GuestCc <compiler>"; exit 1 }

# Opt-in tools: module paths under src\ (no .c), e.g.
#   $Modules = @("containers/ring_buffer","math/fixed_point")
# (mind inter-deps: fifo_queue needs ring_buffer, etc.)
$Modules = @()

$srcs = @("$Here\main.c", "$Here\src\vm_runtime.c", "$Here\src\tui.c")
foreach ($m in $Modules) { $srcs += "$Here\src\$($m -replace '/','\').c" }

$cargs = @("-march=rv32imc","-mabi=ilp32","-nostdlib","-nostartfiles",
           "-ffreestanding","-Os","-ffunction-sections","-fdata-sections",
           "-I$Here\include",
           # -Wl,-s strips symbols (~0.5 KB smaller); drop it for debug symbols.
           "-Wl,--gc-sections","-Wl,-z,max-page-size=4","-Wl,-s","-Wl,-T,$Here\guest.ld",
           "-o",$Out) + $srcs
& $cc @cargs
if ($LASTEXITCODE -ne 0) { Write-Error "guest compile failed" }
Write-Host "build.ps1: built $Out  (cross: $cc)"
