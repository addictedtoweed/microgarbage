#!/usr/bin/env bash
# encode_fmv.sh — turn a video into a .fmv clip (+ .pcm audio) for the
# microgarbage FMV engine. ffmpeg is piped straight into fmv_encode, so there
# is no giant intermediate .rgb and encoding the whole video is fine.
#
#   tools/encode_fmv.sh movie.mp4            # whole video
#   tools/encode_fmv.sh movie.mp4 -t 6       # first 6 seconds
#   tools/encode_fmv.sh movie.mp4 -o intro   # output intro.fmv / intro.pcm
#
# Outputs (in the current directory) OUT.fmv (240x208, 4bpp, 8 palettes/frame,
# 20fps) and OUT.pcm (s16le, 44100 Hz, stereo). Play both with demo_fmv:
#   ./build/demo_fmv OUT.fmv OUT.pcm
#
# These must match fmv_encode.c (W/H/FPS) and demo_fmv.c (RATE).
set -euo pipefail
W=240; H=208; FPS=20; RATE=44100

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"   # tools/
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
command -v ffmpeg >/dev/null 2>&1 || { echo "ffmpeg not found on PATH"; exit 1; }

# build the encoder if missing or out of date (mingw may append .exe)
enc="$here/fmv_encode"
[ -x "$enc" ] || enc="$here/fmv_encode.exe"
if [ ! -x "$enc" ] || [ "$src" -nt "$enc" ]; then
  echo "building fmv_encode ..."
  gcc -Wall -O2 -o "$here/fmv_encode" "$src"
  enc="$here/fmv_encode"; [ -x "$enc" ] || enc="$here/fmv_encode.exe"
fi

tflag=(); [ -n "$dur" ] && tflag=(-t "$dur")

echo "video -> $out.fmv  (${W}x${H}, ${FPS} fps)"
ffmpeg -hide_banner -loglevel error -i "$in" ${tflag[@]+"${tflag[@]}"} \
  -vf "scale=${W}:${H},fps=${FPS}" -f rawvideo -pix_fmt rgb24 - \
  | "$enc" - "$out.fmv"

echo "audio -> $out.pcm  (${RATE} Hz s16 stereo)"
ffmpeg -hide_banner -loglevel error -y -i "$in" ${tflag[@]+"${tflag[@]}"} \
  -vn -ar "$RATE" -ac 2 -f s16le "$out.pcm"

echo
echo "done:  $out.fmv  +  $out.pcm"
echo "play:  ./build/demo_fmv $out.fmv $out.pcm"
