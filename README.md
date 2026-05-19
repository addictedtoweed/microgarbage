# John's Cavalcade of Embedded Garbage

A small library of reusable C modules for embedded and bare-metal
projects. Public domain (CC0). No warranty.

The targets in mind are M0/M3/M4-class microcontrollers, but
nothing here is architecture-specific — these will work fine on a
hosted system too.

A small amount of third-party code (FatFs, used by
`trashdrive_fatfs`) lives under `third_party/` with its own
license — see `third_party/README.md`.

## Layout

```
garbage/
├── README.md
├── modules.txt
├── include/                    ← all public headers live here
│   ├── test_runner.h
│   ├── containers.h            ← aggregator (pulls in all containers)
│   ├── math.h                  ← aggregator (pulls in all math)
│   ├── audio.h                 ← aggregator (pulls in all audio)
│   ├── storage.h               ← aggregator (pulls in all storage)
│   ├── memory.h                ← aggregator (pulls in all memory)
│   ├── containers/
│   │   ├── hashtable.h
│   │   ├── ring_buffer.h
│   │   ├── fifo_queue.h
│   │   └── stack.h
│   ├── math/
│   │   ├── fixed_point.h
│   │   └── fast_div.h
│   ├── audio/
│   │   ├── audio_mixer.h
│   │   └── music_player.h
│   ├── storage/
│   │   ├── trashdrive.h
│   │   └── trashdrive_fatfs.h    ← FatFs bridge (needs third_party/fatfs)
│   ├── memory/
│   │   ├── bump.h
│   │   └── slab_stack.h
│   └── vm/
│       ├── vm_core.h
│       ├── vm_ecall.h
│       ├── vm_host_stdio.h
│       ├── vm_loader.h
│       ├── vm_mailbox.h
│       ├── vm_sched.h
│       └── vm_system.h
│
└── src/                        ← implementation + tests, mirrors include/
    ├── containers/
    │   ├── hashtable.c
    │   ├── ring_buffer.c
    │   ├── fifo_queue.c
    │   ├── stack.c
    │   └── test_*.c
    ├── math/
    │   ├── fixed_point.c
    │   ├── fast_div.c
    │   └── test_*.c
    ├── audio/
    │   ├── audio_mixer.c
    │   ├── music_player.c
    │   └── test_*.c
    ├── storage/
    │   ├── trashdrive.c
    │   ├── trashdrive_fatfs.c    ← FatFs diskio shim
    │   ├── test_trashdrive.c
    │   └── test_trashdrive_fatfs.c
    ├── memory/
    │   ├── bump.c
    │   ├── slab_stack.c
    │   ├── test_bump.c
    │   ├── test_slab_stack.c
    │   └── test_bump_on_slab.c
    └── vm/                       ← RV32IMC interpreter + scheduler
        ├── vm_core.c             ← dispatcher (the instruction loop)
        ├── vm_loader.c           ← ELF32 loader
        ├── vm_ecall.c            ← syscall router
        ├── vm_ecall_handlers.c   ← built-in syscall handlers
        ├── vm_mailbox.c          ← per-VM mailboxes
        ├── vm_sched.c            ← cooperative scheduler
        ├── vm_system.c           ← top-level composition
        ├── vm_host_stdio.c       ← optional host stdin/stdout bridge
        └── test_vm_*.c           ← 14 test suites
```

A separate `examples/` directory contains runnable demos that
embed the VM:

```
examples/
├── README.md
├── common/                       ← shared linker script + build helper
│   ├── guest.ld
│   └── vm_objs.sh
├── 01_hello/                     ← minimal "print and exit" guest
├── 02_counter/                   ← scheduling demo (SYS_YIELD)
├── 03_mailbox/                   ← two guests talking via mailbox
└── 04_keydump/                   ← raw-mode terminal input
```

A `third_party/` directory holds vendored code that uses a
different license from the rest of the repo:

```
third_party/
├── README.md
└── fatfs/                        ← Elm Chan FatFs (BSD-1-clause)
    ├── LICENSE.txt
    ├── ffconf.h                  ← OUR tuned config
    └── source/                   ← from elm-chan.org (NOT committed —
                                   ←  see PLACEHOLDER.md for setup)
```

