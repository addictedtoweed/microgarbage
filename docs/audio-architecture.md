# Audio architecture — design note (draft)

How VM apps produce audio on the SNES_HX_420 cartridge (STM32H745)
and on the desktop host. This locks the design before building.
Companion to `intercore-channel.md` (the M7↔M4 / main↔worker
transport this rides on).

Status: **largely implemented on the desktop host** (the M4-side
service runs on a worker thread; the STM32 SD diskio + PSRAM DMA copy
seam remain hardware follow-ups). The DSP core (mixer + music player)
plus the multi-VM service, block pool, arbiter, and FFT meter exist
and are tested under `src/audio/`. See "Implementation status" below
for what concretely landed vs. what's still design intent.

## Implementation status (updated 2026-05)

Built and tested (desktop, native-Windows + Cygwin/POSIX):

- **Service / channel / pool / arbiter / mixer / music player** — the
  full path in `audio_service.c`, behind the service channel.
- **Streaming long WAVs** — `audio_file_stream.{c,h}` is a
  `music_stream_fn` source over a platform `AudioFileReader` vtable
  (open/read/seek/close); `SYS_AUDIO_STREAM_WAV` builds a looping
  file-stream voice that reads incrementally, so a song far larger than
  the pool plays without loading into it. Desktop binds stdio (`/host`)
  + FatFs (`/td0`); the H745 binds FatFs over SD — same source, only
  the reader differs. (Source-rate≠output-rate resampling is still a
  TODO — author WAVs at the output rate, 44.1 kHz.)
- **Full-stereo mixer** — every channel is `PCM16_STEREO`. Stereo is
  preserved; mono sources are promoted to L==R (no downmix) in the SFX
  feed, the pool→player adapter (`audio_pool_stream`, promote flag),
  and `wav_to_stereo_pcm16`. Samples still rest in the pool as mono16,
  so this added no PSRAM cost.
- **FFT band meter** — enable is **refcounted** across consumers (one
  app disabling it doesn't blank another's equalizer); a VM's hold is
  released on sweep. It captures the whole mix.
- **Music feeder back-pressure** — `music_update` pumps only the mixer
  channel's free space (`mixer_channel_capacity`), so the source
  advances at the render/drain rate rather than racing ahead.
- **Cross-VM** — one shared service; spawn is asynchronous, so multiple
  sessions' apps run concurrently and mix through the one mixer.

Still design intent / not done: priority admission/eviction + mute
states (the arbiter is FCFS reject-on-full today; `priority` reserved),
the STM32 SD diskio driver, and the PSRAM↔SRAM DMA copy seam.

The rest of this note is the original design rationale.

## Output target

CD-quality **44.1 kHz, 16-bit, stereo**, line-level out via the
cartridge's exposed audio pins → PCM5102A DAC. The SNES APU is
**bypassed entirely**; the SNES only manages bus DMA, mailbox/joypad,
coprocessor-release handshake, and vblank timing. Audio is wholly the
cartridge's job.

## Two memories, two jobs (the core split)

1. **PSRAM (8 MB QSPI) — the block pool.** Sample data *at rest*
   (songs, VA streams, SFX). Block-based, fragmentation-free, handle-
   based. **8 KB blocks** (see "Block sizing"). 8 MB / 8 KB = 1024
   blocks → 128-byte free bitmap + ~2 KB out-of-band owner array.
   This is the NEW allocator.

2. **Internal SRAM — staging buffers.** Small, fast, contiguous,
   allocated once at voice setup. The mixer channel rings, the music
   player's streaming buffer, and the pinned heads live here. These
   are NOT block-allocated (they don't fragment — allocated at
   startup, reused). The existing mixer/player already manage these.

Data flow:
```
  PSRAM block(s)  --segmented QSPI DMA-->  SRAM staging (contiguous)
                                                  |
                                          mixer reads/resamples
                                                  |
                                          contiguous output ring
                                                  |
                                          SAI/DMA --> PCM5102A
```
Non-contiguity lives ONLY in PSRAM-at-rest. The moment data reaches
SRAM staging it is contiguous, so the mixer and the SAI/DMA never
touch non-contiguous memory. This is why the existing (contiguous-
buffer) mixer/music_player need no changes — the new pool plugs in
*underneath* them as a stream source.

## Block sizing (8 KB) — rationale

The PSRAM→SRAM staging transfer copies "a frame or two of playback"
into a staging buffer. A staging transfer cannot cross a block
boundary in one QSPI transaction (non-contiguous blocks → separate
transactions), so the block must be >= a comfortable staging
transfer to keep a typical fill to one transaction.

  - 44.1 kHz 16-bit stereo = ~2.9 KB per NTSC video-frame; two frames
    ~5.9 KB.
  - 8 KB block holds a two-frame fill in one contiguous run → one
    QSPI transaction per fill in the common case.

