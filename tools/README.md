# FMV tooling

Host-side encoder for the SNES 4bpp FMV engine. Turns ordinary video into a
single self-contained `.fmv` clip — video **and** audio interleaved per frame —
that the coprocessor streams off SD and demuxes to the PPU and the mixer.
Playback runtime is `src/video/tests/demo_fmv.c`.

## Format

- **Video:** 240×208, 4bpp tiles, **8 palettes × 15 colors + 1 shared black
  backdrop** per frame (re-chosen per frame), **20 fps**, uncompressed.
  Each video block is 26,776 B: `CGRAM 256 | tilemap 1560 | CHR 24960`.
- **Audio:** raw `s16le`, **44100 Hz, stereo** (matches the mixer), interleaved
  one chunk per video frame: 44100 / 20 = 2205 sample-frames = **8820 B/frame**
  (exact, since the rate divides evenly by the fps).
- **DMA fit:** 208 active lines letterboxes the display to a 54-line vblank
  window (16 forced-blank + 38 vblank, joypad auto-read off) = 9207 B/vblank,
  so a video block fits in 3 vblanks → 20 fps.
- **`.fmv` (FMV2)** = 32-B header `"FMV2", u16 w,h,fps,channels, u32 nframes,
  u32 rate, u16 bits, u16 _, u32 audio_bytes_per_frame, u32 _`, then `nframes`
  interleaved units of `[ audio 8820 B | video block 26776 B ]` — audio first
  so the real-time mixer leads when streamed; one sequential read per frame
  demuxes to the mixer and the PPU. (Legacy video-only `FMV1` still plays, with
  an optional sidecar `.pcm`.)

## Encode (one command)

`encode_fmv.sh` (bash) and `encode_fmv.ps1` (Windows PowerShell) both **build
the tools** — the encoder and the host player — and then encode. The audio is
extracted to a temp `.pcm`, muxed into the clip, and deleted, so the output is
one self-contained `.fmv`. ffmpeg is piped straight into the encoder (no
multi-GB raw-video temp), so the whole movie is fine; the `.fmv` just gets big
(~415 MB for full BBB with audio; SD is cheap). Only ffmpeg + a C compiler
(MSYS2 mingw gcc on Windows) are required.

```sh
# bash (MSYS2 / Linux):
tools/encode_fmv.sh movie.mp4              # whole video  -> movie.fmv
tools/encode_fmv.sh movie.mp4 -t 6         # first 6 seconds
tools/encode_fmv.sh movie.mp4 -o intro     # -> intro.fmv
```
```powershell
# Windows PowerShell (auto-adds mingw gcc, runs the binary pipe through cmd,
# sets a space-free TMP for gcc):
.\tools\encode_fmv.ps1 movie.mp4
.\tools\encode_fmv.ps1 movie.mp4 -Seconds 6 -Out intro
```

Outputs land in the current directory; the scripts also build `build/demo_fmv`
so you can play it right away — one file, audio included:

```sh
./build/demo_fmv intro.fmv
```

In the player: **I** info overlay · **V** vsync · **F** filter · **F11**
fullscreen · **Esc** quit. (Audio is the master clock — video follows it.)

## Manual / by hand

```sh
gcc -Wall -O2 -o tools/fmv_encode tools/fmv_encode.c

# single still -> fmv_frame.bin + fmv_preview.ppm (eyeball the quantization):
ffmpeg -i in.mp4 -frames:v 1 -vf scale=240:208 -f rawvideo -pix_fmt rgb24 f.rgb
tools/fmv_encode f.rgb

# a clip + its audio, muxed into one .fmv (RGB24 = 240*208*3 per video frame,
# audio = s16le 44100 stereo):
ffmpeg -i in.mp4 -t 6 -vf scale=240:208,fps=20 -f rawvideo -pix_fmt rgb24 c.rgb
ffmpeg -i in.mp4 -t 6 -vn -ar 44100 -ac 2 -f s16le c.pcm
tools/fmv_encode c.rgb out.fmv c.pcm    # video "-" reads stdin; omit c.pcm (or
                                        # pass "none") for silent; -n N caps frames

# a synthetic test clip, no source (silent audio):
tools/fmv_encode synth 40 synth.fmv
```

## Notes

- 16:9 sources get squished into 240×208; pre-letterbox in ffmpeg if you want
  correct aspect (`scale=240:135,pad=240:208:0:36`).
- `fmv_encode` and the `.fmv`/`.rgb`/`.pcm`/`.mov` artifacts are gitignored.
- Quality is v1 (k-means tile grouping + median-cut + ordered dither). The
  upgrade is iterative per-tile palette re-assignment.