All public headers live under `include/`. The category aggregators
sit at `include/`'s top level for one-include access; per-module
headers sit one directory deeper.

## Usage

1. Drop the `garbage/` directory somewhere in your source tree.
2. Add `-Igarbage/include` to your compiler flags.
3. Include what you need:

```c
#include "containers.h"   /* all data structures */
#include "math.h"         /* fixed_point, fast_div */
#include "audio.h"        /* audio_mixer, music_player */
```

You can also include individual modules:
```c
#include "containers/ring_buffer.h"
```

4. Add the `.c` files of the modules you use to your build. If
   the linker complains about a missing symbol, add the .c file
   the symbol lives in — that's a module you depend on transitively.

### Dependency table

If you'd rather check dependencies upfront than rely on linker
errors:

| Module        | Depends on (compile-time)            |
|---------------|--------------------------------------|
| hashtable     | (none)                               |
| ring_buffer   | (none)                               |
| fifo_queue    | ring_buffer                          |
| stack         | (none)                               |
| fixed_point   | (none)                               |
| fast_div      | (none)                               |
| audio_mixer   | ring_buffer, fixed_point             |
| music_player  | audio_mixer (and its deps)           |
| trashdrive    | (none)                               |
| trashdrive_fatfs | trashdrive, FatFs (third_party)   |
| bump          | slab_stack (only if using slab path) |
| slab_stack    | (none)                               |
| vm_core       | (none)                               |
| vm_loader     | vm_core, bump                        |
| vm_ecall      | vm_core                              |
| vm_mailbox    | fifo_queue, ring_buffer              |
| vm_sched      | vm_core, vm_ecall                    |
| vm_system     | all of the above + slab_stack        |
| vm_host_stdio | vm_system (optional host bridge)     |

So for example, to use `music_player`, copy and build:
`music_player.c`, `audio_mixer.c`, `ring_buffer.c`, and the
corresponding headers in `include/`.

## Building the tests

From the `garbage/` directory:

```sh
# containers
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_hashtable src/containers/test_hashtable.c src/containers/hashtable.c
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_ring_buffer src/containers/test_ring_buffer.c src/containers/ring_buffer.c
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_fifo_queue src/containers/test_fifo_queue.c \
   src/containers/fifo_queue.c src/containers/ring_buffer.c
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_stack src/containers/test_stack.c src/containers/stack.c

# math
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_fixed_point src/math/test_fixed_point.c src/math/fixed_point.c
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_fast_div src/math/test_fast_div.c src/math/fast_div.c

# audio
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_audio_mixer src/audio/test_audio_mixer.c \
   src/audio/audio_mixer.c src/containers/ring_buffer.c
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_music_player src/audio/test_music_player.c \
   src/audio/music_player.c src/audio/audio_mixer.c src/containers/ring_buffer.c

# storage
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_trashdrive src/storage/test_trashdrive.c src/storage/trashdrive.c

# memory
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_bump src/memory/test_bump.c src/memory/bump.c
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_slab_stack src/memory/test_slab_stack.c src/memory/slab_stack.c
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o test_bump_on_slab src/memory/test_bump_on_slab.c \
   src/memory/bump.c src/memory/slab_stack.c

# vm — many small suites plus a real-ELF integration test
# (the integration tests need ELFs from examples/01_hello/build/,
#  02_counter/build/, and 04_keydump/build/ — build those first
#  via examples/01_hello/build.sh, examples/02_counter/build.sh,
#  and examples/04_keydump/build.sh; or skip those two suites)

VM_SRCS="src/vm/vm_core.c src/vm/vm_loader.c src/vm/vm_ecall.c \
         src/vm/vm_ecall_handlers.c src/vm/vm_mailbox.c \
         src/vm/vm_sched.c src/vm/vm_system.c src/vm/vm_host_stdio.c \
         src/memory/bump.c src/memory/slab_stack.c \
         src/containers/fifo_queue.c src/containers/ring_buffer.c"

for suite in vm_core vm_core_alu vm_core_c vm_core_m vm_core_memctl \
             vm_core_system vm_ecall vm_ecall_handlers vm_host_stdio \
             vm_loader vm_mailbox vm_real_elf vm_sched vm_system; do
    cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
       -o test_$suite src/vm/test_$suite.c $VM_SRCS
done
```

## Modules

### containers

