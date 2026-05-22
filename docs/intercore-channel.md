# Inter-core service channel — design note (draft)

A message protocol and transport abstraction for the VM (the
"requester") to reach native co-processor services (the "provider").
On the STM32H745 the requester is the **M7** (running the VM + game
logic) and the provider is the **M4** (running audio, file access,
bitmap→bitplane, FMV, USB-HS, PPU-interface tools). On the desktop
host the requester is the **main thread** and the provider is a
**worker thread** — same protocol, different transport.

Status: **design draft, not yet implemented.** Format-first: this
locks the message protocol and the transport contract before any
concurrent code is written, because concurrency bugs are far cheaper
to design out than to debug.

## Why an explicit channel (not inline calls)

Today the VM's services (filesystem, etc.) run inline on the same
core as a normal ecall handler. On the dual-core target, audio / SD /
media run on the **M4**, so a guest's `play_music` or file read must
cross to another execution context. The channel makes that boundary
explicit and gives it one contract that both platforms implement:

  - **Desktop:** queue in process heap, provider is a worker thread,
    wakeup via condition variable / semaphore.
  - **H745:** queue in shared SRAM, provider is the M4, wakeup via
    HSEM (hardware semaphore) + inter-core interrupt.

The VM-side and service-side code are written against the channel
contract; only the transport differs. This mirrors two abstractions
already in the codebase — the `VmHostTransport` vtable and the
`host_platform` HAL — a third "swap the backend per platform" seam.

## Relationship to existing primitives

- **vm_mailbox** (inter-*VM* messaging) is the conceptual cousin but
  NOT reused directly: it's same-core, has VM-id whitelists, and
  isn't built for cross-core memory ordering. The inter-core channel
  is a lower-level, two-endpoint, ordering-correct primitive.
- **ring_buffer** is single-context only — it tracks fullness via a
  shared `count` field that both push and pop mutate, which races
  under true concurrency. The channel needs an **SPSC (single-
  producer/single-consumer) ring** that derives full/empty from head
  and tail alone, with memory barriers on index publication. This is
  new code, not a reuse of ring_buffer (though it can borrow its
  shape).

## Channel structure

Two SPSC rings per channel — a **full-duplex pair**:

```
  requester (M7 / main thread)            provider (M4 / worker)
        |                                         |
        |  request ring  (req producer) --------> (req consumer)
        |                                         |  does the work
        |  response ring (resp consumer) <------- (resp producer)
        v                                         v
```

- **Request ring:** requester writes request messages, provider reads
  them. Requester is the sole producer; provider the sole consumer.
- **Response ring:** provider writes responses; requester reads them.
  Roles reversed — still exactly one producer, one consumer.

SPSC on each ring independently means **no locks** are needed for the
ring mechanics: each index has exactly one writer. Correctness rests
on memory ordering (below), not mutual exclusion.

## Message format

Fixed-size message slots (variable-size payloads go through shared
buffers referenced by handle/offset, not inline — keeps the ring
simple and the slot size bounded). Proposed 32-byte message:

```
offset size  field
0      2     type        REQ_* / RESP_* opcode
2      2     flags       bit0 = expects_response; bit1 = async
4      4     seq         request sequence number (matches response)
8      4     a0          arg / handle
12     4     a1          arg / size / offset
16     4     a2          arg
20     4     a3          arg / result code (in responses)
24     4     a4          arg / returned handle (in responses)
28     4     reserved
```

`seq` lets the requester correlate a response with its request (so
multiple in-flight requests are unambiguous). Payloads larger than
the inline args (e.g. a block of PCM, a file read buffer) are passed
by **shared-buffer handle + offset + length** in the args — the data
lives in shared SRAM (H745) or shared heap (desktop), never copied
through the ring.

## Message types (initial set)

Grouped by service. Each is a request; most have a matching response.