Note on the QSPI cost: the per-transaction QSPI *hardware* setup
(command/address/dummy ~0.25-0.5 us) amortizes to ~1% even at 2 KB,
so the QSPI floor is NOT the binding constraint at audio sizes. The
real driver of "bigger is better" is **fewer transactions per frame**
(less DMA-descriptor/interrupt/bookkeeping overhead, fewer boundary
crossings). 8 KB is the sweet spot; 16 KB if even fewer transactions
are wanted, at the cost of more waste on small SFX. (Exact QSPI
amortization pending the PSRAM part's clock + dummy-cycle count.)

## The DSP core (already built — do not rebuild)

`audio_mixer` (src/audio/audio_mixer.c):
  - N input channels, per-channel format (8u/8s/16s, mono/stereo),
    per-channel source rate + linear/cubic interpolation, volume, pan,
    looping, mute, headroom saturation. Startup-only allocation.
  - **v2 drift correction** against an external clock with low-pass
    smoothing — already documented for the SNES 21.477 MHz master
    clock (`mixer_observe_sync`, `MixerSyncConfig`). The frame-sync
    fractional resampling is THIS, already implemented.
  - `mixer_render` writes a **contiguous** output buffer → feeds the
    SAI DMA ring directly. (Resolves the "DMA can't chase blocks"
    hazard: the mixer output is always contiguous.)

`music_player` (src/audio/music_player.c):
  - Intro+loop with **sample-accurate, gap-free** transition (two-file
    model, mastered to meet cleanly at the seam).
  - **Pinned heads** for intro and loop → instantaneous start/restart
    regardless of SD latency. (This is the "primed head".)
  - Tiered: deep app streaming_buffer → small mixer channel ring →
    output. `music_update` (non-RT pump) vs `mixer_render` (RT) split.
  - Pulls source via a `stream_fn(user_data, stream_id, dst, n,
    offset)` callback — source-agnostic (SD, PSRAM, memory).

The pool plugs in as a `stream_fn` source: "play from sound-RAM
handle X" = a stream callback that reads from the PSRAM block pool.

## Capacity target

- Up to **3 SD-streamed channels** (music / VA), deep buffers.
- Up to **16 total input tracks** (the 3 streams + ~13 fast SFX from
  the PSRAM pool).

Sizing both platforms:
  - **SRAM staging:** ~3 streamed x 16-32 KB + ~13 SFX x 1-3 KB ≈
    65-135 KB. H745 has ~1 MB SRAM — comfortable.
  - **M4 mixing/frame:** with the mixer's recommended interp choice
    (cubic for music/voice, linear for SFX): 3x cubic + 13x linear
    ≈ ~72 K cycles/video-frame ≈ ~2% of the M4's 4 M cycles/frame.
    Even all-16-cubic is ~8-10%. 16 tracks is comfortable, not bold.
  - The binding risk at 16 is the **3 SD streams** (independent read
    positions → potential card seek thrashing), not channel count.
    Mitigated by deep streaming buffers + the music player's built-in
    loop prefetch.

## Handle model (two distinct handle kinds)

Audio is a **shared global service** — one mixer, one output, one
track pool. Any VM can load samples and play them; the mixer sums all
into the single output. Handles are system-wide (cross-VM shareable).

Two handle kinds, deliberately distinct (the OpenGL texture-vs-
texture-unit distinction):

1. **Object handle** — durable, cheap, pool-bounded. A loaded sample
   or a music object (intro+loop pair). A VM may hold **as many as it
   wants**; the only limit is PSRAM pool capacity. Holding an object
   consumes pool blocks, NOT a mixer track.

2. **Voice handle** — transient, scarce, track-bounded. Returned when
   an object is *played*. Valid until that playback stops. There are
   only N (16) tracks.

So: `LOAD_SAMPLE/LOAD_MUSIC -> object` (unbounded count), then
`TRIGGER_SFX(object,...)/PLAY_MUSIC(object) -> voice | REJECTED`. The
same object can be triggered many times, each attempt getting (or
being denied) its own voice.

## Track arbitration

N (16) mixer tracks. Playing claims a free track; **first-come-first-
serve, reject-on-full** — when all tracks are busy, a new play/
trigger returns REJECTED (the guest reacts or ignores; the new sound
simply doesn't play). A track frees when its stream stops/finishes/
is explicitly stopped.

NOT evict-oldest (cutting a song for an SFX is worse than silence) and
NOT queue (late audio is worse than no audio). **Priority eviction is
deferred** to a future refinement — it would change *which* track a
request gets / whether a high-priority request may evict a low one,
without changing the object/track split. Each track records its
owning VM so a dying VM's tracks are stopped + freed.

## Cross-VM shared handles → refcounting (NOT locking)

Because object handles are shareable across VMs (VM A loads a sample,
VM B may also trigger it), object lifetime is a **reference-count**
problem, not a lock problem:

  - Each pool object carries a refcount: how many VMs/voices reference
    it. Freed only when the count reaches zero.
  - `FREE(object)` from a VM decrements; the object stays alive if
    another VM holds it or a voice is still playing it.
  - A **dying VM drops its references** (orphan handling) —
    decrementing refcounts, not hard-freeing blocks another VM may
    still reference.

The refcount lives in the service and is mutated only by serialized
request processing, so it is not itself racy (see thread-safety).

## Thread-safety (structural, not lock-based)

Three relationships, each handled by structure that holds identically
on desktop and on the H745:

1. **VM vs VM:** NOT concurrent. The scheduler is cooperative round-
   robin on one host thread — VMs time-slice, never execute
   simultaneously. So no two VMs post an audio request at the same
   instant. **No lock needed across VMs.**

2. **VM (requester) vs audio service (provider):** the only genuine
   concurrency (M7 vs M4 / main vs worker). Synchronized by the
   **SPSC channel ring** (acquire/release ordering) — post a message,
   never share mutable state across the boundary. The ring IS the
   thread-safety; no API lock.

3. **Service request-handling vs render:** both on the service side.
   The service drains pending requests at a safe point in its render
   cycle (between buffer fills) — internal discipline, not a VM-facing
   lock.

**Invariant to document loudly:** the request ring is single-producer
*because the cooperative scheduler guarantees one VM posts at a time*.
This is correct today and costs nothing. The ONE thing that would
require revisiting it: if VMs ever become genuinely concurrent
(preemptive, multi-threaded scheduler, or VMs split across M7/M4) —
then the shared request ring needs a lock around `post` or an MPSC
ring (or per-VM channels). Noted as a tripwire, not built now.

## The M4 seam (port path)

The M4 side never understands VMs, scheduling, or M7 state. It
implements one thing: drain the request ring, do the work, post
responses. All inputs arrive as self-contained messages (handles /
sizes / offsets — never live M7 pointers). So porting = implement
`channel_intercore.c` (shared-SRAM + HSEM transport per the channel
note); the entire audio service code that ran on the desktop worker
thread runs UNCHANGED on the M4. Swap the transport, not the service.

Hardware-only at bring-up (the desktop can't model these): the cache
discipline for the shared-SRAM ring (non-cacheable MPU region, or
clean/invalidate) and confirming `mixer_render` meets its DMA-fill
deadline. The logic is proven on desktop first.

## Audio message types (over the channel; namespace 0x01xx)

  - `REQ_AUDIO_POOL_ALLOC` (size) -> object_handle | ENOSPC
  - `REQ_AUDIO_POOL_FREE` (object_handle)         [refcount decrement]
  - `REQ_AUDIO_LOAD_SAMPLE` (src) -> object_handle
  - `REQ_AUDIO_LOAD_MUSIC` (intro_src, loop_src) -> object_handle
  - `REQ_AUDIO_TRIGGER_SFX` (object, gain, pan)  -> voice | REJECTED
  - `REQ_AUDIO_PLAY_MUSIC` (object, flags)       -> voice | REJECTED
  - `REQ_AUDIO_VOICE_STOP` (voice)
  - `REQ_AUDIO_STOP_MUSIC` (object_or_voice)

Big payloads (PCM to load) pass by shared-buffer handle + offset +
length, never inline in the message.

## Build / test plan (desktop-first, like trashfs)

1. **SPSC ring** — lock-free single-producer/single-consumer ring,
   C11 acquire/release. Unit tests + a producer/consumer thread-pair
   stress test under ThreadSanitizer.
2. **ServiceChannel + thread backend** — duplex pair, post/poll/
   notify/wait, worker thread. Echo-service round-trip test.
3. **Audio-pool allocator** — 8 KB-block PSRAM pool, out-of-band owner
   array, refcounted object handles, fragmentation-free alloc/free,
   orphan sweep. Pure desktop-testable C (like trashfs Phase 1).
4. **Pool-as-stream_fn adapter** — feed the existing music player /
   mixer from a pool object handle.
5. **Track arbitration** — N-track pool, two-handle model, FCFS
   reject-on-full, per-track VM ownership + orphan cleanup.
6. **VM syscall seam** — guest audio ecalls -> channel posts ->
   service; wire audio into the host build.
7. **(MCU bring-up)** `channel_intercore.c` — shared-SRAM + HSEM +
   cache discipline. Only genuinely new hardware code.

## Open questions for sign-off

1. **Block 8 KB** vs 16 KB (fewer transactions, more SFX waste) — 8 KB
   recommended; revisit if SFX density is high or PSRAM part is slow.
2. **Track count 16** confirmed (comfortable per the budget above);
   3 of those reserved/typed as SD-stream-capable (deep buffers)?
3. **Refcounted shared objects** — confirm the lifetime model: object
   alive while any VM or voice references it, freed at last drop, VM
   death drops its refs.
4. **Pool ownership on the micro** — pool + allocator resident on the
   M4 (M7 requests handles across the channel), as the M4 owns the
   audio subsystem + PSRAM staging DMA? (Allocator code is the same
   desktop-testable C either way; this fixes which side runs it.)
