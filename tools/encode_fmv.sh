#!/usr/bin/env bash
# encode_fmv.sh — build the FMV tools and encode a video into a single
# self-contained .fmv clip (audio muxed in) for the microgarbage FMV engine.
# ffmpeg is piped straight into fmv_encode, so there is no giant intermediate
# .rgb and encoding the whole video is fine.
#
#   tools/encode_fmv.sh movie.mp4            # whole video
#   tools/encode_fmv.sh movie.mp4 -t 6       # first 6 seconds
#   tools/encode_fmv.sh movie.mp4 -o intro   # -> intro.fmv
#
# Works in the repo (builds fmv_encode + demo_fmv from source via gcc) or as a
# standalone demo package: drop fmv_encode(.exe), demo_fmv(.exe) and ffmpeg(.exe)
# next to this script and it uses them as-is (no gcc needed). ffmpeg comes from
# PATH or from next to this script. Outputs land in the current directory;
# W/H/FPS must match fmv_encode.c and RATE must match demo_fmv.c.
set -euo pipefail
W=240; H=208; FPS=20; RATE=44100

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # tools/
repo="$(cd "$here/.." && pwd)"
src="$here/fmv_encode.c"

usage() { echo "usage: $0 INPUT [-t SECONDS] [-o OUTBASE]"; exit 1; }
[ $# -ge 1 ] || usage
in="$1"; shift
dur=""; out=""
while [ $# -gt 0 ]; do
  case "$1" in
    -t) dur="${2:-}"; shift 2 ;;
    -o) out="${2:-}"; shift 2 ;;
    -h|--help) usage ;;
    *) echo "unknown option: $1"; usage ;;
  esac
done
[ -f "$in" ] || { echo "no such file: $in"; exit 1; }
[ -n "$out" ] || out="$(basename "${in%.*}")"
# ffmpeg: from PATH, or bundled next to this script (demo package)
if ! command -v ffmpeg >/dev/null 2>&1; then
  if [ -x "$here/ffmpeg" ] || [ -x "$here/ffmpeg.exe" ]; then PATH="$here:$PATH"
  else echo "ffmpeg not found on PATH or next to this script ($here)"; exit 1; fi
fi
have_gcc() { command -v gcc >/dev/null 2>&1; }

# --- encoder: build from source if present (+gcc), else use the prebuilt exe ---
enc="$here/fmv_encode"; [ -x "$enc" ] || enc="$here/fmv_encode.exe"
if [ -f "$src" ] && have_gcc && { [ ! -x "$enc" ] || [ "$src" -nt "$enc" ]; }; then
  echo "building fmv_encode ..."
  gcc -Wall -O2 -o "$here/fmv_encode" "$src"
  enc="$here/fmv_encode"; [ -x "$enc" ] || enc="$here/fmv_encode.exe"
fi
[ -x "$enc" ] || { echo "fmv_encode(.exe) not found in $here (and no source+gcc to build it)"; exit 1; }

# --- player (optional, for preview): build from repo source, else find prebuilt ---
player=""
psrc="$repo/src/video/tests/demo_fmv.c"
if [ -f "$psrc" ] && have_gcc; then
  player="$repo/build/demo_fmv"; [ -x "$player" ] || player="$repo/build/demo_fmv.exe"
  if [ ! -x "$player" ] || [ "$psrc" -nt "$player" ]; then
    echo "building demo_fmv (host player) ..."
    mkdir -p "$repo/build"
    gcc -Wall -Wextra -std=c11 -I"$repo/include" -o "$repo/build/demo_fmv" \
        "$repo/src/video/ppu.c" "$repo/src/video/present_gl_win32.c" "$psrc" \
        -lopengl32 -lgdi32 -luser32 -lwinmm 2>/dev/null \
      || echo "  (player build skipped — needs Windows/mingw + OpenGL; the .fmv still encodes)"
    player="$repo/build/demo_fmv"; [ -x "$player" ] || player="$repo/build/demo_fmv.exe"
  fi
else
  for cand in "$here/demo_fmv" "$here/demo_fmv.exe" "$repo/build/demo_fmv" "$repo/build/demo_fmv.exe"; do
    [ -x "$cand" ] && { player="$cand"; break; }
  done
fi

tflag=(); [ -n "$dur" ] && tflag=(-t "$dur")

# 1) extract the audio track to a temp raw-PCM file. It gets muxed into the
#    .fmv below, then deleted — the clip is a single self-contained file.
tmp_pcm="$out.tmp.pcm"
echo "audio -> (temp)  (${RATE} Hz s16 stereo)"
if ! ffmpeg -hide_banner -loglevel error -y -i "$in" ${tflag[@]+"${tflag[@]}"} \
       -vn -ar "$RATE" -ac 2 -f s16le "$tmp_pcm" 2>/dev/null; then
  echo "  (no audio track — encoding silent)"; rm -f "$tmp_pcm"; tmp_pcm="none"
fi

# 2) pipe video frames into the encoder; it interleaves one audio chunk per
#    frame (audio first) into the FMV2 container. ffmpeg piped straight in, so
#    no giant intermediate .rgb — the whole movie is fine.
echo "video+audio -> $out.fmv  (${W}x${H}, ${FPS} fps, muxed)"
ffmpeg -hide_banner -loglevel error -i "$in" ${tflag[@]+"${tflag[@]}"} \
  -vf "scale=${W}:${H},fps=${FPS}" -f rawvideo -pix_fmt rgb24 - \
  | "$enc" - "$out.fmv" "$tmp_pcm"

[ "$tmp_pcm" != "none" ] && rm -f "$tmp_pcm"

echo
echo "done:  $out.fmv  (audio muxed in)"
if [ -n "$player" ] && [ -x "$player" ]; then echo "play:  $player $out.fmv"
else echo "play:  demo_fmv $out.fmv   (copy the demo_fmv player next to this script to enable playback)"; fi