#### hashtable

String-keyed hashtable with auto-resize and chained collisions.
Pluggable allocator (defaults to malloc/free); the `_create_with_allocator`
variant accepts custom alloc/free function pointers.

FNV-1a hash, configurable initial capacity, resizes at 0.75 load.

API: `ht_create`, `ht_create_with_allocator`, `ht_destroy`,
`ht_put`, `ht_get`, `ht_remove`, `ht_size`, `ht_iter`.

#### ring_buffer

Fixed-capacity circular buffer. Caller-provided storage. Memcpy
semantics — any element size works. Overwrites oldest element on
push when full.

For reject-on-full semantics (a bounded FIFO queue), see `fifo_queue`.

API: `rb_init`, `rb_reset`, `rb_push`, `rb_pop`, `rb_peek`,
`rb_peek_at`, `rb_count`, `rb_empty`, `rb_full`.

#### fifo_queue

Bounded FIFO queue with reject-on-full semantics. Thin wrapper
around `ring_buffer` that fails on full instead of overwriting.

API: `fifo_init`, `fifo_push` (returns success), `fifo_pop`,
`fifo_peek`, `fifo_count`, `fifo_empty`, `fifo_full`.

#### stack

Fixed-capacity LIFO stack. Caller-provided storage. Reject-on-full.

API: `stack_init`, `stack_reset`, `stack_push`, `stack_pop`,
`stack_peek`, `stack_count`, `stack_empty`, `stack_full`.

### math

#### fixed_point

Q-format fixed-point arithmetic. Five formats:
- `q15_t` / `q31_t` — audio samples
- `q16_16_t` — general-purpose DSP working format
- `q32_32_t` — phase accumulators
- `q48_16_t` — long-running counters with sub-unit precision

Each format provides add/sub/mul/div/mac/neg/abs (wrapping), plus
`sat_*` variants that saturate. Float and double conversions per
format. Cross-format conversions.

Header-only; the `.c` file exists for build consistency but is
essentially empty.

Use plain ops for phase accumulators (wrap is correct), use `sat_*`
ops for audio sample math (clip is correct).

#### fast_div

Fast integer division by a runtime-known divisor. Pays an upfront
cost to "prepare" the divisor (compute a magic reciprocal), then
each division becomes a multiply-and-shift.

On M0/M3 (no hardware divide; software UDIV is ~150 cycles): ~17x
faster than software division.

On M4 with hardware divide (~12 cycles): marginal as a non-inlined
function call, ~2.4x faster if inlined via LTO.

Four type variants: `u32`, `s32`, `u64`, `s64`. Each has a `_prepare`,
quotient-only, and combined quotient+remainder function.

Uses the Granlund-Möller algorithm with extensive comments in
fast_div.c explaining the math. Verified against plain `/` and `%`
across hundreds of divisor/dividend combinations.

API: `fdiv_<type>_prepare`, `fdiv_<type>`, `fdiv_<type>_qr`.

### audio

#### audio_mixer

Multi-channel audio mixer with parameterized output, per-channel
allocation, resampling, pan, looping, and optional clock sync.

Source formats: 8-bit unsigned/signed, 16-bit signed; mono or
stereo. Output: 1-16 bit resolution, signed or unsigned, 8-bit or
16-bit storage, mono or stereo. Per-channel source rate with linear
or cubic interpolation. Per-channel pan and looping.

Allocates everything at create time. After create, the render loop
is allocation-free. Pass your own allocator to `mixer_create_with_allocator`
if you want control over where the memory comes from.

Interpolation tradeoff:
- LINEAR: ~5-8 cycles/channel/frame on M0/M3. Acceptable for SFX.
- CUBIC: ~20-50 cycles/channel/frame. Better quality, suited for
  music/voice. The practical quality ceiling for upsampling use.

**Clock sync (v2):** create with `mixer_create_with_sync` to enable
drift correction against an external clock. The application
periodically calls `mixer_observe_sync` with the current pair of
internal-frame and external-tick counters; the mixer applies a
smoothed correction to per-channel playback rates. Under sync, all
channels go through the resampling path so correction applies
uniformly. Useful for tracking an external master clock (e.g.,
SNES master) when the mixer's internal clock would otherwise drift
relative to it.

