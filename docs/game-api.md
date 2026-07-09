# Game API (cart-side `game.elf`) — design

This document captures the API surface a customer writes against when
building a SNES coprocessor game as a guest ELF on the microgarbage
runtime, plus the design decisions behind each piece. It is the
contract for the guest-side library at
`examples/common/guest/mg_*.{h,c}` and the host-side ECALL handlers
that back it.

Audience: someone implementing the headers and host handlers, or a
game developer writing against the API. Both should be able to read
this end-to-end.

## What this is, and what already exists

A `game.elf` is a RV32IMC guest running inside the microgarbage VM on
the coprocessor (M7 today, future targets later). It drives the SNES
via the cart bus by staging payloads into the 64KB cart window and
toggling a frame-ready byte — a contract the SNES-side kernel
([[snes-cart-kernel]]) already consumes through a 4KB boot blob and
runtime kernel handshake.

Foundation already in place (see [[mgapi-cart-runtime]]):

- **Cart bus seam**: `SYS_COPRO_STAGE_PAYLOAD / STAGE_DMA_SLOT /
  FRAME_COMMIT / READ_PADS / WAIT_VBLANK` at 1180–1184.
- **Audio**: full `SYS_AUDIO_*` surface at 1160–1170 (load WAV,
  trigger SFX, play / stream music, set gain, FFT meters).
- **Memory**: `SYS_ALLOC` / `SYS_FREE` for local heap;
  `SYS_L2_ALLOC / FREE / STATS` at 1185–1187 for cross-VM bulk.
- **Filesystem**: `/cart/` (trashfs RAM slice for assets) +
  `/host/` passthrough in dev.
- **Reset**: `SYS_COPRO_RESET_COUNT` (1188) with the existing
  CIC-line reset marshalling.

This document specifies the *game-facing wrappers* on top of those
primitives, plus the new ECALLs they need.

## API surface

One umbrella header drags in the per-concern subheaders:

```c
#include "mg_game.h"
/* -> mg_frame.h, mg_input.h, mg_sprite.h, mg_actor.h, mg_bg.h,
 *    mg_mode7.h, mg_hdma.h, mg_gfx.h, mg_audio.h, mg_panic.h */
```

Result codes for any DMA-bearing call:

```c
typedef enum {
    MG_OK              =  0,
    MG_ERR_DMA_SLOTS   = -1,   /* the 8-slot frame DMA list is full   */
    MG_ERR_DMA_BYTES   = -2,   /* would blow the byte budget          */
    MG_ERR_INVALID     = -3,
} MgResult;

#define MG_OR_PANIC(expr) \
    do { MgResult _r = (expr); \
         if (_r != MG_OK) mg_panic("DMA failed: " #expr); } while(0)
```

### Frame pacing (`mg_frame.h`)

```c
void mg_wait_frame(void);    /* block on runtime's producer; pads now fresh */
void mg_frame_commit(void);  /* hand staging tables back to runtime         */
uint8_t  mg_frame_slots_remaining(void);
uint16_t mg_frame_bytes_remaining(void);
void     mg_force_blank(uint8_t top, uint8_t bottom);
uint8_t  mg_force_blank_top(void);
uint8_t  mg_force_blank_bottom(void);
```

**Model**: `mg_frame_commit` hands the staging tables (the DMA
descriptor list + dirty shadow buffers) back to the runtime and
returns immediately; `mg_wait_frame` blocks until the SNES side has
consumed a frame, at which point pads are fresh and the staging
tables are clear. On the cart side the transfer is driven by the
**virtual-NMI kernel** ([[snes-cart-kernel]]) — NMI off, a single
self-chaining H+V timer IRQ that runs a cycle-budgeted DMA chainer
inside a **dynamic force-blank letterbox**: it reads the live beam,
streams as many bytes as the blank window can afford (~160 B/line,
measured safe on hardware), and defers the overflow to the next
frame. A payload too big for one window simply streams over several
frames. The design direction is for a game to compose its own
frame-shape-tailored transfer handler via the guest **NMI builder**
(`mg_nmi.h`) rather than lean on the generic kernel path — the
`mg_frame_*` contract here is unchanged either way.

