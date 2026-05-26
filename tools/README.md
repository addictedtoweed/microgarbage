# FMV tooling

Host-side encoder for the SNES 4bpp FMV engine. Turns ordinary video into a
`.fmv` clip the coprocessor streams to the PPU, plus a matching `.pcm` audio
track. Playback runtime is `src/video/tests/demo_fmv.c`.

## Format

- **Video:** 240×208, 4bpp tiles, **8 palettes × 15 colors + 1 shared black
  backdrop** per frame (re-chosen per frame), **20 fps**, uncompressed.
  Each frame is 26,776 B: `CGRAM 256 | tilemap 1560 | CHR 24960`.
- **Audio:** raw `s16le`, **44100 Hz, stereo** (matches the mixer).
- **DMA fit:** 208 active lines letterboxes the display to a 54-line vblank
  window (16 forced-blank + 38 vblank, joypad auto-read off) = 9207 B/vblank,
  so a frame fits in 3 vblanks → 20 fps.
- `.fmv` = `"FMV1", u16 w, u16 h, u16 fps, u16 reserved, u32 nframes`, then
  `nframes` frame blocks.

## Encode (one command)

`encode_fmv.sh` (bash) and `encode_fmv.ps1` (Windows PowerShell) both **build
the tools** — the encoder and the host player — and then encode. ffmpeg is
piped straight into the encoder (no multi-GB temp), so the whole movie is fine;
the `.fmv` just gets big (~320 MB for full BBB; SD is cheap). Only ffmpeg + a C
compiler (MSYS2 mingw gcc on Windows) are required.

```sh
# bash (MSYS2 / Linux):
tools/encode_fmv.sh movie.mp4              # whole video  -> movie.fmv + movie.pcm
tools/encode_fmv.sh movie.mp4 -t 6         # first 6 seconds
tools/encode_fmv.sh movie.mp4 -o intro     # -> intro.fmv + intro.pcm
```
```powershell
# Windows PowerShell (auto-adds mingw gcc, runs the binary pipe through cmd,
# sets a space-free TMP for gcc):
.\tools\encode_fmv.ps1 movie.mp4
.\tools\encode_fmv.ps1 movie.mp4 -Seconds 6 -Out intro
```

Outputs land in the current directory; the scripts also build `build/demo_fmv`
so you can play it right away:

```sh
./build/demo_fmv intro.fmv intro.pcm
```

In the player: **I** info overlay · **V** vsync · **F** filter · **F11**
fullscreen · **Esc** quit. (Audio is the master clock — video follows it.)

## Manual / by hand

```sh
gcc -Wall -O2 -o tools/fmv_encode tools/fmv_encode.c

# single still -> fmv_frame.bin + fmv_preview.ppm (eyeball the quantization):
ffmpeg -i in.mp4 -frames:v 1 -vf scale=240:208 -f rawvideo -pix_fmt rgb24 f.rgb
tools/fmv_encode f.rgb

# a clip from a raw RGB24 stream (240*208*3 per frame):
ffmpeg -i in.mp4 -t 6 -vf scale=240:208,fps=20 -f rawvideo -pix_fmt rgb24 c.rgb
tools/fmv_encode c.rgb out.fmv          # or:  tools/fmv_encode - out.fmv  (stdin)

# a synthetic test clip, no source:
tools/fmv_encode synth 40 synth.fmv

# audio track to go with a clip:
ffmpeg -i in.mp4 -t 6 -vn -ar 44100 -ac 2 -f s16le out.pcm
```

## Notes

- 16:9 sources get squished into 240×208; pre-letterbox in ffmpeg if you want
  correct aspect (`scale=240:135,pad=240:208:0:36`).
- `fmv_encode` and the `.fmv`/`.rgb`/`.pcm`/`.mov` artifacts are gitignored.
- Quality is v1 (k-means tile grouping + median-cut + ordered dither). The
  upgrade is iterative per-tile palette re-assignment.