**Audio** (M4 audio subsystem):
  - `REQ_AUDIO_POOL_ALLOC` (size) → handle | ENOSPC
  - `REQ_AUDIO_POOL_FREE` (handle)
  - `REQ_AUDIO_LOAD_MUSIC` (intro_src, loop_src) → music_handle
  - `REQ_AUDIO_PLAY_MUSIC` (music_handle, flags)
  - `REQ_AUDIO_STOP_MUSIC` (music_handle)
  - `REQ_AUDIO_TRIGGER_SFX` (sample_handle, gain, pan, priority)
        → voice_handle | REJECTED (track limit)
  - `REQ_AUDIO_VOICE_STOP` (voice_handle)

**File** (M4 transparently owns SD/FatFs):
  - `REQ_FILE_OPEN` (path_buf, flags) → fd | -errno
  - `REQ_FILE_READ` (fd, buf, n) → bytes | -errno
  - `REQ_FILE_WRITE` (fd, buf, n) → bytes | -errno
  - `REQ_FILE_CLOSE` (fd)
  - (trashfs RAM disk stays M7-local — see "Filesystem split")

**Media** (later — placeholders so the protocol reserves space):
  - `REQ_GFX_BITMAP_TO_BITPLANE` (src, dst, format)
  - `REQ_FMV_*`, `REQ_PPU_*`, `REQ_USB_*`

Opcodes are namespaced by high byte (0x01xx audio, 0x02xx file,
0x03xx gfx, ...) so services can be added without renumbering.

## Sync vs async semantics

Two calling patterns, chosen per request via the `flags` bit:

- **Async (fire-and-forget or poll-later):** requester posts the
  request and continues. For `play_music`/`trigger_sfx` this is the
  norm — the M7 never blocks on audio. If a result is needed, the
  requester polls the response ring later, matching by `seq`.
- **Sync (post-and-wait):** requester posts, then blocks until the
  matching response arrives. Used where the guest genuinely needs the
  result before continuing (e.g. a file read whose bytes it's about
  to use). On the H745 the M7 "blocks" cooperatively — it can spin on
  the response ring, or yield to the scheduler and resume when the
  HSEM signals. **No request blocks the M4**; only the requester ever
  waits, and only when it opts in.

The frame-release model fits this perfectly: the M7 posts requests,
does its ~2M-cycle game-logic window, and the M4 services requests in
parallel. Sync requests that can't complete within the window are
designed to be rare (most file I/O is prefetched, most audio is
async).

## Memory ordering — the correctness crux

This is where a desktop thread model and the H745 differ, and where
the only real concurrency hazard lives.

The SPSC ring is correct iff: the producer **writes the slot payload
before publishing the new head index**, and the consumer **reads the
head index before reading the slot payload** — with a barrier between
each pair so the writes/reads can't be reordered across the index
publication.

- **Desktop:** C11 atomics with acquire/release ordering on the
  head/tail indices (`atomic_store_explicit(..., release)` /
  `atomic_load_explicit(..., acquire)`). On coherent desktop CPUs the
  hardware does most of the work; the atomics stop the *compiler*
  reordering.
- **H745:** acquire/release plus, critically, **cache maintenance** —
  the M7 and M4 have separate caches over shared SRAM. Either the
  channel memory is placed in a **non-cacheable MPU region** (simplest
  and recommended for the ring control structures), or the producer
  does a cache-clean after writing and the consumer a cache-invalidate
  before reading. Getting this wrong yields stale reads that **the
  desktop model will NOT reproduce** (see fidelity caveats).

## Transport abstraction (the swappable backend)

```
typedef struct {
    /* Post a message into the ring this endpoint produces into.
     * Returns 0, or -1 if the ring is full (caller retries/backs off). */
    int  (*post)(void *ctx, const ChannelMsg *msg);

    /* Try to pop a message from the ring this endpoint consumes.
     * Returns 1 if a message was produced into *out, 0 if empty. */
    int  (*poll)(void *ctx, ChannelMsg *out);

    /* Signal the peer that a message is waiting (condvar wake / HSEM).
     * May be a no-op if the peer polls. */
    void (*notify)(void *ctx);

    /* Block until a message is available or timeout (sync waits).
     * Desktop: condvar wait. H745: WFE on HSEM, or cooperative spin. */
    int  (*wait)(void *ctx, uint32_t timeout_ms);

    void *ctx;
} ServiceChannel;
```

