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
│   │   ├── stack.h
│   │   ├── slist.h             ← singly-linked list (lean, node pool)
│   │   ├── dlist.h             ← doubly-linked list (node pool)
│   │   └── tree.h              ← ordered tree, BST/AVL (node pool)
│   ├── math/
│   │   ├── fixed_point.h
│   │   └── fast_div.h
│   ├── audio/
│   │   ├── audio_mixer.h
│   │   ├── music_player.h
│   │   ├── audio_pool.h          ← block pool for samples-at-rest
│   │   ├── audio_pool_stream.h   ← pool → music_player stream adapter
│   │   ├── audio_arbiter.h       ← track arbitration (FCFS / priority)
│   │   ├── audio_service.h       ← mixer/pool/arbiter behind a channel
│   │   ├── audio_fft.h           ← FFT band meter (fixed-point, no libm)
│   │   └── audio_sink.h          ← output-backend seam + WAV parse
│   ├── storage/
│   │   ├── trashdrive.h
│   │   ├── trashdrive_fatfs.h    ← FatFs bridge (needs third_party/fatfs)
│   │   └── trashfs.h             ← tiny built-in read/write filesystem
│   ├── memory/
│   │   ├── bump.h
│   │   └── slab_stack.h
│   └── vm/
│       ├── vm_core.h
│       ├── vm_ecall.h
│       ├── vm_host_stdio.h
│       ├── vm_host_fs.h        ← optional file syscalls (needs FatFs)
│       ├── vm_host_audio.h     ← guest SYS_AUDIO_* → audio service
│       ├── vm_loader.h
│       ├── vm_mailbox.h
│       ├── vm_sched.h
│       ├── service_channel.h   ← SPSC request/response channel
│       ├── channel_thread.h    ← desktop (pthread) channel transport
│       └── vm_system.h
│
└── src/                        ← implementation + tests, mirrors include/
    ├── containers/
    │   ├── hashtable.c
    │   ├── ring_buffer.c
    │   ├── fifo_queue.c
    │   ├── stack.c
    │   └── tests/                ← unit-test suites (test_*.c)
    ├── math/
    │   ├── fixed_point.c
    │   ├── fast_div.c
    │   └── tests/                ← unit-test suites (test_*.c)
    ├── audio/
    │   ├── audio_mixer.c
    │   ├── music_player.c
    │   ├── audio_pool.c          ← refcounted block pool
    │   ├── audio_pool_stream.c
    │   ├── audio_arbiter.c
    │   ├── audio_service.c       ← the service worker
    │   ├── audio_fft.c           ← band meter
    │   ├── audio_fft_kernel.c    ← fixed-point radix-2 FFT
    │   ├── audio_wav_read.c      ← RIFF/WAVE parser
    │   ├── audio_sink_wav.c      ← WAV-dump output backend
    │   ├── audio_sink_waveout.c  ← live Win32 waveOut backend
    │   └── tests/                ← unit-test suites (test_*.c)
    ├── storage/
    │   ├── trashdrive.c
    │   ├── trashdrive_fatfs.c    ← FatFs diskio shim
    │   ├── trashfs.c             ← built-in filesystem
    │   └── tests/                ← unit-test suites (test_*.c)
    ├── memory/
    │   ├── bump.c
    │   ├── slab_stack.c
    │   └── tests/                ← unit-test suites (test_*.c)
    └── vm/                       ← RV32IMC interpreter + scheduler
        ├── vm_core.c             ← dispatcher (the instruction loop)
        ├── vm_loader.c           ← ELF32 loader
        ├── vm_ecall.c            ← syscall router
        ├── vm_ecall_handlers.c   ← built-in syscall handlers
        ├── vm_mailbox.c          ← per-VM mailboxes
        ├── vm_sched.c            ← cooperative scheduler
        ├── vm_system.c           ← top-level composition
        ├── vm_host_stdio.c       ← optional host stdin/stdout bridge
        ├── vm_host_fs.c          ← optional host file syscalls (FatFs)
        ├── vm_host_audio.c       ← guest audio syscalls → service
        ├── service_channel.c     ← SPSC request/response channel
        ├── channel_thread.c      ← pthread channel transport (POSIX/Cygwin)
        ├── channel_win32.c       ← Win32 channel transport (native Windows)
        └── tests/                ← VM unit-test suites (test_vm_*.c)
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
├── 04_keydump/                   ← raw-mode terminal input
└── 05_shell/                     ← the interactive host (the main
                                   ←  deliverable): a shell that spawns
                                   ←  guest ELFs, with stdio, files,
                                   ←  TUI, and live audio
