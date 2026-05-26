<#
  encode_fmv.ps1 — build the FMV tools and encode a video into a .fmv clip
  (+ .pcm audio) for the microgarbage FMV engine, on Windows PowerShell.

    .\tools\encode_fmv.ps1 movie.mp4               # whole video
    .\tools\encode_fmv.ps1 movie.mp4 -Seconds 6    # first 6 seconds
    .\tools\encode_fmv.ps1 movie.mp4 -Out intro    # -> intro.fmv / intro.pcm

  Needs ffmpeg on PATH and the MSYS2 mingw64 gcc (auto-added from
  C:\msys64\mingw64\bin if not already on PATH). Outputs land in the current
  directory. W/H/FPS must match fmv_encode.c; RATE must match demo_fmv.c.
#>
param(
  [Parameter(Mandatory = $true, Position = 0)][string]$Video,
  [int]$Seconds = 0,
  [string]$Out = ""
)
$ErrorActionPreference = "Stop"
$W = 240; $H = 208; $FPS = 20; $RATE = 44100

$here = Split-Path -Parent $MyInvocation.MyCommand.Path   # tools\
$repo = Split-Path -Parent $here
$src  = Join-Path $here "fmv_encode.c"

# --- toolchain: gcc (mingw) + ffmpeg ---
if (-not (Get-Command gcc -ErrorAction SilentlyContinue)) {
  $mingw = "C:\msys64\mingw64\bin"
  if (Test-Path (Join-Path $mingw "gcc.exe")) { $env:PATH = "$mingw;C:\msys64\usr\bin;$env:PATH" }
  else { throw "gcc not found. Install MSYS2 (mingw64) or add gcc to PATH." }
}
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) { throw "ffmpeg not found on PATH." }
if (-not (Test-Path $Video)) { throw "no such file: $Video" }
if ($Out -eq "") { $Out = [IO.Path]::GetFileNameWithoutExtension($Video) }

# mingw gcc needs a space-free TMP (the default %TEMP% has the username,
# which contains a space here and breaks gcc's intermediate files).
$tmp = Join-Path $env:SystemDrive "fmv_tmp"
New-Item -ItemType Directory -Force $tmp | Out-Null
$env:TMP = $tmp; $env:TEMP = $tmp; $env:TMPDIR = $tmp

function Stale($out, $srcfile) { return -not (Test-Path $out) -or (Get-Item $srcfile).LastWriteTime -gt (Get-Item $out).LastWriteTime }

# --- build the encoder (required) ---
$enc = Join-Path $here "fmv_encode.exe"
if (Stale $enc $src) { Write-Host "building fmv_encode ..."; gcc -Wall -O2 -o $enc $src }

# --- build the host player (for preview) ---
$build  = Join-Path $repo "build"; New-Item -ItemType Directory -Force $build | Out-Null
$player = Join-Path $build "demo_fmv.exe"
$psrc   = Join-Path $repo "src\video\tests\demo_fmv.c"
if (Stale $player $psrc) {
  Write-Host "building demo_fmv (host player) ..."
  gcc -Wall -Wextra -std=c11 "-I$repo\include" -o $player `
      "$repo\src\video\ppu.c" "$repo\src\video\present_gl_win32.c" $psrc `
      -lopengl32 -lgdi32 -luser32 -lwinmm
}

$t = if ($Seconds -gt 0) { "-t $Seconds " } else { "" }

# PowerShell pipes corrupt binary, so run the ffmpeg|encoder pipe through cmd.
Write-Host "video -> $Out.fmv  (${W}x${H}, $FPS fps)"
cmd /c "ffmpeg -hide_banner -loglevel error -i `"$Video`" $t-vf scale=${W}:${H},fps=$FPS -f rawvideo -pix_fmt rgb24 - | `"$enc`" - `"$Out.fmv`""

Write-Host "audio -> $Out.pcm  ($RATE Hz s16 stereo)"
cmd /c "ffmpeg -hide_banner -loglevel error -y -i `"$Video`" $t-vn -ar $RATE -ac 2 -f s16le `"$Out.pcm`""

Write-Host ""
Write-Host "done:  $Out.fmv  +  $Out.pcm"
Write-Host "play:  .\build\demo_fmv.exe $Out.fmv $Out.pcm"