**Force-blank**: trades visible scanlines for DMA budget. Each
blanked scanline the chainer reclaims yields ~160 bytes at the
measured-safe rate. `(0, 0)` is the 224-line visible baseline;
`(8, 8)` is the FMV path's 208 visible. Documented presets in the
header.

### Input (`mg_input.h`)

```c
typedef struct { uint16_t bits; /* MG_BTN_* mask */ } MgPad;
typedef struct { MgPad p0, p1, p2, p3; } MgPads;

enum {
    MG_BTN_B = 0x8000, MG_BTN_Y = 0x4000, MG_BTN_SELECT = 0x2000,
    MG_BTN_START = 0x1000, MG_BTN_UP = 0x0800, MG_BTN_DOWN = 0x0400,
    MG_BTN_LEFT = 0x0200, MG_BTN_RIGHT = 0x0100,
    MG_BTN_A = 0x0080, MG_BTN_X = 0x0040, MG_BTN_L = 0x0020, MG_BTN_R = 0x0010,
};

MgPads mg_pads(void);    /* latest producer-published snapshot, non-blocking */
bool   mg_pad_held    (MgPad p, uint16_t btn);
bool   mg_pad_pressed (MgPad p, uint16_t btn);  /* this-frame edge */
bool   mg_pad_released(MgPad p, uint16_t btn);
```

**Producer model**: the runtime polls joypads as part of its hot
path; guests read the published snapshot. Edge detection
(`pressed/released`) is library-side, per-VM: each guest keeps its
own last-frame pad state so game.elf and a spawned child.elf each
see independent edges without colliding.

### Sprites (`mg_sprite.h`)

```c
typedef struct {
    int16_t  x;                /* 9-bit signed, runtime packs sign bit  */
    uint8_t  y;                /* y >= 240 = offscreen convention       */
    uint16_t tile;             /* 9-bit (0..511); natural C alignment    */
    uint8_t  palette    : 3;
    uint8_t  priority   : 2;
    uint8_t  hflip      : 1;
    uint8_t  vflip      : 1;
    uint8_t  size_large : 1;
} MgSprite;

void mg_sprite_set      (uint8_t slot, const MgSprite *s);
void mg_sprite_get      (uint8_t slot, MgSprite *out);
void mg_sprite_move     (uint8_t slot, int16_t x, uint8_t y);
void mg_sprite_hide     (uint8_t slot);
void mg_sprites_clear_all(void);

typedef enum {
    MG_SPR_SIZES_8_16  = 0,    MG_SPR_SIZES_8_32  = 1,
    MG_SPR_SIZES_8_64  = 2,    MG_SPR_SIZES_16_32 = 3,
    MG_SPR_SIZES_16_64 = 4,    MG_SPR_SIZES_32_64 = 5,
    MG_SPR_SIZES_16x32_32x64 = 6,
    MG_SPR_SIZES_16x32_32x32 = 7,
} MgSpriteSizes;
void mg_sprite_sizes    (MgSpriteSizes pair);
void mg_sprite_chr_base (uint16_t base0_word, uint16_t base1_word);

/* Slot allocator — optional, cooperative. Direct slot access still
 * works. Auto-released on VM exit. */
int  mg_sprite_request_range(uint8_t count);
void mg_sprite_release_range(uint8_t first, uint8_t count);

/* Cooperative snapshot/restore for nested-program save/restore. */
void     mg_oam_snapshot(void *buf_544);
MgResult mg_oam_restore (const void *buf_544);
```

