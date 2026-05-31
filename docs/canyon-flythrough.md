# Canyon flythrough — design

A host-side tool plus a small runtime addition that lets you bake a
prebaked course from JSON, drop the binary into PSRAM trashfs, and
fly a camera through it along a smooth spline with banking and a
procedural snow-capped horizon. Builds on the existing
`demo_canyon4.c` (4bpp Mode-1 canyon rendered via the FMV transport)
by replacing its fixed-march camera with a course follower.

Audience: someone implementing this when picked back up, or the
customer reading the spec before authoring a course.

## What already exists (don't redo)

- **`src/video/r3d.c`** — fixed-point Q16.16 polygon engine. Affine3
  transform → project → flat-shade → z-buffer → edge raster, into an
  8bpp framebuffer. `r3d_render_dither` is the ordered-dither
  variant used by canyon4.
- **`src/video/tests/demo_canyon4.c`** — renders the canyon scene at
  240×208, swizzles to 4bpp tiles, displays via the emulated PPU as
  a Mode-1 BG. **Camera marches forward at a fixed rate** along the
  z-axis. This is what the course follower replaces.
- **Procedural canyon mesh** — the canyon geometry is regenerated
  per-frame in camera-relative x. No world data to ship. The course
  only carries camera path, not terrain.
- **Audio service / SYS_AUDIO_STREAM_WAV** — for music during a
  flight. Loaded separately from the course file.

See [[r3d-renderer-and-fmv]] for the full canyon4 background.

## Course format

JSON for human authoring; binary for runtime.

### JSON (the input)

```json
{
  "magic": "CRSE",
  "version": 1,
  "speed_units_per_sec": 60.0,
  "mountain_seed": 12345,
  "flags": [],
  "waypoints": [
    { "pos": [0, 0, 0],        "width": 40 },
    { "pos": [0, 0, 100],      "width": 40 },
    { "pos": [50, 5, 200],     "width": 38, "roll_deg": -10 },
    { "pos": [80, 10, 320],    "width": 35 },
    { "pos": [60, 5, 460],     "width": 38 },
    { "pos": [0, 0, 600],      "width": 40 },
    "...etc..."
  ]
}
```

