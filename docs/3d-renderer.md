# Variable-framerate 3D renderer — design

A SNES-side software 3D renderer that draws into a **dual-layer**
framebuffer (4bpp BG1 + 2bpp BG2 composited by color math for a fixed
~60-colour palette), at **240×200**, and streams it to VRAM through the
existing virtual-NMI cycle-budgeted DMA chainer. Framerate is **dynamic**:
the renderer spends as many hardware vblanks per displayed frame as the
scene's detail demands, so simple frames run fast and dense frames degrade
gracefully instead of tearing.

Audience: whoever picks this up to implement it. This doc locks the
bandwidth math and the rolling-buffer timing; the geometry/rasteriser
reuses [[r3d-renderer-and-fmv]].

## What already exists (don't redo)

- **`src/video/r3d.c`** — fixed-point Q16.16 polygon engine (transform →
  project → flat-shade → z-buffer → edge raster into an 8bpp framebuffer;
  `r3d_render_dither` is the ordered-dither variant). The 3D *math* is done;
  this project changes the **output stage** (dual-layer tile encode) and the
  **transport** (rolling subframe buffers), not the rasteriser.
- **Virtual-NMI kernel** ([[virtual-nmi-kernel]]) — NMI-off, timer-IRQ driven,
  with a runtime cycle-budgeted DMA chainer (`calc_bytes_rem`, live OPVCT
  check, defer/resume, FRAME_DONE strobe). The renderer is a new producer
  feeding this unchanged chainer.