**Model**: all per-sprite writes go to a host-side shadow OAM with no
DMA cost. At `mg_frame_commit`, the runtime emits **one** DMA
descriptor covering the dirty range (low watermark to high
watermark). `mg_sprites_clear_all` sets shadow OAM to all-y=240
and resets watermarks — the cleared state IS the published state,
so subsequent set calls only mark their slots dirty.

`mg_sprite_get` reads the shadow; the actor system uses it to
"increment-by-velocity" without re-publishing the full struct.

**Slot ownership**: flat 128-slot space, shared across all VMs.
"Whoever writes the slot wins." The allocator is an advisory tool
for cooperating siblings; direct writes still work and are not
walled off (no per-call ownership check in the hot path).

### Actors (`mg_actor.h`)

Pure guest library on top of `mg_sprite_*`. No new ecalls.

```c
#ifndef MG_ACTOR_MAX_PARTS
#define MG_ACTOR_MAX_PARTS 8
#endif

typedef struct {
    int16_t  ox, oy;                /* offset from actor origin, px        */
    uint16_t tile;
    uint8_t  slot;                  /* OAM slot this part owns             */
    uint8_t  palette    : 3;
    uint8_t  priority   : 2;
    uint8_t  hflip      : 1;
    uint8_t  vflip      : 1;
    uint8_t  size_large : 1;
    uint8_t  visible    : 1;
} MgActorPart;

typedef struct {
    int32_t  x_q16, y_q16;          /* sub-pixel position, Q16.16          */
    int32_t  vx_q16, vy_q16;        /* velocity per frame, Q16.16          */
    uint8_t  part_count;
    bool     visible;
    bool     facing_left;           /* mirrors hflip + ox for all parts    */
    MgActorPart parts[MG_ACTOR_MAX_PARTS];
} MgActor;

void mg_actor_init    (MgActor *a);
bool mg_actor_add_part(MgActor *a, MgActorPart part);
void mg_actor_step    (MgActor *a);
void mg_actor_render  (const MgActor *a);
void mg_actor_set_pos (MgActor *a, int16_t x, int16_t y);
static inline int16_t mg_actor_x(const MgActor *a) { return (int16_t)(a->x_q16 >> 16); }
static inline int16_t mg_actor_y(const MgActor *a) { return (int16_t)(a->y_q16 >> 16); }
```

**Deliberately out of scope**: animation systems, collision boxes,
sprite-slot allocation, Z-order between actors. Each is too
opinionated to live in the core library and adds little code
saving for the developer building it themselves on top.

**Q16.16 truncation**: `>> 16`, not rounded. Predictable floor
behaviour matches what an integer-only system would naturally see.

### Background layers (`mg_bg.h`)

```c
typedef enum {
    MG_BG_MODE_0, MG_BG_MODE_1, MG_BG_MODE_2, MG_BG_MODE_3,
    MG_BG_MODE_4, MG_BG_MODE_5, MG_BG_MODE_6, MG_BG_MODE_7,
} MgBgMode;
typedef enum { MG_BG_LAYER_1, MG_BG_LAYER_2, MG_BG_LAYER_3, MG_BG_LAYER_4 } MgBgLayer;
typedef enum {
    MG_BG_SIZE_32x32, MG_BG_SIZE_64x32, MG_BG_SIZE_32x64, MG_BG_SIZE_64x64,
} MgBgSize;

typedef struct {
    uint16_t tile     : 10;
    uint16_t palette  : 3;
    uint16_t priority : 1;
    uint16_t hflip    : 1;
    uint16_t vflip    : 1;
} MgBgTile;                         /* matches SNES tilemap word layout */

void     mg_bg_mode  (MgBgMode mode);
void     mg_bg_setup (MgBgLayer layer, uint16_t tilemap_word,
                      MgBgSize size, uint16_t chr_word);
void     mg_bg_enable(MgBgLayer layer, bool main_screen, bool sub_screen);

void     mg_bg_set_tile(MgBgLayer layer, uint8_t x, uint8_t y, MgBgTile cell);
void     mg_bg_get_tile(MgBgLayer layer, uint8_t x, uint8_t y, MgBgTile *out);
void     mg_bg_blit   (MgBgLayer layer, uint8_t x, uint8_t y,
                       const MgBgTile *cells, uint16_t n);
MgResult mg_bg_upload (MgBgLayer layer, uint8_t x, uint8_t y,
                       const MgBgTile *cells, uint16_t n);

void     mg_bg_scroll (MgBgLayer layer, int16_t hx, int16_t vy);
void     mg_bg_mosaic (uint8_t size, uint8_t layer_mask);
void     mg_bg_main_priority(MgBgLayer layer, bool hi);
```