- `pos`: world-space [x, y, z] in metres (any scale you like; the
  renderer doesn't care about absolute units).
- `width`: canyon half-width at this waypoint, in metres (the
  procedural canyon mesh uses this).
- `roll_deg` (optional): override the auto-derived bank angle at
  this waypoint, in degrees. When omitted, the runtime derives roll
  from local curvature (Frenet-Serret: bank into turns like a real
  plane).
- `speed_units_per_sec`: how fast the parameter `t` advances along
  the spline. The course can vary speed later via section flags;
  this is the default.
- `mountain_seed`: seed for the procedural backdrop generator. Same
  course → same skyline every play.

### Binary (the output, what runtime reads)

```c
struct CourseHeader {
    uint32_t magic;            /* 'CRSE' = 0x45535243 */
    uint16_t version;          /* 1                                 */
    uint16_t waypoint_count;
    int32_t  speed_q16;        /* default forward velocity (Q16.16) */
    uint32_t mountain_seed;
    uint32_t flags;            /* reserved                          */
    /* followed by waypoint_count × CourseWaypoint */
};

struct CourseWaypoint {
    int32_t  x_q16, y_q16, z_q16;  /* world position, Q16.16        */
    int16_t  roll_q15;             /* banking, -32768..32767 = -π..π */
    uint16_t width_q8;             /* canyon half-width, Q8.8        */
    uint8_t  section_flags;        /* 0=normal, 1=tunnel, ...        */
    uint8_t  _pad[3];
};
```

24 bytes per waypoint. The bake tool computes auto-derived
`roll_q15` from spline curvature when the JSON omits it.

### Sizing

| Course length | Waypoints (1/0.5s) | Bytes |
|---|---|---|
| 30 sec | 60 | 1.4 KB |
| 2 min | 240 | 5.7 KB |
| 5 min | 600 | 14 KB |
| 30 min | 3600 | 86 KB |

PSRAM-resident at runtime; **streaming is not needed** for any
plausible course length. Loaded once at boot from `/cart/courses/`.

## Bake tool — `tools/course_bake`

```
course_bake in.json out.course
```

Steps:

1. Parse JSON. Reject if magic/version mismatch.
2. Compute Catmull-Rom tangents per waypoint (derived from neighbors
   — not stored).
3. For each waypoint without explicit `roll_deg`, derive auto-roll
   from local curvature:
   `roll = atan2(side_acceleration, gravity_equivalent)`. Clamp to
   ±60°. Smooth with neighbor averaging.
4. Pack to binary: header + array of 24-byte waypoints.

Standalone host program (libc only). Lives at `tools/course_bake.c`,
builds via `gcc tools/course_bake.c -o tools/course_bake`.

## Runtime course follower

A new `src/video/course.{h,c}` exposing the loader, spline math, and
the per-frame camera builder.

```c
/* include/video/course.h */
typedef struct Course Course;

/* Load a binary .course file from disk into a freshly malloc'd
 * Course handle. Returns NULL on bad magic / version / IO. */
Course *course_load(const char *path);
void    course_destroy(Course *c);

/* Advance the parameter t by dt seconds along the spline. Returns
 * true while the course is still running; false at the end. */
bool    course_step(Course *c, float dt);

/* Build the camera affine for the current t. The renderer takes
 * this directly. */
void    course_camera(const Course *c, R3dAffine *out_view);

/* Inspect for the dev HUD. */
unsigned course_current_waypoint(const Course *c);
float    course_current_speed   (const Course *c);
float    course_current_roll    (const Course *c);   /* radians   */
unsigned course_waypoint_count  (const Course *c);
```

Per-frame math, in pseudo-code:

```c
/* segment in [0, n-1), fractional u in [0, 1) */
unsigned seg = (unsigned)(c->t);
float    u   = c->t - seg;

/* Catmull-Rom at u using control points seg-1..seg+2.
 * Tangent from finite differences on neighbors. */
camera_pos  = catmull_rom_pos (c->wp, seg, u);
forward     = catmull_rom_tan (c->wp, seg, u);   /* normalized */

right       = normalize(cross(forward, world_up));
up          = cross(right, forward);

/* Apply this segment's roll (interpolated between waypoints'
 * roll_q15 values). */
float roll  = catmull_rom_roll(c->wp, seg, u);
up          = rotate_around(up, forward, roll);

/* Build the 4×3 affine. */
*out_view = r3d_make_view(camera_pos, right, up, forward);
```

Roughly 12 multiplies per axis for CR position + ~30 for the basis
construction. Fits fixed-point on the M7 or the desktop host
without issue.

## Mountain backdrop

A separate transport from the canyon foreground. Lives at
`src/video/mountain_bg.c`.

### Layout

- **PPU mode**: still Mode 1 (BG1 + BG3; BG2 dropped for palette).
- **BG1**: the canyon, 4bpp, 240×208 framebuffer transport (existing).
- **BG3**: the mountain backdrop, 2bpp = 4 colors.
- **BG3 CHR**: 64 tiles × 16 bytes = 1 KB, baked once at boot. Each
  tile is one 8×8 column slice of a 512-pixel-wide horizon strip.
- **BG3 tilemap**: 64×32, fills the screen, references the CHR row.
  The horizontal tiling loops the strip ~4× to give 360° of effective
  yaw sweep.

### Palette (4 colors)

| Slot | Use | Approx RGB | BGR555 |
|---|---|---|---|
| 0 | Sky (light violet) | (180, 160, 220) | 0x6C16 |
| 1 | Far mountain (shadow) | (60, 50, 80) | 0x2087 |
| 2 | Mid mountain (lit) | (110, 100, 130) | 0x420D |
| 3 | Snow cap | (240, 240, 245) | 0x7BDE |

Values are starting points; iterate to taste.

### Procedural generator (boot-time)

```c
/* Once at boot, fill the 1 KB BG3 CHR strip. Deterministic from
 * seed → same course shows the same mountains every play. */
void bake_mountain_chr(uint32_t seed, uint8_t *chr_out_1024);
```

Algorithm:

1. **Seed peaks** — generate 12 random "pointy peaks" across the
   512-pixel strip. Each peak has random x position, width
   [16..64 px], height [16..80 px]. Heights bias toward shorter so a
   few stand out tall.
2. **Compute silhouette** — for each column x in 0..511, height =
   max(peak contribution at x for all peaks). Sharp tops emerge
   naturally from triangular peak shapes.
3. **Paint columns** — top to bottom per column:
   - Above silhouette: sky (color 0)
   - Top ~15% of each peak's vertical extent: snow (color 3)
   - Below snow band, slope facing "light" side: color 2
   - Below snow band, slope facing "shadow" side: color 1
   Light/shadow decided by signed local slope.

Runtime cost: paint once at boot, ~few thousand cycles on the M7.
No per-frame work after that.

### Per-frame transport

- **Yaw scroll**: `BG3HOFS = (camera_yaw_q16 >> 16) % 2048`. PPU
  register write, no DMA cost.
- **Pitch scroll**: `BG3VOFS = camera_pitch * px_per_radian`. Same.
- **Tilt** (banking horizon): one HDMA channel on `BG3HOFS` with a
  per-scanline table: `offset[y] = roll * (y - horizon_line)`. 224
  bytes/frame budget. The horizon line stays straight; lines above
  shift one way, lines below the other, giving a tilted-horizon
  effect without Mode 7.

Limit: tilt looks believable up to ~15-20°. Beyond that the
flat-strip-with-HDMA trick breaks down. For barrel rolls you'd need
Mode 7 with multi-BG or pre-rotated CHR — out of scope.

## The fly tool — `tools/canyon_fly`

Windows host program. Loads a `.course` binary, runs the
existing r3d canyon renderer with the course follower replacing the
fixed-march camera, displays via the present shim.

```
canyon_fly [--course=path] [--hud=on|off]
```

Default `--course` looks for `./course.course` in the working
directory. Default `--hud=on`.

### Dev HUD

Three lines in the top-left corner, toggleable with F1:

```
waypoint 42 / 600
speed    60.0 u/s
roll     -12.3°
```

Drawn into the 8bpp framebuffer before swizzling, so it survives
the 4bpp transport. White-on-black, monospace.

## Files to create

```
include/video/course.h        course types + API
src/video/course.c            loader + spline + camera builder
src/video/mountain_bg.c       procedural generator + transport
tools/course_bake.c           JSON → binary, standalone host tool
tools/canyon_fly.c            (mostly cribbed from demo_canyon4.c
                               with the course follower wired in)
```

Plus a build script entry for each new tool. `tools/course_bake` is
trivial gcc; `tools/canyon_fly` shares the existing canyon4 link
line plus `src/video/course.c` and `src/video/mountain_bg.c`.

`demo_canyon4.c` stays untouched as the simpler "renderer only,
fixed march" regression demo. The course path is a parallel target.

## Locked design decisions

1. **Course is PSRAM-resident, not streamed.** Even 30-minute
   courses fit in <100 KB; streaming buys complexity for no gain.
2. **JSON for the human format**, packed binary for the runtime.
   Bake tool is a separate standalone host program.
3. **Catmull-Rom interpolation** through waypoints. Tangents derived
   from neighbors at runtime, not stored. Cheap, smooth, no need
   for the author to specify tangents.
4. **Roll is auto-derived from curvature by default, explicit
   override per waypoint.** Real-plane bank for natural feel,
   manual control for dramatic moments.
5. **BG2 dropped; mountains on BG3 (2bpp, 4 colors).** Procedural
   pointy-peaked snow-capped silhouette against light violet sky.
6. **Mountain seed lives in the course header** so the same course
   shows the same skyline every play.
7. **Mountains procedural for now** — stand-in. Hand-painted CHR is
   a later swap of `bake_mountain_chr` with the rest of the
   transport unchanged.
8. **Tilt via HDMA on BG3HOFS**, not Mode 7. Believable to ~20° of
   bank; beyond that is a later problem.
9. **Course format starts open, not looped.** A `flags` bit for
   "closed loop" exists in the header but the first impl is fly-
   straight-through.

## Deferred / out of scope

- **Closed-loop courses.** Header flag bit reserved; not wired.
- **Section transitions** (e.g. canyon → tunnel → open sky). The
  per-waypoint `section_flags` byte exists; the renderer doesn't
  branch on it yet.
- **Variable mountain CHR per section.** One skyline per course.
- **Hand-painted mountains.** Drop-in replacement for the procedural
  generator when art priority rises.
- **Barrel rolls / mountains rotating past 20°.** Would need Mode 7
  + multi-BG or pre-rotated CHR.
- **Per-segment palette tints.** No "dusk", "dawn", "underground"
  shifts yet — single palette for the whole course.
- **Audio cue triggers along the course.** Events table is not in
  the format; music plays independently via SYS_AUDIO_STREAM_WAV.
- **Hot-reload via dev USB bridge.** Files load at boot only; a
  reload key combo is left for the implementation pass.

## Open spots for the implementation pass

- **JSON parser library choice.** A 200-line hand-rolled parser
  covers this schema; jsmn or similar would also work. Tool is
  standalone host so dependency cost is low.
- **Frame rate target.** Canyon4 has a 20/30 fps toggle. The
  fly tool should respect it; the course's `speed_units_per_sec`
  doesn't depend on framerate (the spline parameter advances in
  real seconds).
- **What happens at end of course.** Loop back to t=0? Hover at the
  last waypoint? Halt the tool? Probably "freeze and prompt for
  Esc" for the dev fly tool; real game would decide per gameplay.
- **Auto-roll smoothing window.** Bigger window = smoother feel,
  smaller window = more responsive. Default 3-waypoint averaging is
  a reasonable starting point.

## See also

- [[r3d-renderer-and-fmv]] — the renderer this builds on.
- [[snes-coprocessor-game]] — the larger system context.
- [[game-api-decisions]] — the cart-side API surface that this
  flythrough work is informing (course loading, mountain transport
  — both eventually need a game.elf-facing wrapper).
- `docs/game-api.md` — the full game API spec.
- `docs/conventions.md` — style guide for any new files.