API: `mixer_create`, `mixer_create_with_allocator`,
`mixer_create_with_sync`, `mixer_destroy`,
`mixer_set_volume`, `mixer_set_pan`, `mixer_mute`,
`mixer_channel_start`, `mixer_channel_stop`, `mixer_channel_reset`,
`mixer_write_channel`, `mixer_render`, `mixer_channel_buffered`,
`mixer_observe_sync`, `mixer_reset_sync`.

#### music_player

Intro-and-loop music playback on top of `audio_mixer`. Handles the
common game-audio pattern of a non-looping intro section followed
by a looping body.

The player borrows one mixer channel and feeds samples into it
over time. Source data is pulled through a single user-supplied
callback that the player calls when its internal streaming buffer
runs low. The callback receives an opaque stream ID, destination,
sample count, and offset — typically the user maps the stream ID
to an open file handle.

The player optionally maintains pinned heads — the first N samples
of the intro and/or loop kept resident in RAM. When playback
(re)starts, the pinned head plays first while the stream callback
catches up, giving rapid restart even when reading from SD card.

The intro→loop transition is sample-accurate and gap-free.

The stream callback is called synchronously from `music_update`,
which must run from non-real-time context (main loop or low-priority
task). The mixer's real-time render path is unaffected.

API: `music_create`, `music_create_with_allocator`, `music_destroy`,
`music_prime_intro`, `music_prime_loop`, `music_play`, `music_stop`,
`music_pause`, `music_resume`, `music_update`, `music_state`.

### storage

#### trashdrive

A small RAM-backed block device for mounting your own filesystem
on top of. "BYOFS" — bring your own filesystem. Designed to pair
with FatFs or Petit FatFs (or anything else that consumes a block
device interface), giving you a tiny FAT volume in RAM that uses
the same API as your SD card.

Why this shape: most embedded projects already have a filesystem
library (typically FatFs) integrated for SD card access. By
implementing a block device rather than a filesystem, trashdrive
plugs into that existing library — your code reads RAM-backed files
through the same `f_open` / `f_read` / `f_write` calls as SD files,
just with a different drive number.

Properties:
- Fixed 512-byte sectors (matches FatFs / Petit FatFs conventions).
- Caller provides the memory region. Minimum 16 KB, must be a
  multiple of 512 bytes.
- Region contents NOT touched by `trash_init` — formatting is the
  filesystem library's job (e.g., `f_mkfs`). Use `trash_clear` if
  you want to zero the region first.
- No allocations: the `TrashDrive` struct is caller-declared, just
  a few pointers and counts. Init fills it in.

Typical wiring with FatFs (excerpt — NOT part of this module):

```c
static TrashDrive g_ram_drive;

void app_init(void) {
    static uint8_t psram_region[1024 * 1024];   // wherever you want it
    trash_init(&g_ram_drive, psram_region, sizeof(psram_region));
}

// In your project's diskio.c:
DRESULT disk_read(BYTE pdrv, BYTE *buf, LBA_t sec, UINT n) {
    if (pdrv != DRIVE_RAM) return RES_PARERR;
    return trash_read(&g_ram_drive, buf, sec, n) == TRASH_OK
           ? RES_OK : RES_ERROR;
}
// ... similar wrappers for disk_write, disk_ioctl, etc.
```

Then application code is normal FatFs:

```c
FATFS fs;
f_mount(&fs, "1:", 1);
f_mkfs("1:", NULL, NULL, 0);   // first time only

FIL f;
f_open(&f, "1:/scripts/foo.lua", FA_WRITE | FA_CREATE_ALWAYS);
f_write(&f, code, code_len, &bw);
f_close(&f);
```

API: `trash_init`, `trash_clear`, `trash_read`, `trash_write`,
`trash_sector_count`, `trash_sector_size`, `trash_total_bytes`.

#### trashdrive_fatfs

Bridges `trashdrive` to Elm Chan's [FatFs](http://elm-chan.org/fsw/ff/)
library so you can mount a real FAT filesystem inside a RAM
region. After registration you use FatFs's standard API
(`f_open`, `f_read`, `f_write`, `f_mkdir`, `f_unlink`, etc.) —
the diskio shim in this module routes the underlying sector
reads and writes to the registered TrashDrive.