**Two write paths**:

- **Shadow path** (`mg_bg_set_tile`, `mg_bg_get_tile`, `mg_bg_blit`):
  writes to host-side shadow tilemap, no DMA cost. Runtime tracks
  dirty range per layer; commit emits one DMA per dirty range. Good
  for static scenes and HUDs.

- **Direct path** (`mg_bg_upload`): bypasses shadow, queues one DMA
  slot, costs `n*2` bytes against budget. Caller responsible for
  keeping shadow in sync if it matters. Good for stream-in scroll
  worlds where you know exactly which cells are new this frame.

Both paths **wrap in row-major** past the right edge: writing past
`(31, 5)` continues at `(0, 6)` on a 32×32 tilemap; past `(31, 31)`
wraps to `(0, 0)`. Documented as a feature for "instantaneous
transport" effects.

**Shadow budget**: capped at 4 layers × 32×32 = 8 KB total host
DTCM. Layers needing 64×32 or larger must use the direct path
exclusively. Hard cap, predictable.

### Mode 7 (`mg_mode7.h`)

Separate header because Mode 7 has no per-tile concept; it's one big
8bpp 1024×1024 plane with a 2D affine transform.

```c
typedef struct {
    int16_t a, b, c, d;             /* 8.8 fixed-point affine [A B; C D]  */
    int16_t cx, cy;                 /* 13-bit signed center                */
    int16_t hofs, vofs;             /* 13-bit signed scroll                */
} MgMode7Params;

typedef enum {
    MG_MODE7_WRAP, MG_MODE7_CLAMP, MG_MODE7_FILL_TILE0, MG_MODE7_FILL_BLACK,
} MgMode7Wrap;

void     mg_mode7_set         (const MgMode7Params *p);
void     mg_mode7_wrap        (MgMode7Wrap behavior);
void     mg_mode7_scale_rotate(MgMode7Params *out,
                               uint16_t scale_q8, int16_t angle_q15);
MgResult mg_mode7_chr_upload  (uint16_t vram_word, const void *src, uint16_t bytes);
MgResult mg_mode7_map_upload  (uint16_t vram_word, const void *src, uint16_t bytes);
```

**Perspective ("F-Zero", "canyon")**: not a separate API. You use
HDMA targeting `M7A..M7D` from `mg_hdma.h`. Costs 4 channels +
~900 bytes/frame as the canyon demos already demonstrate.

### HDMA (`mg_hdma.h`)

