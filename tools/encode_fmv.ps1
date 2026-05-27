<#
  encode_fmv.ps1 — build the FMV tools and encode a video into a single
  self-contained .fmv clip (audio muxed in) for the microgarbage FMV engine,
  on Windows PowerShell.

    .\tools\encode_fmv.ps1 movie.mp4               # whole video
    .\tools\encode_fmv.ps1 movie.mp4 -Seconds 6    # first 6 seconds
    .\tools\encode_fmv.ps1 movie.mp4 -Out intro    # -> intro.fmv

  Works two ways:
    - in the repo: builds fmv_encode + demo_fmv from source (needs the MSYS2
      mingw64 gcc, auto-added from C:\msys64\mingw64\bin).
    - standalone demo package: drop fmv_encode.exe, demo_fmv.exe, and
      ffmpeg.exe next to this script and it uses them as-is (no gcc needed).
  ffmpeg comes from PATH, or from ffmpeg.exe next to this script. Outputs land
  in the current directory. W/H/FPS must match fmv_encode.c; RATE must match
  demo_fmv.c.
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

# --- ffmpeg: from PATH, or bundled next to this script (demo package) ---
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) {
  if (Test-Path (Join-Path $here "ffmpeg.exe")) { $env:PATH = "$here;$env:PATH" }
  else { throw "ffmpeg not found on PATH or next to this script ($here). Put ffmpeg.exe there or add it to PATH." }
}
if (-not (Test-Path $Video)) { throw "no such file: $Video" }
if ($Out -eq "") { $Out = [IO.Path]::GetFileNameWithoutExtension($Video) }

# gcc is only needed when building from source; add MSYS2 mingw64 on demand.
function Need-Gcc {
  if (Get-Command gcc -ErrorAction SilentlyContinue) { return $true }
  $mingw = "C:\msys64\mingw64\bin"
  if (Test-Path (Join-Path $mingw "gcc.exe")) { $env:PATH = "$mingw;C:\msys64\usr\bin;$env:PATH"; return $true }
  return $false
}
# mingw gcc needs a space-free TMP (the default %TEMP% has the username, which
# contains a space here and breaks gcc's intermediates). Only matters building.
function Use-Tmp { $t = Join-Path $env:SystemDrive "fmv_tmp"; New-Item -ItemType Directory -Force $t | Out-Null; $env:TMP=$t; $env:TEMP=$t; $env:TMPDIR=$t }
function Stale($out, $srcfile) { return -not (Test-Path $out) -or (Get-Item $srcfile).LastWriteTime -gt (Get-Item $out).LastWriteTime }

# --- encoder: build from source if present (+gcc), else use the prebuilt exe ---
$enc = Join-Path $here "fmv_encode.exe"
if ((Test-Path $src) -and (Stale $enc $src) -and (Need-Gcc)) {
  Use-Tmp; Write-Host "building fmv_encode ..."; gcc -Wall -O2 -o $enc $src
}
if (-not (Test-Path $enc)) { throw "fmv_encode.exe not found in $here (and no source+gcc to build it)." }

# --- player (optional, for preview): build from repo source, else find a prebuilt exe ---
$player = $null
$psrc   = Join-Path $repo "src\video\tests\demo_fmv.c"
if ((Test-Path $psrc) -and (Need-Gcc)) {
  $build = Join-Path $repo "build"; New-Item -ItemType Directory -Force $build | Out-Null
  $player = Join-Path $build "demo_fmv.exe"
  if (Stale $player $psrc) {
    Use-Tmp; Write-Host "building demo_fmv (host player) ..."
    gcc -Wall -Wextra -std=c11 "-I$repo\include" -o $player `
        "$repo\src\video\ppu.c" "$repo\src\video\present_gl_win32.c" $psrc `
        -lopengl32 -lgdi32 -luser32 -lwinmm
  }
} else {
  foreach ($cand in @((Join-Path $here "demo_fmv.exe"), (Join-Path $repo "build\demo_fmv.exe"))) {
    if (Test-Path $cand) { $player = $cand; break }
  }
}

$t = if ($Seconds -gt 0) { "-t $Seconds " } else { "" }

# We need cmd.exe for the binary ffmpeg|encoder pipe (PowerShell's pipeline
# corrupts binary streams). Invoke it by full path: a bare 'cmd' that PATH
# can't resolve pops Windows' "select an app to open cmd" dialog.
$comspec = if ($env:ComSpec) { $env:ComSpec } else { Join-Path $env:SystemRoot "system32\cmd.exe" }

# 1) extract the audio track to a temp raw-PCM file. It gets muxed into the
#    .fmv below, then deleted — the clip is a single self-contained file.
$tmpPcm = "$Out.tmp.pcm"
Write-Host "audio -> (temp)  ($RATE Hz s16 stereo)"
& $comspec /c "ffmpeg -hide_banner -loglevel error -y -i `"$Video`" $t-vn -ar $RATE -ac 2 -f s16le `"$tmpPcm`""
$audioArg = if (Test-Path $tmpPcm) { $tmpPcm } else { Write-Host "  (no audio track - encoding silent)"; "none" }

# 2) pipe video frames into the encoder; it interleaves one audio chunk per
#    frame (audio first) into the FMV2 container.
Write-Host "video+audio -> $Out.fmv  (${W}x${H}, $FPS fps, muxed)"
& $comspec /c "ffmpeg -hide_banner -loglevel error -i `"$Video`" $t-vf scale=${W}:${H},fps=$FPS -f rawvideo -pix_fmt rgb24 - | `"$enc`" - `"$Out.fmv`" `"$audioArg`""

if (Test-Path $tmpPcm) { Remove-Item $tmpPcm }

Write-Host ""
Write-Host "done:  $Out.fmv  (audio muxed in)"
if ($player) { Write-Host "play:  $player $Out.fmv" }
else { Write-Host "play:  demo_fmv.exe $Out.fmv   (copy demo_fmv.exe next to this script to enable playback)" }