This is a thin C file (~150 lines) that implements the five
`disk_*` functions FatFs requires. The actual filesystem logic
lives in FatFs proper, under `third_party/fatfs/`. **FatFs is
NOT public domain** like the rest of this library — it's
BSD-1-clause licensed by Elm Chan. See `third_party/README.md`
and `third_party/fatfs/LICENSE.txt` for the details.

```c
#include "storage/trashdrive.h"
#include "storage/trashdrive_fatfs.h"
#include "ff.h"

static uint8_t  pool[64 * 1024];
static TrashDrive drive;
static FATFS fs;

/* Initialize the block device, then register it as FatFs drive 0. */
trash_init(&drive, pool, sizeof(pool));
trash_fatfs_register(0, &drive);

/* Format and mount. */
BYTE work[FF_MAX_SS];
f_mkfs("0:", NULL, work, sizeof(work));
f_mount(&fs, "0:", 1);

/* Use standard FatFs from here. */
FIL f;
f_open(&f, "0:/hello.txt", FA_WRITE | FA_CREATE_ALWAYS);
UINT bw;
f_write(&f, "hi", 2, &bw);
f_close(&f);
```

API: `trash_fatfs_register`, `trash_fatfs_get`. Everything else
is FatFs (see `third_party/fatfs/source/ff.h` for the full API
once you've extracted FatFs).

**Setting it up:**

1. Run `./setup_licenses.sh` from the repo root (writes `LICENSE`
   and creates `third_party/` scaffold). See the script's
   comments for what it does.
2. Download FatFs from <http://elm-chan.org/fsw/ff/> and extract
   the source files into `third_party/fatfs/source/`. The exact
   files needed are documented in
   `third_party/fatfs/PLACEHOLDER.md`.
3. Build with the extra include paths:
   `-Ithird_party/fatfs/source -Ithird_party/fatfs -DHAVE_FATFS`
4. Link `src/storage/trashdrive_fatfs.c`, `src/storage/trashdrive.c`,
   `third_party/fatfs/source/ff.c`, and
   `third_party/fatfs/source/ffsystem.c`.

The tests in `src/storage/test_trashdrive_fatfs.c` will compile
without FatFs (they print "skipped" and exit cleanly) so CI
keeps green even when FatFs isn't present. With `-DHAVE_FATFS`
and the FatFs source in place, they run real `f_open`/`f_write`/
`f_mkdir`/`f_unlink` operations against a 64 KB RAM volume.

### memory

#### bump

A bump allocator: simplest possible allocator pattern. A region
plus a pointer that advances on each allocation. Allocations are
essentially free (pointer math plus alignment); you cannot free
individual allocations, only reset the whole arena at once.

Good for "allocate a bunch of related things, use them all, throw
them all away" workloads — per-frame allocations, per-request
state, per-script-load buffers, temporary work during init.

The arena's memory comes from either:
- A caller-provided buffer (`bump_init` with a region pointer)
- A slab_stack allocator instance (`bump_init_from_slab` — slab
  must be present and linked)

No per-allocation header. No magic number. Allocations are raw
pointers with no metadata. If you need corruption detection, use
slab_stack directly. The slab-backed bump path inherits slab's
header protections for the arena chunk itself.

Default alignment is 8 bytes (BUMP_DEFAULT_ALIGNMENT). Custom
alignment via `bump_alloc_aligned`.

Tracks a peak-water-mark (`b->peak_offset`) so you can tune the
arena size after deployment.

```c
static uint8_t arena[4096];
BumpAllocator b;
bump_init(&b, arena, sizeof(arena));

MyStruct *items[20];
for (int i = 0; i < 20; i++) {
    items[i] = bump_alloc(&b, sizeof(MyStruct));
    // ...
}
// Use items here.

bump_reset(&b);  // All pointers now invalid; arena reset to start.
```

API: `bump_init`, `bump_init_from_slab`, `bump_destroy`,
`bump_reset`, `bump_alloc`, `bump_alloc_aligned`,
`bump_used`, `bump_remaining`, `bump_capacity`, `bump_peak`.

#### slab_stack

A two-layer slab allocator with O(1) alloc and free. Designed for
embedded systems where predictable allocation behavior matters
more than squeezing out every byte.

Architecture:
- 16 power-of-2 bins (32B, 64B, 128B, ... up to 1MB)
- Each bin holds a fixed number of fixed-size blocks (config-driven)
- A 32-bit bitmap tracks which bins have free blocks (level 1)
- Each bin has a stack of pointers to its free blocks (level 2)
- Allocate = check bitmap, pop from stack. Free = push, set bit.

Properties:
- Caller provides the memory region. Allocator partitions it at init.
- Config struct specifies per-bin block counts; bins with 0 are unused.
- 8-byte header per block (4B magic + 4B bin index).
- Magic catches double-free, foreign pointers, and header corruption.
  Strip with `-DSLAB_NO_MAGIC` for release builds.
- No fallback to larger bins: if your bin is exhausted, alloc fails.
  This is by design — predictable failure beats hidden fragmentation.
- No splitting, no coalescing.
- Live stats embedded in the allocator struct as public read-only
  fields: `a->total_bytes_in_use`, `a->peak_bytes_in_use`,
  `a->bins[i].blocks_in_use`, `a->bins[i].peak_in_use`, etc.

Locking is user-supplied via callbacks. Pass `slab_null_locker` for
single-threaded use. For ISR-safe use, implement lock/unlock as
PRIMASK save/restore (save-state pattern: lock returns the saved
state, unlock takes it back).

```c
SlabConfig cfg = {0};
cfg.bucket_counts[0] = 32;   /* 32 blocks of 32 bytes */
cfg.bucket_counts[1] = 16;   /* 16 blocks of 64 bytes */
cfg.bucket_counts[2] = 8;    /* 8 blocks of 128 bytes */

size_t need = slab_required_bytes(&cfg);
static uint8_t pool[4096];   /* size for your config */

SlabAllocator a;
slab_init(&a, pool, sizeof(pool), &cfg, slab_null_locker);

void *p = slab_alloc(&a, 50);    /* gets a 64-byte block */
/* ... use p ... */
slab_free(&a, p);

/* Inspect usage directly: */
printf("Used: %zu / %zu (peak %zu)\n",
       a.total_bytes_in_use, a.total_bytes_managed, a.peak_bytes_in_use);
```

API: `slab_init`, `slab_destroy`, `slab_required_bytes`,
`slab_alloc`, `slab_free`,
`slab_bytes_used`, `slab_bytes_free`, `slab_bytes_peak`.

##### Using slab_stack as the project-wide allocator

Several modules in this library (hashtable, audio_mixer, music_player)
take `alloc`/`free` callbacks with `malloc`-compatible signatures:

```c
typedef void *(*alloc_fn)(size_t n);
typedef void  (*free_fn)(void *p);
```

`slab_alloc` and `slab_free` don't match these directly because they
take an allocator pointer as their first argument. To wire slab_stack
in as the project's allocator, write thin wrappers in your project
that close over your slab instance:

```c
/* In your project's memory.c (or similar): */

static uint8_t  g_pool[64 * 1024];
static SlabAllocator g_slab;

void *project_alloc(size_t n) { return slab_alloc(&g_slab, n); }
void  project_free(void *p)   { slab_free(&g_slab, p); }

void project_memory_init(void) {
    SlabConfig cfg = {0};
    cfg.bucket_counts[0] = 64;   /* tune to your workload */
    cfg.bucket_counts[1] = 32;
    cfg.bucket_counts[2] = 16;
    /* ... etc ... */
    slab_init(&g_slab, g_pool, sizeof(g_pool), &cfg, slab_null_locker);
}
```

Then pass the wrappers to any module that takes an allocator:

```c
project_memory_init();   /* must happen first */

HashTable *ht = ht_create(16, project_alloc, project_free);
AudioMixer *mix = mixer_create_with_allocator(8, 1024, 48000,
                                               &fmt, 0,
                                               project_alloc, project_free);
```

The library deliberately doesn't provide these wrappers itself — they
introduce a global, and the library is built to avoid globals so that
multiple independent allocators can coexist (one per Modbus master,
one per core, one per logical subsystem, etc.). Each project decides
its own integration shape.

**Thread safety:** the wrappers inherit the thread safety of the
underlying slab. If your slab is configured with `slab_null_locker`,
the wrappers are single-threaded. For multi-threaded or ISR-safe
use, configure the slab with an appropriate locker (typically
`__disable_irq`/`__enable_irq` saved-state pair on Cortex-M).

**Order of init:** the slab must be initialized before any module
that uses the wrappers. Failing this returns NULL from `project_alloc`
and the module's `create` will fail. Initialize slab very early in
your boot sequence.

### vm

#### vm_core, vm_loader, vm_ecall, vm_mailbox, vm_sched, vm_system, vm_host_stdio

A small RV32IMC virtual machine for running multiple guest programs
cooperatively on a single host. The "core" is a portable
switch-on-opcode interpreter; the supporting modules add an ELF32
loader, ECALL syscall routing, per-VM mailbox messaging, a
cooperative scheduler, and a top-level composition that wires it
all together.

Why this exists: dynamically loadable, sandboxable programs on
microcontrollers that don't have an MMU or a "real" OS. Programs
are built with stock `riscv32-unknown-elf-gcc` (or clang) — no
custom toolchain. Standard tools work: objdump, addr2line, etc.

Properties:
- **Single header to embed:** `vm_system.h` (which transitively
  pulls in the rest).
- **No mandatory allocator:** caller provides shared and local
  memory pools; everything is sub-allocated from those.
- **RV32IMC instruction set:** I (integer), M (multiply/divide),
  C (compressed). No floats, no atomics, no privilege levels.
- **Four address regions** (top 2 bits of the 32-bit virtual
  address select): CODE, RODATA, DATA (per-VM private), SHARED
  (cross-VM, host-managed).
- **Up to 64 VMs per system** (`uint64_t` ready/blocked bitmaps
  in the scheduler).
- **Cooperative scheduling** with adaptive quantum and critical-
  section debt amortization. Guests yield voluntarily; the
  scheduler also wakes blocked guests when their message arrives
  or their timeout expires.
- **ECALL syscalls** in two ranges: Linux-compatible low numbers
  (SYS_EXIT=93, SYS_READ=63, SYS_WRITE=64) and a VM-specific
  range starting at 1024 (SYS_SELF, SYS_YIELD, SYS_ALLOC,
  SYS_SEND, etc.). Auto-installed by `vm_system_init`; hosts can
  add their own.
- **Optional host stdio bridge** (`vm_host_install_stdio`) for
  forwarding guest SYS_READ/SYS_WRITE to the host process's
  stdin/stdout/stderr — including raw-mode terminal input for
  TUI / game-style guests.
- **Three working examples in `examples/`**: load and exit, a
  yielding counter, two guests cooperating via mailbox, and a
  raw-mode keystroke dumper.

Public domain like the rest of the library. See the
`vm_*.h` headers for per-module ABI docs. See `examples/README.md`
for the runnable demos.

API surface (each module's `.h` has full docs):
- `vm_init`, `vm_step`, `vm_translate_*` (vm_core)
- `vm_load`, `VmBacking` (vm_loader)
- `vm_ecall_router_init`, `vm_ecall_register`, `vm_ecall_dispatch` (vm_ecall)
- `vm_mailbox_init`, `vm_mailbox_send`, `vm_mailbox_recv`,
  `vm_mailbox_whitelist_*` (vm_mailbox)
- `vm_sched_init`, `vm_sched_register`, `vm_sched_step` (vm_sched)
- `vm_system_init`, `vm_system_load_vm`, `vm_system_run`,
  `vm_system_step` (vm_system)
- `vm_host_install_stdio`, `vm_host_install_stdio_ex`,
  `VmHostStdioConfig` (vm_host_stdio)

## Roadmap

These are planned but not yet built:

- **moving_avg** — EMA and windowed moving average filters
- **matrix** — small fixed-size matrix operations (3x3, 4x4)
- **linear_regression** — basic least-squares fit
- **vendor tool** — interactive picker with dependency resolution

Build order is "when there's a real use case for it." Don't read
roadmap items as commitments; they're more like notes-to-self
about what direction this might go.

## Conventions

Across modules:

- 2-3 letter prefix per module matching its name (`ht_*`, `rb_*`,
  `mixer_*`, etc).
- Caller-provided storage for fixed-capacity modules; allocator
  pattern for dynamic ones.
- Functions returning `int` for success/failure use 0 / -1.
- Saturating ops named `sat_*`; plain ops wrap (matches native
  `int` behavior).
- All public APIs documented in their headers.