```

Design notes for the larger subsystems live under `docs/`:
`audio-architecture.md` (the audio engine + inter-core channel),
`intercore-channel.md` / `transports.md` (the channel + its
transports), `trashfs-format.md` (the built-in filesystem on-disk
format), and `execution-model.md` (the planned cooperative/preemptive
RTOS configuration + tiered-memory design — a spec, not yet built).

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
| slist         | (none)                               |
| dlist         | (none)                               |
| tree          | (none)                               |
| fixed_point   | (none)                               |
| fast_div      | (none)                               |
| audio_mixer   | ring_buffer, fixed_point             |
| music_player  | audio_mixer (and its deps)           |
| audio_pool    | (none)                               |
| audio_pool_stream | audio_pool, music_player         |
| audio_arbiter | audio_pool                           |
| audio_fft     | (none — fixed-point, no libm)        |
| audio_sink    | (none — WAV dump; waveout needs -lwinmm) |
| audio_service | audio_mixer, audio_pool, audio_arbiter, music_player, service_channel |
| trashdrive    | (none)                               |
| trashdrive_fatfs | trashdrive, FatFs (third_party)   |
| trashfs       | (none)                               |
| bump          | slab_stack (only if using slab path) |
| slab_stack    | (none)                               |
| service_channel | spsc_ring                          |
| channel_thread | service_channel (POSIX/Cygwin: pthreads) |
| channel_win32 | service_channel (native Windows)     |
| vm_core       | (none)                               |
| vm_loader     | vm_core, bump                        |
| vm_ecall      | vm_core                              |
| vm_mailbox    | fifo_queue, ring_buffer              |
| vm_sched      | vm_core, vm_ecall                    |
| vm_system     | all of the above + slab_stack        |
| vm_host_stdio | vm_system (optional host bridge)     |
| vm_host_fs    | vm_system, vm_host_stdio, trashdrive_fatfs |
| vm_host_audio | vm_system, audio_service, service_channel |

So for example, to use `music_player`, copy and build:
`music_player.c`, `audio_mixer.c`, `ring_buffer.c`, and the
corresponding headers in `include/`.

## Building the host

The shell host (`examples/05_shell`) is the main interactive
deliverable. Its **target runtime is native Windows** — a
self-contained `.exe` that uses the Win32 paths (WinSock2,
`SetConsoleMode`) and does not depend on `cygwin1.dll`. Embedded
toolchains (STM32CubeIDE, ST-LINK, vendor flashers) live on native
Windows, so the dev host matches.

The compiler decides the target, not the shell you launch from, so
mingw-w64 always produces a native binary — even when invoked from
a Cygwin prompt. Two equivalent entry points produce the same
`build/host.exe`:

```powershell
# From PowerShell (or cmd):
.\build-win.ps1                 # native host.exe + guest ELFs
.\build-win.ps1 -NoGuest        # host only
.\build-win.ps1 -Clean
```

```sh
# From a Cygwin or MSYS2 shell:
./build-win.sh                  # native host.exe + guest ELFs
./build-win.sh --no-guest       # host only
./build-win.sh clean
```

Both default to `x86_64-w64-mingw32-gcc` and link `-lws2_32`.
Override the compilers with `-Cc`/`-GuestCc` (PowerShell) or
`CC=`/`GUEST_CC=` (bash).

The older per-example `build.sh` scripts use **Cygwin's own gcc**
on purpose: that binary links `cygwin1.dll`, exercises the POSIX
code paths, and is what the platform-neutral unit-test suites build
against for fast local iteration. Use `build.sh` for tests;
`build-win.*` for the shippable host.

## Building the tests

Tests live next to the code they cover, under `src/<module>/tests/`.
Build and run them all from the `garbage/` directory:

```sh
./run_tests.sh            # build + run every suite
./run_tests.sh audio      # only suites whose name matches "audio"
CC=gcc ./run_tests.sh     # override the compiler (default: cc)
```

`run_tests.sh` is the single source of truth for each suite's link
dependencies. It drops binaries + logs in `build/tests/` and exits
non-zero if any suite fails.

The canonical test compiler is Cygwin/POSIX `cc` (links `cygwin1.dll`,
exercises the POSIX host paths). A native mingw-w64 `cc` builds the
platform-neutral majority too: the runner detects the toolchain, adds
the Windows-only shim sources (`vm_host_stdio_win32.c`, the waveOut
backend) and **skips** the few POSIX-only suites (those using
`pipe`/`fsync`/`/tmp`) with a printed reason. FatFs suites and the
ELF-driven integration suites (`vm_real_elf`, `vm_host_stdio` — which
load guest ELFs from `examples/*/build/`) skip themselves when their
prerequisites are absent; build those examples first for full coverage.

To build one suite by hand, copy its dependency line from
`run_tests.sh`, e.g.:

```sh
cc -Wall -Wextra -Wpedantic -std=c11 -O2 -Iinclude \
   -o build/tests/audio_mixer src/audio/tests/test_audio_mixer.c \
   src/audio/audio_mixer.c src/containers/ring_buffer.c
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

#### slist / dlist

Linked lists over a caller-provided **node pool** (no malloc). You hand
the list a byte buffer; it carves nodes from it and keeps an internal
free list, rejecting inserts when the pool is exhausted — the same
fixed-capacity discipline as the rest of the library. Nodes are
addressed by 32-bit index, so the structure and the sizing math are
identical on 32- and 64-bit hosts. Both ship a compile-time size macro
(`SLIST_POOL_BYTES` / `DLIST_POOL_BYTES`, for static arrays and
`_Static_assert`) and a matching runtime function (`slist_pool_bytes` /
`dlist_pool_bytes`, for bump arenas) — they agree.

`slist` is the **lean** one: a single link per node (4 bytes + payload),
forward-only iteration, O(1) push/pop at the front and O(1) push at the
back (`pop_back` is O(n)). Use it where memory is tight — e.g. the
VM/guest side.

`dlist` is the **richer** one: two links per node, push/pop at both ends
all O(1), and both forward (`begin`/`next`) and backward
(`rbegin`/`prev`) iteration. Use it on the host or when you need those
operations.

API (slist): `slist_init`, `slist_clear`, `slist_push_front/back`,
`slist_pop_front/back`, `slist_front/back`, `slist_count/empty/full`,
`slist_begin/next/get`.
API (dlist): the same shape plus `dlist_rbegin`/`dlist_prev`.

#### tree

An **ordered binary tree** over a node pool, with a balancing discipline
chosen at init: `TREE_BST` (plain unbalanced — smallest per-insert work,
but O(n) worst case on sorted input) or `TREE_AVL` (height-balanced —
O(log n) guaranteed regardless of insertion order, no sorted-input
footgun). One instance uses one discipline for its life; you don't mix.

The key property: **the same walk works for every discipline.**
`tree_begin`/`tree_next` is one shared in-order traversal that yields
elements in sorted order whether the tree is a BST or an AVL —
`tree_rbegin`/`tree_prev` walk descending. `tree_find`/`tree_contains`
are likewise shared. So a developer learns one iteration idiom and one
lookup, and picks the insertion algorithm independently.

You supply a comparator at init; duplicate keys are rejected (set
semantics). Same node-pool sizing pair as the lists
(`TREE_POOL_BYTES` macro + `tree_pool_bytes()`), plus
`tree_pool_bytes_for_depth()` which returns the worst-case bytes to hold
*any* tree up to a given depth (a full tree — a bound, not exact).

API: `tree_init`, `tree_clear`, `tree_insert`, `tree_remove`,
`tree_find`, `tree_contains`, `tree_count/empty/full`,
`tree_begin/rbegin/next/prev/get`.

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

#### audio_pool

A block-based pool for sound effects held resident in RAM. Carves a
caller-provided region into fixed-size blocks; allocations are
refcounted handles, so a sample shared by several playing voices stays
alive until the last reference is released. Block-based (not
contiguous) so loading and unloading samples is cheap and
fragmentation-free — suited to a game that swaps effects in and out as
it changes levels. Designed to live in bulk memory (e.g. PSRAM) while
the mixer's working set stays in fast SRAM.

API: `audio_pool_init`, `audio_pool_alloc`, `audio_pool_retain`,
`audio_pool_release`, `audio_pool_sweep_vm` (reclaim everything one
owner allocated).

#### audio_pool_stream

Adapts a pooled (or any in-RAM) sample to the `music_player` stream
callback shape, so the same intro/loop machinery can play a
RAM-resident source, not just a file.

#### audio_arbiter

Places triggered voices onto a fixed set of mixer tracks. First-come
while tracks are free; tracks are owner-tagged so one VM's voices can
be swept when it exits. (A uniform integer-priority eviction policy is
specified in `docs/execution-model.md` for the multi-app configuration.)

API: `audio_arbiter_init`, `audio_arbiter_trigger`,
`audio_arbiter_stop`, `audio_arbiter_sweep_vm`.

#### audio_fft

A band meter over the mixed output: a 256-point FFT reduced to a small
number of log-spaced bands with 0–255 levels, for spectrum displays.
**Fixed-point, no libm** — a Q15 polynomial sine precomputes the
twiddle and Hann tables, the transform is integer radix-2, and the
level map is an integer log2. The capture is a cheap append on the
real-time render path; the (non-real-time) FFT update runs from the
service's process loop, never inside `mixer_render`.

API: `audio_fft_init`, `audio_fft_capture`, `audio_fft_update`,
`audio_fft_levels`.

#### audio_sink

The output-backend seam: a small vtable (`open`/`write`/`close`) that
lets the same rendered audio go to different destinations. Two backends
ship: a **WAV dumper** (hand-rolled 44-byte RIFF/WAVE PCM16 header,
zero dependencies) for writing renders to a `.wav` file, and a live
**Win32 waveOut** backend (`-lwinmm`, no other deps) whose blocking
write *is* the audio clock. This header also carries a small RIFF/WAVE
**parser** (`wav_parse`, `wav_to_mono_pcm16`) for loading drop-in `.wav`
assets.

API: `audio_sink_open`, `audio_sink_write`, `audio_sink_close`;
`wav_parse`, `wav_to_mono_pcm16`.

#### audio_service

Ties the mixer, pool, arbiter, FFT meter, and an output sink together
behind a `service_channel`, so it can run as a worker (a stand-in for a
dedicated audio core/coprocessor) while guest programs post
`SYS_AUDIO_*` requests to it over the channel. This is what the
`05_shell` host runs to give guests live sound. The render path stays
real-time; request handling and music streaming run from a separate
non-real-time pump.

API: `audio_service_create`, `audio_service_destroy`,
`audio_service_process`, `audio_service_render`, `audio_service_run`.

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
- `vm_host_install_fs`, `vm_host_install_fs_atexit`,
  `vm_host_fs_reset`, `VmDirent` (vm_host_fs)

#### vm_host_fs — file syscalls for guests

Exposes POSIX-shaped file operations to guest VMs via ECALL:

| Syscall      | # | What it does |
|--------------|---|--------------|
| SYS_OPENAT   | 56 | open or create a file/dir; returns fd |
| SYS_CLOSE    | 57 | close a fd |
| SYS_LSEEK    | 62 | seek within an open file |
| SYS_READ     | 63 | (also handles file fds when fs installed) |
| SYS_WRITE    | 64 | (same) |
| SYS_MKDIRAT  | 34 | create a directory |
| SYS_UNLINKAT | 35 | remove a file or empty directory |
| SYS_READDIR  | 120 | read one directory entry |

Syscall numbers match Linux's RISC-V generic ABI for compatibility
with stock libc wrappers (picolibc, newlib). The host backs them
with FatFs (via `trashdrive_fatfs`), giving the guest a real
read-write filesystem inside a RAM region.

The fd table holds up to `VM_HOST_FS_MAX_FILES` (default 16) open
files. fd 0/1/2 stay reserved for stdio (managed by
`vm_host_stdio`); file fds start at 3. The two modules cooperate
via a small setter hook so `read`/`write`/`close` work uniformly
across stdio and file fds.

Guests can use POSIX-ish paths starting with `/` — they get
rewritten to `0:/...` for FatFs's volume convention. Guests that
need to access multiple volumes can use the FatFs-native form
`<digit>:/...` directly.

The VM has no concept of current working directory. The *at-style
syscalls require `dirfd = AT_FDCWD (-100)` and interpret paths as
absolute. Adding chdir/getcwd support is straightforward (flip
`FF_FS_RPATH` to 2 in ffconf.h) but not done by default.

Build dependency: requires FatFs (see `third_party/fatfs/`). The
tests (`test_vm_host_fs.c`) compile in two modes — with or without
`-DHAVE_FATFS` — for the same reason as `test_trashdrive_fatfs.c`.

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