```c
typedef enum {
    MG_HDMA_DEST_BG1_HOFS = 0x0D,  MG_HDMA_DEST_BG1_VOFS = 0x0E,
    MG_HDMA_DEST_BG2_HOFS = 0x0F,  MG_HDMA_DEST_BG2_VOFS = 0x10,
    MG_HDMA_DEST_BG3_HOFS = 0x11,  MG_HDMA_DEST_BG3_VOFS = 0x12,
    MG_HDMA_DEST_BG4_HOFS = 0x13,  MG_HDMA_DEST_BG4_VOFS = 0x14,
    MG_HDMA_DEST_FIXED_COLOR = 0x32,
    MG_HDMA_DEST_M7A = 0x1B, MG_HDMA_DEST_M7B = 0x1C,
    MG_HDMA_DEST_M7C = 0x1D, MG_HDMA_DEST_M7D = 0x1E,
    MG_HDMA_DEST_M7X = 0x1F, MG_HDMA_DEST_M7Y = 0x20,
} MgHdmaDest;

typedef enum {
    MG_HDMA_XFER_1B_1R, MG_HDMA_XFER_2B_1R,
    MG_HDMA_XFER_2B_2R, MG_HDMA_XFER_4B_2R,
} MgHdmaXfer;

typedef struct {
    uint8_t     channel;
    MgHdmaDest  dest;
    MgHdmaXfer  xfer;
    bool        indirect;
} MgHdmaCfg;

void     mg_hdma_setup        (const MgHdmaCfg *cfg);
MgResult mg_hdma_upload_table (uint8_t channel, const void *table, uint16_t len);
void     mg_hdma_enable       (uint8_t channel, bool on);
uint16_t mg_hdma_wram_bytes_remaining(void);
```

**Model**: tables are staged into a reserved WRAM region during
vblank (one DMA slot + `len` bytes per upload). The PPU consumes
the table scanline-by-scanline during active display without
further coprocessor involvement, freeing the cart side to run
game logic.

**Deferred**: a future rearmed scanline-timer IRQ for mid-line
register tweaks (smaller bytes per scanline, more parameters).
That work lives in `mg_hdma_irq.h` when needed.

### CHR + palette + asset transport (`mg_gfx.h`)

```c
/* CHR — one call, one DMA slot. */
MgResult mg_chr_upload(uint16_t vram_word, const void *src, uint16_t bytes);

/* Palette — shadow CGRAM with two input formats. */
void     mg_palette_set       (uint8_t idx, uint16_t bgr555);
void     mg_palette_set_rgb   (uint8_t idx, uint8_t r, uint8_t g, uint8_t b);
void     mg_palette_load      (uint8_t start, const uint16_t *bgr555, uint16_t n);
void     mg_palette_load_rgb24(uint8_t start, const uint8_t  *rgb24,  uint16_t n);

void     mg_palette_snapshot  (uint16_t *out_512_bytes);
MgResult mg_palette_restore   (const uint16_t *in_512_bytes);

uint16_t mg_bgr555(uint8_t r, uint8_t g, uint8_t b);

/* Runtime procedural CHR packing — M7-native, not a guest-side loop. */
MgResult mg_pack_chr_4bpp(void *dst, const void *src_linear, uint16_t tile_count);
MgResult mg_pack_chr_8bpp(void *dst, const void *src_linear, uint16_t tile_count);
```

**No `mg_chr_upload_page` page-aligned helper.** The "CHR page"
convention is inconsistent across BG (4KB), sprite (8KB), and
Mode 7. Always pass `vram_word` directly.

**Palette**: shadow model identical to OAM. Per-color writes are
free; commit emits one DMA per dirty CGRAM range. Worst-case full
CGRAM DMA is one slot + 512 bytes/frame regardless of how many
`palette_set` calls happened.

**No guest-side bitplane fallback.** The M7-native syscall is fast;
a pure-C VM-side packer would be too slow to be useful, and any
runtime variant that ships without the syscall is hypothetical.

### Audio (`mg_audio.h`)

Thin renames over the existing `SYS_AUDIO_*` surface. The arbiter's
admission/eviction already handles the "attempts to play pending
eviction" semantics. Auto-free on VM exit via
`audio_service_sweep_vm` (already exists).

```c
typedef int32_t MgSfx;          /* object handle from LOAD_WAV / LOAD_SAMPLE */
typedef int32_t MgVoice;        /* voice handle from TRIGGER / PLAY / STREAM */

#define MG_AUDIO_LOOP  (1u << 0)

MgSfx    mg_sfx_load        (const char *path);
MgSfx    mg_sfx_load_sample (const void *buf, uint32_t bytes);
void     mg_sfx_free        (MgSfx handle);

MgVoice  mg_sfx_play   (MgSfx handle, int16_t gain_q15, int16_t pan_q15);
MgVoice  mg_stream_play(const char *path, uint32_t flags);  /* MG_AUDIO_LOOP */
void     mg_audio_stop (MgVoice voice);
void     mg_audio_gain (MgVoice voice, int16_t gain_q15);
```