Backends:
  - `channel_thread.c` — desktop: heap rings + C11 atomics + a condvar
    for `wait`/`notify`. The provider is a `pthread`/`std::thread`
    worker draining the request ring and running the real services.
  - `channel_intercore.c` — H745: shared-SRAM rings + HSEM + the cache
    discipline above. (Built during MCU bring-up.)

The VM-side syscall handlers and the service implementations both
talk only to `ServiceChannel`. Same code, both platforms.

## Filesystem split (consequence for the existing fs)

On the H745 the M4 owns SD/FatFs, so the M7's `vm_host_fs` FatFs
backend becomes a **proxy** that forwards open/read/write/close as
`REQ_FILE_*` over the channel. The **trashfs RAM disk stays M7-local**
(it's just memory the M7 can touch directly — no benefit to crossing
cores). So the fs seam gains a third routing case on the MCU:
  - host passthrough → desktop only
  - trashfs → M7-local (direct, as today)
  - FatFs/SD → M4 via the channel (proxy)
On the desktop, everything stays inline (the worker thread is optional
for SD; the channel is mainly exercised for audio). This is a future
change to vm_host_fs, noted here so the channel protocol reserves the
`REQ_FILE_*` space now.

## Desktop-model fidelity (honest caveats)

The thread-backed desktop channel faithfully models:
  - the message protocol and serialization,
  - the async/concurrent service behavior (real worker thread),
  - the handle/ownership lifecycle across contexts,
  - the non-blocking discipline (does the main thread keep running?),
  - data races in the protocol logic (run tests under ThreadSanitizer).

It does **NOT** model:
  - **cache coherence** between M7/M4 over shared SRAM — desktop CPUs
    are coherent; the MPU/cache-maintenance correctness is hardware-
    only and must be validated during bring-up.
  - **hard real-time deadlines** — desktop threads are OS-scheduled;
    meeting the audio DMA-fill deadline is hardware-only.
  - **true lockstep parallelism timing** — the OS scheduler may hide
    ordering bugs the two real cores would expose (ThreadSanitizer
    claws most of this back).

So the desktop model validates **protocol + logic + concurrency
correctness** (most of the design risk), and defers **cache
maintenance + timing** to hardware bring-up. A good trade: the
expensive-to-debug logic bugs are caught cheaply.

## Build / test plan (desktop-first, like trashfs)

1. **SPSC ring** — the lock-free single-producer/single-consumer ring
   with C11 acquire/release. Pure, unit-testable (single-thread tests
   for mechanics; a stress test with a real producer/consumer thread
   pair under ThreadSanitizer for ordering).
2. **ServiceChannel + thread backend** — the duplex pair, post/poll/
   notify/wait, a worker thread. Test with a trivial echo service
   (REQ → RESP round-trip, seq matching, sync and async paths).
3. **First real service over the channel** — the audio-pool allocator
   (REQ_AUDIO_POOL_ALLOC/FREE), proving the protocol with real work.
4. **Layer remaining audio services** (load/play/trigger), then the
   VM syscall seam (guest → ecall → channel post).
5. **(MCU bring-up)** `channel_intercore.c` — shared-SRAM + HSEM +
   cache discipline. The only genuinely new code on hardware; every-
   thing above is already proven on the desktop.

## Open questions for sign-off

1. **Message slot size** — 32 bytes (8 × uint32) enough for all inline
   args, with big payloads by shared-buffer handle? Or do any requests
   want more inline?
2. **Ring depth** — how many in-flight requests per direction? (Audio
   is bursty at scene starts; file I/O is steady.) A depth of 16–32
   per ring is cheap; deeper costs shared SRAM.
3. **Sync-wait policy on the M7** — cooperative spin on the response
   ring, or yield-to-scheduler-and-resume-on-HSEM? (Affects how a
   blocking file read interacts with the frame-release window.)
4. **One channel or several?** — a single multiplexed channel for all
   services (simpler, one queue), or separate channels per service
   class (audio vs file vs media — isolates a flood of SFX requests
   from blocking file I/O)? Leaning multiplexed to start, split later
   if contention shows up.
```