- **Cart-window / PpuBatch staging** — `copro_mg_state.c` pack/emit helpers
  (extracted in #69), `cart_window_*` setters, the FRAME_DONE consumer hook.
- **DMA rate calibration** — `snes/dma_rate_test.s` (#75), the standalone ROM
  that measures the real per-line GP-DMA rate during blank.

## The core idea: only the top band is just-in-time

VRAM is writable only during blank (vblank or a force-blanked line). A full
dual-layer 240×200 frame is far too big to push in one frame's blank, so the
frame is split into **5 horizontal bands** ("subframes") of 40 lines each, and
the renderer holds **5 rolling subframe buffers**. A displayed frame spans
multiple hardware (60 Hz) frames — that's where the dynamic framerate comes
from — and each band is DMA'd to VRAM in a *different* one of those vblanks.

The decisive constraint, confirmed with the user: **only the top band must be
transferred just-in-time.** It displays first (at the top of the playfield)
and longest, so it is written *last*, in the **final vblank** before the frame
goes live. The bottom 4 bands were already made resident in the earlier
vblanks of the frame's display span, at a relaxed pace. So the tight
real-time question reduces to a single band per final NMI.

```
 displayed frame N+1 spans K hardware vblanks:
   vblank 0 .. K-2 : fill bottom bands 4,3,2,1 (relaxed, plenty of slack)
   vblank K-1      : fill TOP band  <-- the only just-in-time transfer
   then frame N+1 displays
```

## Band geometry & per-band cost

- 240×200, 5 bands → **40 lines/band = 5 tile rows × 30 tiles = 150 tiles/band**.
- Dual-layer, **one shared tilemap entry drives both layers** (tile N =
  4bpp-CHR[N] on BG1 *and* 2bpp-CHR[N] on BG2):
  - 4bpp CHR: 32 B/tile
  - 2bpp CHR: 16 B/tile
  - shared tilemap entry: 2 B/tile
  - **= 50 B per unique tile**
- Top band cost:
  - all-unique (worst case, dense detail/no fills): 150 × 50 = **7,500 B**
  - ~50 % repeat-tile fills (typical polygon interiors): **~3,900 B**
  - heavy solid fill: **~2,700 B**

Repeat tiles are the key lever: a solid-colour polygon interior is one CHR
pair reused across many tilemap cells, so bandwidth scales with **edge/detail
tiles, not screen area**.

## Bandwidth: does the top band fit the final NMI?

GP-DMA bus rate is **170 B/line** (1364 master cyc/line ÷ 8 cyc/byte) — the
hard ceiling. bsnes-accuracy charges DRAM-refresh stalls ares ignores, so the
real rate is **~163 B/line**; the FMV path's old 138 was conservative *packing*
across many small slots, never a rate cap. The top band is **3 contiguous
DMAs** (4bpp-CHR region, 2bpp-CHR region, tilemap), so per-slot overhead is
sub-scanline and the burst lands near the full rate.

Final-NMI window = vblank (38 lines) + top letterbox; the bottom letterbox is
available too if it wasn't already spent on a lower band:

| window | @150 | @163 (bsnes-acc) | @170 (ceiling) |
|---|---|---|---|
| 50 lines (vblank + 12 top_lb) | 7,500 | 8,150 | 8,500 |
| 62 lines (+ 12 bottom_lb)     | 9,300 | 10,106 | 10,540 |

Top band needs only **7,500 B worst case** — and just **121 B/line** averaged
over the 62-line window. So:

- **With any tile reuse (~3,900 B): trivial** at every rate/window.
- **All-unique top band (7,500 B):** fits the full 62-line window even at the
  degraded 163 rate (10,106 B, ~2.6 KB margin). In the minimal 50-line window
  it needs ≥150 B/line — which both emulators clear.

**Verdict: the top band fits the final NMI with margin in every realistic
case.** The only failure mode is an all-unique band crammed into a thin
50-line window at a degraded rate — avoided by either keeping the bottom
letterbox free (62-line window) or the encoder guaranteeing ≥1 repeat tile.

### Encoder budget rule

Cap unique tiles per band at `window_bytes / 50`. For the safe design point
(62-line window, conservative 150 B/line → 9,300 B): **≤186 unique tiles/band**
— above the 150 tiles a band even contains, i.e. *no* band can overflow. The
rule only bites if a future higher-res band or a thinner window is chosen.

## VRAM is single-buffered (no flip)

Dual-layer 240×200 is **~38 KB resident** (24 KB 4bpp CHR + 12 KB 2bpp CHR +
~2 KB tilemap). Two copies = 76 KB > the 64 KB VRAM, so there is **no
double-buffer / BG-base flip**. VRAM holds one frame and is updated **in place**,
band by band, during blank. For slow content this is tear-free; only fast motion
shows a sub-pixel shear at band seams (acceptable, and bounded by the framerate
floor below). This also means the "top band just-in-time" timing from the
bandwidth section relaxes — with no flip there's no go-live instant; bands just
refresh round-robin. The band-fit math still bounds how many bands a vblank moves.

## VRAM layout (240×200 dual-layer)

- **BG1 4bpp CHR**: up to 5×150 = 750 unique tiles worst case across all bands,
  but the working set per displayed frame is what's resident. Budget the CHR
  region for the rolling working set, not 5 full unique bands.
- **BG2 2bpp CHR**: parallel region, same tile indices.
- **One shared tilemap** (32×32 BG, 240 px = 30 cols used), driving both layers
  via BG1SC/BG2SC pointing at the same map data is *not* possible (separate
  base regs) — instead emit the tilemap once and DMA it to both BG bases, or
  share by pointing BG2SC at the same address (allowed; both layers read the
  same map). Confirm against [[bg1sc-encoding-gotcha]] (base is in 1024-word
  units).
- Color math: BG1 (4bpp) as main, BG2 (2bpp) as subscreen, add/half per the
  ~60-colour scheme. Window/color-math regs need PpuBatch plumbing like TM/TS
  ([[ppu-baseline-registers]]).

## Rolling-buffer state machine

**Worst-case floor = 15 fps: a full 5-band frame refresh spread over ≤4 hardware
vblanks.** A vblank moves ~10,000 B, so it carries **two** fill-heavy bands when
needed — that's how 5 bands fit 4 vblanks rather than 5. Faster (simpler) frames
use fewer vblanks → up to 60 fps. The framerate controller picks K vblanks from
the frame's total encoded bytes.

| | bands/vblank | vblanks/frame (K) | displayed fps |
|---|---|---|---|
| floor (worst) | ~1.25 | 4 | **15** |
| typical | ~2.5 | 2 | 30 |
| simple | 5 | 1 | 60 |

Staging: a **PSRAM ping-pong (depth ≥2)** of the encoded tile-set, so the
renderer builds frame N+1 while the chainer is still draining frame N. PSRAM is
MB-scale (~38 KB/set), so depth is free — bump it for more render lookahead.

**Reuse, don't rebuild:** the existing FMV sub-frame chainer already bursts a
~7.5 KB sub-frame per vblank and chains across NMIs on the cycle budget
([[virtual-nmi-kernel]]). Milestone 1 feeds the cube's encoded bands through that
same staging — the only new transport piece is the PSRAM ping-pong. Teardown must
clear staged slot descriptors across mode changes ([[staged-leak-across-demos]]).

## Build order (milestones)

The first proof-of-concept is a **slowly rotating cube that bounces around the
screen**. Because it's slow, no new bandwidth machinery is needed to see it —
build in increasing risk:

1. **M1 — cube on the existing single-layer 4bpp path.** r3d rotating+bouncing
   cube → dither to 4bpp tiles → the canyon4/FMV sub-frame transport. Proves
   geometry + bounce + multi-vblank transfer with zero new plumbing. New file:
   a guest `demo_cube.c`.
2. **M2 — "high color".** Add the 2bpp BG2 + color-math layer (TS sub-screen,
   CGADSUB/CGWSEL PpuBatch plumbing). Encode the cube into both CHR sets against
   the shared tilemap.
3. **M3 — dynamic framerate controller** + PSRAM ping-pong, if pushing past what
   the chainer gives for free.

## Open questions / next steps

- **Measure the real rate** (#75): run `dma_rate_{150..170}b_{1,3}slot.sfc`,
  record the green→red flip (1-slot = raw, 3-slot = real burst shape) on
  bsnes-accuracy. Fill the 163 estimate above with the measured number.
- **Shared-tilemap dual-base**: verify BG2SC can alias BG1's map base so one
  tilemap DMA feeds both layers (else pay a second tilemap DMA = +2 B/tile).
- **Color-math PpuBatch plumbing**: extend the encoder/kernel like TM/TS.
- **Encoder tile-dedup**: hash CHR pairs per band, emit unique CHR + tilemap
  indices, enforce the per-band unique-tile budget.
- **Framerate controller**: map per-frame total bytes → K vblanks on the
  cycle-budgeted chainer.