`MgVoice == 0` means **rejected** by the arbiter (no eviction won).
Game code checks the return for whether the SFX actually got a track.

### Panic (`mg_panic.h`)

```c
void mg_panic(const char *msg) __attribute__((noreturn));
```

**Flow**:

1. Game calls `mg_panic("description")`.
2. Runtime saves message + caller context to a persistent slot in M7
   L1 DTCM (survives SNES reset; M7 doesn't reset).
3. Runtime sets a "reboot into error display" flag in the same area.
4. Runtime pulses the CIC reset line via the existing reset
   marshalling.
5. SNES boots; runtime kernel sees the error flag and stages
   `error.elf` instead of the autostart game.
6. Player sees the message; SNES reset clears the flag and boots
   the normal autostart path.

**No VRAM/CGRAM reservation during normal play.** The game owns
100% of both. The error display is just another guest ELF baked
into the runtime, gets the full hardware to itself when invoked.

## ABI numbers

Reserved range: **1180–1219** (40 slots).

```
1180-1184  cart-window staging primitives (existing)
1185-1189  reset / lifecycle              (1188 RESET_COUNT exists)
1190-1199  OAM   (sprite_set/move/hide/get/clear_all/sizes/chr_base/
                  request_range/release_range, oam_snapshot/restore)
1200-1209  BG    (mode/setup/enable/set_tile/blit/upload/get_tile/
                  scroll/mosaic/main_priority)
1210-1214  Mode 7 + HDMA
1215-1219  palette + gfx + panic
```

Exact assignments shuffle within the range as the implementation
lands; the **range** is the locked commitment.

## Template directory

```
examples/09_game_template/
├── README.md            build instructions, asset workflow, API pointers
├── build.sh             POSIX/msys2 build wrapper
├── build-win.ps1        PowerShell equivalent
├── game.c               canonical main + frame loop (~55 lines)
├── assets/
│   ├── player.png
│   ├── hud_font.png
│   └── music.wav
├── generated/           gitignored, built from assets/
└── .gitignore
```

The 55-line `game.c` skeleton exercises every concept once:
asset upload, BG layer setup, HUD text via `mg_bg_set_tile`,
sprite movement, edge-detected input, streaming music, and
`MG_OR_PANIC` on each DMA call. See the design discussion (or
just write it from this doc) for the exact source.

### `build.sh` pipeline

```
assets/*.png  --png_to_chr-->  .chr + .pal
.chr + .pal   --bin2c-->       _chr.c + _pal.c
game.c + generated/*.c --vm_objs.sh--> build/game.elf
```

Reuses `examples/common/vm_objs.sh` for the RV32IMC build path;
adds two new asset-pipeline tools (see below).

## Tooling

### `tools/png_to_chr` (NEW)

```
png_to_chr [--bpp=4|8] [--palette=shared|per-tile|multi]
           [--maxcolors=N] in.png out.chr out.pal
```

Output:
- `out.chr` — planar bitplane bytes (32B/tile for 4bpp, 64B/tile for 8bpp)
- `out.pal` — BGR555 palette entries

Quantization options:
- `--palette=shared` — one palette across all tiles (the canyon4 style)
- `--palette=per-tile` — N small palettes, one per tile
- `--palette=multi` — 8 sub-palettes, each tile picks one (FMV style); **default**

Shares quantizer code with `tools/fmv_encode.c` via a new
`tools/lib/quantize.{h,c}` extraction (the v2 quantizer's per-tile
reassignment + Lloyd refinement).

### `tools/bin2c`

Existing pattern (the host build scripts already use it). One
canonical implementation; reused unchanged.

## Deferred / out of scope

These were considered and pushed to follow-ups:

- **Per-scanline rearmed-timer IRQ** for mid-line register tweaks
  beyond what HDMA tables can express. `mg_hdma_irq.h`, later.
- **Animation systems**, **collision boxes**, **Z-order between
  actors** — too opinionated for the core library; ship as separate
  add-on libraries if a customer needs them.
- **`mg_actor_render_all(actors, n)`** batch helper. Marginal value;
  the game's main loop already iterates its own actor list.
- **Pure-C guest-side bitplane packer.** Too slow on the VM to be
  useful; the M7 syscall is the answer.
- **Window masks and color math.** A `mg_compose.h` follow-up.
- **LMB cursor input.** Not in scope for the generic game API;
  lives in a customer-specific sample when needed.

## Locked design decisions, with rationale

1. **Thin "SNES-aware" API over generic framebuffer abstraction.**
   The whole point of the coprocessor split is to expose the bandwidth
   budget. Hiding sprites/BGs/CHR/CGRAM behind a "draw image at xy"
   would prevent the customer from debugging their own overruns.

2. **One call = one DMA slot** for `mg_chr_upload` and
   `mg_bg_upload`. Predictable accounting; the game's call count
   maps 1:1 to slot consumption. Bulk operations split themselves
   into multiple calls if needed.

3. **Shadow buffers for sprites, palette, and BG tiles.** Per-cell
   writes are free; commit coalesces into one DMA per dirty range.
   Doesn't violate (2) because shadow writes aren't DMA-bearing
   calls.

4. **Sub-pixel positions live on actors, not sprites.** Sprite API
   is integer-only; Q16.16 stays in the actor layer. Clean
   separation of concerns.

5. **No runtime enforcement of shared-resource conflicts** (sprite
   slots, palette entries, CHR ranges). All guests share the same
   hardware state; "whoever writes the slot wins." Nested-program
   cooperation via explicit snapshot/restore. Programs that don't
   cooperate are the programmer's problem.

6. **`mg_panic` via SNES reset → reboot into `error.elf`** rather
   than a runtime-owned display-takeover. No permanent VRAM/CGRAM
   reservation during normal play; error display gets the full
   hardware to itself when invoked.

7. **Build-time asset tool + M7-native runtime conversion.** No
   pure-C VM-side bitplane packer. The build tool covers 95% of
   asset shipping; the M7 syscall covers procedural runtime CHR
   (canyon4 use case).

8. **64 KB cart window as a compile-time constant.** Not a runtime
   parameter. A cheaper micro with less DTCM would be a runtime
   fork, not an ABI knob.

## Open spots for the implementation pass

Things this document does not pin down:

- **Exact ABI numbers within 1180–1219.** Shuffle as the impl lands.
- **`MG_ACTOR_MAX_PARTS = 8`** default with redefine-before-include
  for games needing more.
- **`MgBgTile` field bounds checking** — left raw for hot-path
  speed; document the limits.
- **`mg_bg_blit` overflow** beyond the tilemap edge — row-major
  wrap default; consider adding a `MG_BG_BLIT_CLIP` flag later if
  a customer asks.
- **Pack-cart tooling** — a future `tools/pack_cart.sh` wraps
  `game.elf` + boot blob + kernel into a real cart image. Not the
  template's job to ship.

## See also

- [[mgapi-cart-runtime]] — host-side runtime that backs these
  primitives.
- [[snes-cart-kernel]] — SNES-side kernel + boot blob the runtime
  hands off to.
- [[r3d-renderer-and-fmv]] — the 4bpp canyon4 and FMV transports
  the asset pipeline shares quantizer code with.
- [[audio-streaming-stereo]] — the audio backend `mg_audio.h`
  wraps.
- `docs/conventions.md` — the in-tree style guide that the
  guest-library headers follow.
