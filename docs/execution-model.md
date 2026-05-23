# Execution model & memory configuration

> **Status: DESIGN — for review. No implementation yet.**
> This locks the concepts before any headers or code, the same way
> `audio-architecture.md` was locked before the audio build. Correct
> the ideas here first; interfaces and code follow only after review.

This document defines how the VM and its native services are scheduled
and how the allocator's footprint is configured, across two deployment
shapes selected at compile time in `config.h`:

(the two configs are **cooperative** and **preemptive**, defined in the
bullets just below — both are multi-task; the axis is scheduling
discipline, not task count)
- **cooperative** — multiple apps, each its own task/thread, scheduled
  **cooperatively** (they yield at calls, waits, explicit yield points,
  and via the interpreter's periodic auto-yield, §3.1). No systick
  preemption. Lower footprint; no preemption machinery. The accessible
  default. Builds first (closest to today + the auto-yield safety valve).
- **preemptive** — adds **systick time-slicing** so no task can
  monopolize the core even without yielding, and high-priority work
  (audio) preempts promptly. Real preemption (real systick on the micro,
  *simulated* via a timer thread on Windows). The opt-in ceiling, for
  untrusted/compute-heavy tasks or hard latency. The larger, iterative
  effort.

**Both are multi-task** — multiple apps run in their own threads in
either config (this is what lets `/builtin` tools, an editor, and the
dev's app coexist; see §1). Both have native services (audio) on their
own device-clock threads. The *only* difference is whether the scheduler
**preempts** (systick) or relies on **yielding** (cooperative).

Everything else — the VM, the services, the slab allocator, the
service channel — is shared between the two. Only the scheduling
discipline and provisioning differ.

---

## 1. Why two configurations

Even the "I just want to run my app, no porting" path is **multi-task**:
the app runs as a task in its own thread, and the system can spin up
*other* programs beside it — builtins from a read-only flash-resident
`/builtin` (a text editor, eventually a curated busybox-style embedded
toolset), each as its own task. So running several small programs is the
*normal* case, not an advanced opt-in. The developer still does nothing
special: their app is a task, other tasks coexist, and they don't manage
any of it.

What that developer gets by default is the **cooperative** config:
tasks yield at external calls (they're calling out anyway), at I/O
waits, and — the safety valve — via the **interpreter's periodic
auto-yield** (we control the VM step loop, so it yields every N
instructions even if the guest never calls out; §3.1). That makes
cooperative scheduling robust: a compute-bound guest still yields,
because the interpreter makes it. So no app fully monopolises the core
*even cooperatively*, without any `SuspendThread` machinery.

The remaining gap cooperative can't close — and the reason **preemptive**
exists — is twofold: the interpreter can't yield *inside* a native
passthrough (a long `memcpy` runs to completion natively; the auto-yield
is between VM instructions, not inside native code), and cooperative
gives no *hard* latency guarantee. A developer who needs to preempt a
runaway native call, or who has a hard real-time deadline, opts into the
preemptive config and its systick time-slicing. For many users the
cooperative-with-auto-yield config is enough; preemption is the
specialist add-on.

On a micro the unused config's code and RAM are **stripped**, not merely
disabled — a cooperative-only build carries no systick/preemption code,
and a hard-real-time hot-path core (e.g. an M7 emulating a cartridge bus)
includes only what it needs.

```c
/* config.h */
#define GARBAGE_SCHED_COOPERATIVE  0
#define GARBAGE_SCHED_PREEMPTIVE   1

#ifndef GARBAGE_SCHED_MODE
#define GARBAGE_SCHED_MODE  GARBAGE_SCHED_COOPERATIVE  /* accessible default */
#endif
```

---

## 2. The execution-model seam

Both configurations sit behind one interface so the VM and services are
written once. The interface has two halves: the **task scheduler**
(manages VM tasks) and the **native-service category** (clock-driven
threads beside the scheduler). They are deliberately separate — a
service is *not* a task.

### 2.1 Tasks (the scheduled things)

A **task** is a VM instance the scheduler owns. Lifecycle:
create → run → (yield | suspend | resume) → exit. In **cooperative**
mode tasks are not time-sliced — they run until they yield (at calls,
waits, yield points, or the interpreter's auto-yield), and `suspend`/
`resume` aren't used for time-slicing. In **preemptive** mode tasks are
preemptible and priority-ordered, suspended/resumed by the systick.
Both modes have *multiple* tasks; the difference is whether the
scheduler forces switches (preemptive) or waits for yields
(cooperative).

```c
/* Sketch — not final. */
typedef struct Task Task;
typedef int (*task_entry_fn)(void *arg);

typedef enum { TASK_PRIO_LOW, TASK_PRIO_NORMAL, TASK_PRIO_HIGH } TaskPriority;

/* Implemented by each backend (see §3). */
typedef struct {
    const char *name;
    bool  (*start)(void);                 /* bring the scheduler up      */
    Task *(*create)(task_entry_fn, void *arg, TaskPriority);
    void  (*yield)(void);                  /* cooperative yield point     */
    void  (*suspend)(Task *);              /* preemptive only             */
    void  (*resume)(Task *);               /* preemptive only             */
    void  (*run)(void);                    /* run until all tasks exit    */
    void  (*stop)(void);
} SchedulerBackend;
```

In cooperative mode `suspend`/`resume` aren't used for time-slicing and
`yield` is the cooperative hand-off; in preemptive mode all are live. The
backend seam means the VM never calls a platform thread API directly.

### 2.2 Native services (the clock-driven things)

A **native service** is *not* a task. It is a clock/event-driven entity
that runs **outside** the task scheduler, coupled to the VM world only
through a **channel** (the existing SPSC `ServiceChannel`). Audio is the
first and currently only member. Graphics and a SNES PPU emulator are
anticipated future peers; this document fixes the *shape* so they slot
in as peers, but **no code exists for them**.

Two properties classify a service:

- **clock / wake source** — what paces it. Audio: the output device's
  buffer-drain (DMA-complete on the micro, waveOut buffer-free on
  Windows — the device *is* the DMA engine there). A future PPU:
  vblank / vsync.
- **pacer vs sink** —
  - a **sink** consumes VM output (audio mostly: the VM pushes audio
    commands; the service mixes and drives the device);
  - a **pacer** drives the VM's cadence (a PPU: vblank pokes the VM
    "prepare the next frame", the VM produces state, hands it over the
    channel; the service enforces the per-frame DMA budget on consume).
  A service may be both. The channel is already request/response
  bidirectional, so a pacer's "release for data prep" signal toward the
  VM is expressible today; the seam must simply not assume services are
  pure consumers.

In **both** configurations a service runs on its **own thread**, paced
by its clock — there is no "single loop" to pump it from, because both
configs are multi-task (apps each run in their own thread; see §1). The
difference between configs is only *scheduling discipline*:

- **preemptive** — the systick can preempt app threads so the audio
  service (or any high-priority work) runs promptly; the service thread
  is **never** time-sliced and **never** suspended by the systick (§3.2 —
  the systick manages only app/task threads, never service threads).
- **cooperative** — no systick; the audio service thread still exists
  and still blocks on its device + wakes on buffer-drain, but it relies
  on app threads yielding (at calls/waits/yield points, plus the
  interpreter's periodic auto-yield, §3.1) so it gets the core when the
  device needs data. A pathologically non-yielding app thread *could*
  starve it — which is precisely the hazard preemption removes.

So audio is a device-clock service thread regardless of config; only
whether app threads can monopolize the core differs. (Audio is never "pumped from the app loop" —
there is no single loop; apps are always threads, so audio is always its
own service thread.)

### 2.3 Invariants that keep concurrency sound

Multitasking introduces *real* concurrency where the cooperative era had
structural non-concurrency ("VMs never ran at once"). These invariants
replace that structural guarantee with explicit rules:

1. **Services couple to VMs only through channels.** A service never
   reaches into VM memory or shared structures directly; all data
   crosses via the channel + its staging buffer (the host audio
   handlers already do exactly this — translate guest pointer, copy to
   staging, hand off). This bounds the concurrency-audit surface: each
   new service is safe by the same argument regardless of how many tasks
   preempt each other.
2. **Preemptible paths are slab-allocator-only and CRT-free.** See §3.2
   and §4.5 — the `SuspendThread` simulation can freeze a task mid-call,
   so a task must never hold a lock the rest of the system needs. Using
   our own allocator (whose locking we control) instead of the C
   runtime's is what makes preemption safe. Native `malloc` is allowed
   **only once at startup** for provisioning (§4.3), before any
   preemption is live.
3. **The systick manages only task threads.** Service threads are
   excluded from the suspend/resume set, so audio is never frozen
   mid-mix.

### 2.4 Audio: per-app independent access (the worked example)

Audio is the concrete instance of a native service, and multi-app use
pins down how *any* service is shared. Every app accesses audio
independently — loads/removes its own sound effects, plays/stops its own
streams, triggers its own voices — without coordinating with other apps.
The mixer blends everyone's output into the one device.

**Channel-per-app.** The original channel was SPSC (single-producer):
fine when VMs never ran concurrently, but with apps as concurrent
threads each calling audio, that single-producer assumption breaks. The
fix preserves the lock-free design: **each app task gets its own
SPSC `ServiceChannel` to the audio service**, created at task spawn and
torn down at task exit. Each app is still the single producer of *its*
channel (lock-free intact); the service drains all app channels
(round-robin) in its process loop. Responses route back on the
requesting app's own channel — no cross-talk, cleaner than a shared
channel would be. This is the one genuinely new structural piece for
multi-app audio; the pool, arbiter, and music player below are unchanged.

**Three resource classes, three scarcities, three rejection points** —
all owner-tagged to the requesting app, all reclaimed on app exit (or ELF
swap-out, when that ends the task), all `config.h`-configurable and
feeding the footprint math (§4). Voices and streams are priority-
arbitrated (uniform integer priority, below); resident-SFX *loading* is
bounded purely by pool RAM:

| resource | scarcity / limit | when full | config knob |
|---|---|---|---|
| **resident SFX** | audio-pool block RAM | `load` fails (no priority — RAM is RAM) | pool size (block count) |
| **playing voices** | mixer track count | strict-greater priority evicts lowest, else `trigger` rejects | track count (default 16) |
| **filesystem music streams** | global stream budget across *all* apps | strict-greater priority evicts lowest, else request errors | max streams (default 3) |

- **Resident SFX** — the audio pool (block-based, non-contiguous, 8 KB
  blocks, refcounted, out-of-band metadata) is exactly the cheap
  add/remove churn an ELF-swapping game needs. No count cap beyond RAM;
  an app loads SFX until blocks run out, then `load` fails. *(Built.)*
- **Playing voices** — the arbiter places any app's triggered voices on
  the shared tracks, priority-arbitrated (below): FCFS while slots are
  free, strict-greater-priority eviction when full. Owner-tagged; swept
  per-app. *(Arbiter exists; the `priority` field — currently reserved —
  becomes live for the eviction rule below; that is the one arbiter
  change.)*
- **Music streams** — a filesystem PCM stream with primed intro + loop
  heads for seamless looping (built: `prime_intro`/`prime_loop`, pinned
  head buffers, gap-free two-file model). The budget is **global across
  all apps** (not per-app), owner-tracked so an app's streams release on
  exit. The existing `AUDIO_SERVICE_MAX_MUSIC=3` becomes the configurable
  default; the addition is formalizing it as a global, owner-tracked,
  swept budget. Each stream slot's buffers scale the streaming-RAM
  footprint (§4). Stream allocation is priority-arbitrated (below).

**Uniform priority (streams and voices).** Both streams and SFX voices
carry a **plain integer `priority` magnitude** (free-form; devs number
however they like; a baseline default, e.g. 0). On a full pool (streams
vs the stream budget, voices vs the track count) the allocator finds the
**lowest-priority occupant**: a newcomer **strictly greater** evicts it
and takes the slot; **equal-or-lower rejects** (so the no-priority-set
case — everything at the baseline — behaves exactly like stable
FCFS-reject, no churn). Comparison is a trivial scan over a tiny set (≤
budget or ≤ track count) — no performance concern; the magnitude can be
any integer. Evicting a stream hard-stops the evicted music; this is rare
by default (equal priorities reject) and intended only for deliberate
override.

**Two fields, not one.** `priority` is *only* a magnitude. The emergency
behaviour is a **separate explicit field** `emergency_behavior`
(`NONE` / `MUTE_HOLD` / `STOP_RESET`), never bit-packed into the priority
integer — the eviction compare looks only at the plain magnitude; the
mixer reads the behaviour enum. (Spare priority bits are not a reason to
couple the two concepts; the self-documenting two-field form is chosen
for clarity and auditability. If the ecall ABI is ever register-tight,
the two may be packed/unpacked *only* at the marshal seam as a transport
detail — the model and logic always see two fields.)

**Emergency tier (the safety override).** The **maximum priority value**
(the integer ceiling, all-FFs) is the emergency tier — unbeatable by
construction (nothing exceeds the ceiling). The device's alarm/warning
sounds use it; the developer sizes the warning-channel budget (§4)
deliberately to fit their required simultaneous alarms. The guarantee
holds under a **first-party-trusted-code assumption** (all device code is
the manufacturer's and respects the convention) — recorded explicitly;
if untrusted apps ever run, API-boundary clamping of app priorities below
the ceiling would be the addition. Emergency streams get primed heads
(seamless) like all streams.

When an emergency-tier source is active, its `emergency_behavior` drives
a **mixer mute of the other (non-emergency) tracks**:

- **`MUTE_HOLD`** — other tracks are muted but **keep running at zero
  gain** (the mixer keeps advancing them; position tracking is therefore
  free — not-stopping *is* the tracking). Audio is restored by an
  **explicit** developer release/return-to-playback command, resuming at
  the **correct current position** (as if it had played underneath). The
  mute is held until explicitly released — never auto-lifted.
- **`STOP_RESET`** — other tracks are **killed/reset** (positions
  abandoned). After the emergency, the developer must **explicitly
  restart** whatever audio they want; nothing is tracked or restored.

**All recovery is explicit developer command** — releasing a `MUTE_HOLD`,
stopping the (possibly looping) emergency message, restarting
`STOP_RESET`-killed audio. The system **never auto-resumes or
auto-releases**: every state change is a privileged developer action.
This puts all safety-critical recovery choreography in the certified
device firmware, not the platform — the right boundary for a safety case
(no implicit transitions to audit). Composed example — the canonical
latched medical alarm: a **looping** emergency SFX (see SFX loop below)
raised at the ceiling with `STOP_RESET` gives a continuous dominant alarm
that silences everything else and stays latched until the device's
fault-handling logic explicitly stops it and restarts normal audio.

**SFX loop flag.** SFX gain a simple per-trigger **loop** property —
plain start-to-end repeat until the voice is stopped or evicted; **no**
intro/chaining (that two-file seam stays music-only). This exposes the
mixer's existing per-channel loop support through the trigger API. It
composes with the emergency tier (a looping emergency SFX = a repeating
alarm tone).

**ELF-swap cleanup.** A game that swaps ELFs for different loads spawns a
new task per ELF; the old task ends, and its exit sweeps its
owner-tagged SFX (pool), voices (arbiter), and streams (budget) and tears
down its channel. The new ELF starts clean. The app only ever calls
load / trigger / play / stop — teardown is implicit on exit. (If a build
instead re-images one persistent task across ELF loads, it must
explicitly free between loads, since the owner outlives the swap; the
new-task-per-ELF model is the zero-teardown happy path.)

---

## 3. Scheduler backends

Like the channel transport, the scheduler is one interface with
swappable backends, chosen by `config.h` / link:

### 3.1 Cooperative (the accessible default; builds first)

The floor. One task, run directly; external calls synchronous; services
pumped by the loop. **This is the closest thing to today's cooperative
scheduler, and the first implementation is to wrap the current behaviour
behind the §2 interface with no behaviour change** — the safety net, so
the working system remains available as a backend while the preemptive
one is built.

### 3.2 Windows simulated-systick (preemptive; the dev/validation path)

The configuration that lets the preemptive model be **exercised on a
platform that can be built here and run by you** — turning real-time
behaviour from "hardware-only, unverifiable" into "simulatable".

- **one Windows thread per VM task**, the slab allocator still owning
  all task-side memory.
- a **timer thread acting as the systick interrupt**: it fires on a
  period, **suspends the currently-active task thread**
  (`SuspendThread`), runs the scheduler's task-management pass (pick
  next task by priority), then **resumes** the chosen thread
  (`ResumeThread`). From any task's view it can be stopped at an
  arbitrary instruction and resumed later — faithful preemptive
  semantics.
- **the systick suspends only task threads**, never service threads
  (invariant §2.3.3) — audio's thread is device-paced and left alone.

**The `SuspendThread` hazard, stated plainly.** If the timer thread
suspends a task while it holds a lock the rest of the system needs — a
CRT lock inside `malloc`/`printf`, etc. — and another task or the
scheduler then needs that lock, the system deadlocks on a frozen owner.
This is the known peril of `SuspendThread`-based scheduling. Mitigations
are the invariants: route all task-side allocation through the slab
allocator (lock under our control), keep CRT calls off the preemptible
VM-step path, and ensure the scheduler's own bookkeeping shares no lock
with anything a suspended task can hold. **This is also the part most
likely to pass a quick test and deadlock under load — it needs your real
running to shake out.** The simulation validates *logic and structure*
(does preemption happen, does the scheduler pick correctly, no deadlock
under TSan); it does **not** model hardware timing — Windows will not
schedule the timer thread with hard precision, so preemption granularity
is coarser than a real 1 ms systick. Functional fidelity, not timing
fidelity.

### 3.3 Native micro-RTOS & FreeRTOS (preemptive; hardware-verified by you)

Two more backends behind the same seam, for bare-metal targets:

- a **native micro-RTOS** scheduler (the default for standalone micro
  builds — "enough for anyone" without external dependencies).
- a **FreeRTOS backend**: VM tasks become FreeRTOS tasks
  (`xTaskCreate`), yields/waits map to FreeRTOS primitives, critical
  sections to `taskENTER_CRITICAL`. This is the enterprise-supported
  path — it lets the system drop into a customer's existing FreeRTOS
  application and coexist with FreeRTOS-coupled middleware (USB/net
  stacks, vendor HALs that assume FreeRTOS task contexts).

**Honest limit on the FreeRTOS integration:** we route *our* scheduling,
sync, timing, and allocation through our abstraction + slab allocator,
but a FreeRTOS-dependent middleware library will still use FreeRTOS's
own heap and tasks underneath. We provide a FreeRTOS *backend* for our
abstraction; we do not (and cannot fully) prevent inseparable middleware
from using FreeRTOS directly. "Use our system as much as possible" is
honoured for our own code, not imposed on third-party libs.

These two are **verified on hardware by you** — the sandbox cannot run
them — exactly as the waveOut backend and `channel_win32.c` were.

---

## 4. Memory configuration

The slab allocator (`slab_stack.h`) already takes a caller-provided
`(region, bytes, SlabConfig, locker)` and never assumes where the
region came from — so "static array on the micro" and "malloc'd buffer
on the PC" are just two callers with two provisioning strategies. It
also already has a `SlabLocker` callback abstraction with documented
single-threaded / ISR / RTOS modes — that is precisely the
critical-section seam the scheduler needs (§4.5).

**Two intents, one source.** This memory model (like the execution model)
serves both deployment intents from the same source via `config.h`: the
SNES cartridge build (two-core, hot-path M7, the bulk PSRAM tier) and the
commercial secure/optimized micro builds (which may have only fast SRAM,
or a different bulk tier, or none). The tiering and sizing below are all
config choices, not forks.

**Tiered: L1 + optional L2.** Memory is configured as tiers that map onto
the board's physical topology:

- **L1** — fast internal micro SRAM. The hot/small working allocations.
  Always present.
- **L2** — slower bulk external memory (PSRAM, FMC-attached SDRAM, etc. —
  the allocator doesn't care which; it's "the big slow region"). For
  larger or colder allocations. **Optional and `config.h`-gated**: a
  board with no external memory simply doesn't define an L2 region and
  the L2 slab compiles out.

Each tier is just **another `SlabAllocator` instance over its region**,
with its own `SlabConfig` (L2 skews toward larger bins) and its own
locker — the allocator is already region-agnostic, so this is
configuration, not new allocator code. The audio sample pool is a
specialized L2 resident (samples-at-rest in bulk memory), coexisting with
(partitioned from) any general L2 slab; both count in the L2 footprint.
**Crossing tiers for hot work is just an app-level `memcpy`** — copy a
working piece from L2 into L1 scratch, work, copy back — entirely at the
app's discretion. There is **no transparent caching/paging** between
tiers, by deliberate design: it would add non-determinism a safety case
can't easily certify. (A *future* direction, deferred and uncoded: making
allocator tiers map to distinct VM **virtual address ranges** — e.g.
reshaping the 1 GB shared region into tier-backed sub-ranges, translation
routing by address range to the physical tier. Noted for shape so the
present design stays compatible; not built.)

### 4.1 `config.h` declares the bin shape (per tier)

The allocator has 16 bins, bin *i* of block size `SLAB_MIN_BLOCK << i`
(32 B … 1 MB). `config.h` declares the **default block count per bin**,
**per tier** (the L2 set exists only when L2 is configured):

```c
/* config.h — default blocks per slab bin (bin i = (32 << i) bytes). */
#define GARBAGE_SLAB_BLOCKS_32B     0
#define GARBAGE_SLAB_BLOCKS_64B     0
#define GARBAGE_SLAB_BLOCKS_128B    0
/* ... one per bin ... */
#define GARBAGE_SLAB_BLOCKS_8KB     0
/* ... up to 1 MB ... */
```

These feed a `SlabConfig.bucket_counts[]`. Footprint is then **derived**
from the shape — the developer never hand-tallies bytes.

**No roll-into-the-next-bin (deterministic, by design).** If a bin is
exhausted, the allocation **fails** (`SLAB_ERR_BIN_EXHAUSTED`) — it does
*not* satisfy the request from a larger bin. On a micro you do not want a
5-byte request, finding the 32 B bin empty, burning a 16 KB block in
desperation. Fail-fast per bin means the developer learns their bin
sizing is wrong and fixes the config, rather than chasing mysterious
large-bin exhaustion later. For the safety case this is a *feature*:
deterministic per-bin failure is auditable; silent cross-bin
cannibalization would make exhaustion non-deterministic and hard to
certify. (This is the allocator's existing documented behaviour.)

### 4.2 The totaling formula (mirrors `slab_required_bytes` exactly)

From the allocator's own `slab_required_bytes`, the region size is:

```
required = 8 (alignment headroom)
         + Σ over bins i of:
             count_i * sizeof(void*)                       (free-stack)
           + count_i * ((SLAB_MIN_BLOCK << i) + SLAB_HEADER_SIZE)  (blocks)
```

`SLAB_HEADER_SIZE` is 8 (4 magic + 4 bin index; same with
`SLAB_NO_MAGIC`). `sizeof(void*)` is **8** on the 64-bit PC host and
**4** on the 32-bit M7 — so the macro form takes the pointer size as a
config constant (`GARBAGE_SLAB_PTR_BYTES`) to stay correct on both.

This formula must be available **twice**, and they must not drift:

- a **compile-time macro** `GARBAGE_SLAB_TOTAL_BYTES` (a constant
  expression) — so the micro can declare a `static` backing array of
  exactly that size and `_Static_assert` it against the RAM budget;
- the existing **runtime function** `slab_required_bytes(cfg)` — for
  the PC's startup sizing (§4.3).

To prevent drift, a `_Static_assert` (or a startup check) verifies the
macro and the function agree for the default counts.

### 4.3 Two provisioning strategies, one allocator

**Micro — static.** The `config.h` counts are compile constants; the
total is the macro; the backing store is a single statically-declared
array of that size; `slab_init` runs over it. A `_Static_assert` checks
the array fits the target's SRAM region (and doesn't collide with the
audio pool, the stacks, etc.). An over-large bin config fails the
**build**, not the device.

```c
/* micro */
static uint8_t g_slab_region[GARBAGE_SLAB_TOTAL_BYTES];
_Static_assert(GARBAGE_SLAB_TOTAL_BYTES <= GARBAGE_SLAB_BUDGET_BYTES,
               "slab config exceeds the RAM budget — reduce bin counts");
```

**PC host — dynamic, overridable.** The `config.h` counts are
**defaults**, overridable at startup by the existing config file (§4.4).
After the (validated) counts are known, `slab_required_bytes` totals
them at runtime and a **single native `malloc`/`VirtualAlloc`** provides
the buffer; `slab_init` runs over it. This one provisioning allocation
happens **at startup, before any task or systick is live**, so it does
not violate the "slab-only on preemptible paths" invariant (§2.3.2) —
the CRT-lock hazard is about *runtime/preemptible* paths, and a single
init-time `malloc` touches none of those.

### 4.4 PC override via the existing config file

Reuse the host's existing `vm.cfg` / inicfg mechanism and its
`defaults → file → CLI` cascade (the same one mounts use) — no new
config paradigm. A new section carries per-bin overrides:

```ini
[slab]
blocks_32b  = 256
blocks_64b  = 128
blocks_8kb  = 16
```

Layering: `config.h` compiled-in defaults < config-file values < (if
desired) CLI flags.

### 4.5 Override validation (PC only)

On the micro the counts are compile constants and `_Static_assert`-
bounded — safe for free. On the PC they are **untrusted runtime input**:
a config file could set a count to zero, or to a value whose total would
`malloc` gigabytes. The PC path must therefore **validate**: per-bin
counts within sane ranges, the computed total within a configured
maximum, and a graceful clear-message failure if `malloc` would be
unreasonable or fails — never attempt an absurd allocation and crash.
This validation is the PC's cost for runtime flexibility; the micro gets
the equivalent guarantee for free at compile time.

### 4.6 The locker is the critical-section seam

`SlabLocker` already abstracts lock/unlock with saved state and
documents the modes we need: **single-threaded → null locker**
(cooperative/single-threaded), **bare-metal ISR → `__disable_irq`/`__enable_irq`**,
**RTOS → mutex / critical section** (preemptive). So the allocator's
own internal critical sections are already routed through the same
mechanism the scheduler will use; the `config.h` exec mode selects which
locker the allocator is initialized with. No new locking concept is
introduced — the existing `SlabLocker` is the seam.

**Why interrupt-disable is safe here (the safety-case rationale).** The
bare-metal locker disables interrupts around the allocator's critical
section. This is normally a dangerous habit — but it is *optimal* here
precisely because the protected op is **O(1) and bounded**: alloc/free is
a free-stack push/pop + a header magic write/check, a fixed handful of
instructions (tens of nanoseconds on a 480 MHz M7), with **no loop, scan,
coalesce, or block**. Bin selection is `clz`-based size arithmetic (one
cycle), touching no shared state, so it needs no lock at all — only the
push/pop is bracketed. The worst-case interrupt latency introduced is
therefore those few instructions, far below any deadline that matters
(audio DMA, the SNES bus). This bounded-O(1) property is also what makes
the broader "all allocation goes through the slab on preemptible paths"
invariant (§2.3.2) *safe*: a variable-time allocator could not be
interrupt-disabled safely. For the safety case the claim is concrete and
auditable — "the allocator critical section is provably O(1), so disabled-
interrupt latency is bounded to N instructions" — not a hand-wave.

Header-magic **double-free / foreign-pointer detection** (returns an
error rather than corrupting the free-stack) is likewise O(1) and lives
in the same bracket; for a safety build, a detected double-free is
evidence of a real bug and a product may choose to escalate (log / fault
/ safe-state) rather than silently return the error code — a policy left
to the device firmware.

---

## 5. Build order (each step independently valuable & verifiable)

1. **Exec-model interface (§2), no behaviour change.** Define the
   scheduler + native-service seam; implement it as a thin wrapper over
   the **current cooperative behaviour**. Nothing changes
   at runtime — this is the safety net. *(Verifiable here: existing
   tests + audio still pass.)*
2. **Memory config (§4).** `config.h` bin defaults, the dual
   macro/function total, the micro static-array path with
   `_Static_assert`, the PC malloc + `vm.cfg` override + validation.
   *(Verifiable here: totals match `slab_required_bytes`; PC override
   parses, validates, provisions; the allocator runs over both buffer
   kinds.)*
3. **Windows simulated-systick backend (§3.2).** Thread-per-task + the
   timer-thread systick + suspend/schedule/resume. Bring up one task,
   then two; prove preemption + resumption + no deadlock (TSan).
   *(Verifiable here functionally; timing + the SuspendThread hazard
   under load are yours to shake out.)*
4. **Audio as a native service under preemption (§2.2).** Audio keeps
   its device-clock thread beside the scheduler, excluded from the
   systick set; verify it stays fed while a busy task is preempted.
5. **Native micro-RTOS + FreeRTOS backends (§3.3).** Hardware-verified
   by you.

Stop at any step where the result is "good enough"; nothing later is
required for the earlier steps to be useful. The cooperative floor
(steps 1–2) is a complete, shippable, low-risk configuration on its own.

---

## 6. Honesty boundaries (what is verified where)

- **Verified in the build sandbox:** functional correctness of the
  exec-model seam and the cooperative backend; the memory-config totals,
  static/dynamic provisioning, override parsing/validation; the Windows
  sim *functionally* preempting/resuming without deadlock under TSan.
- **Verified by you on real targets:** real-time *timing* (the sim is
  functional-not-timing-faithful); the `SuspendThread` CRT-lock hazard
  under sustained load; the native micro-RTOS and FreeRTOS backends; the
  M7 bus-emulation timing and its frame-data atomicity (your hardware
  domain — the abstraction only needs to *allow* a bare-metal hot-path
  core, which the floor configuration provides).

This is the same verification split that the audio arc used: the
sandbox proves structure and logic; hardware-specific timing and
platform primitives are confirmed on your machine.

---

## 7. Preemptive scheduler: concrete design (grounded in the real code)

§3.2 sketched the Windows simulated-systick. This section pins down the
*mechanics*, grounded in the existing `vm_sched` (read, not assumed) and
a proven preemption primitive — the blueprint the implementation builds
against.

### 7.1 The preemptive scheduler is a NEW scheduler, not a wrap of vm_sched

The cooperative `vm_sched` is a single host thread that calls `vm_step`
on one VM at a time with an instruction *budget*, and the VM returns
control when the budget is spent (or it yields/blocks/traps). The
scheduler decides when to stop a VM.

The preemptive model is fundamentally different: **each task runs in its
own thread**, running its own loop continuously, and a **systick
interrupts it from outside**. The task does not return on a budget; it is
preempted. So the preemptive scheduler is not a modification of
`vm_sched`'s loop — it is a *separate* scheduler that shares the VM
*core* (instruction semantics, register file, ecall routing) but has a
completely different driving loop. The two are the two backends the
`config.h` mode flag selects between; `vm_sched` stays untouched as the
cooperative backend (the safety net).

Consequence for the inner loop: because preemption is external, the VM's
per-thread loop needs **no per-instruction budget check** — so it can be
**heavily unrolled** (several instructions per iteration, fewer
branch-backs, better I-cache use). The cooperative loop pays
per-instruction bookkeeping to know when to return; the preemptive loop
pays none and is free to be as tight as possible. This is also a
*performance* path for a hot single VM, not only a fairness mechanism.

### 7.2 Tasks are thread-centric and content-agnostic

A **task** is *a thread the scheduler preempts*, plus its scheduling
state. What the thread runs is just its entry function:

- a **VM task** runs the unrolled interpreter loop over a `VmCpu`;
- a **native task** runs plain compiled C (compute work, a
  passthrough-heavy routine).

From the systick's view they are identical — a thread to suspend and
resume. The scheduler core is therefore content-agnostic; "schedule both
VM and native tasks" needs no special machinery, it falls out of the task
being "a thread," not "a VM". A trivial cycle-burning loop *is* a native
task — which is why the first test (below) is simultaneously the
native-task proof.

### 7.3 Three categories that must not be conflated

- **Task** (VM or native): under the scheduler, time-sliced/preempted by
  the systick. In the preemptible set.
- **Service** (audio; future graphics/PPU): *outside* the scheduler,
  paced by a device/event clock, **never** in the preemptible set —
  structural, not priority-based. The systick has no knowledge of it.
  (Whether audio is modeled as an outside-the-scheduler service or as a
  high-priority blocking task à la a FreeRTOS timer task is an open
  choice, revisited when services are built.)
- **Transient critical section**: a normally-preemptible task that
  *temporarily* suppresses its own preemption (enter/exit) to finish
  something atomically. A momentary task-controlled state, distinct from
  the permanent service category.

Priority is NOT a non-preemptibility mechanism for services: "highest
priority" is a *relative, conventional* guarantee (something else could
reach that priority), whereas a service being outside the preemptible set
is *structural* (no priority value puts the systick in charge of it).
Same reasoning as the reserved emergency-audio tier vs honour-system
priority. Use the category for services; use priority for ordering tasks;
use critical-section suppression for transient task atomicity.

### 7.4 Fixed-priority preemptive, O(1) selection

The scheduling model is the standard small-RTOS one (as in FreeRTOS):
**fixed-priority preemptive with round-robin among equals.**

- **One ready queue per priority level** + a **priority bitmap** (bit *p*
  set iff level *p* has a ready task).
- **Pick next = find-highest-set-bit(bitmap) → rotate that level's
  queue.** The highest-set-bit is a single `clz`/`ctz` (one ARM
  instruction — the same primitive the slab allocator uses for bin
  selection). The queue rotation is O(1). So the *entire* scheduling
  decision is constant time regardless of task count or level count — no
  scanning, no sorting, no priority comparison loop. This bounded
  constant-time selection is what makes scheduling latency deterministic,
  which the real-time/safety case needs.
- **Higher priority always preempts lower**: when a higher-priority task
  becomes ready, the systick switches to it.

### 7.5 Round-robin within a level: time-sliced or yield/block-based

Among ready tasks at the *same* priority level, two configurable
behaviours (cf. FreeRTOS `configUSE_TIME_SLICING`):

- **time-sliced**: the systick rotates the level's queue every tick (or
  N ticks) — fair time-slicing among equal compute tasks.
- **yield/block-based**: a task at the level runs until it yields or
  blocks, then the next at that level runs — lower overhead, right for
  well-behaved tasks that block naturally.

**Critical / high-priority tasks** (the developer's "critical tasks",
the FreeRTOS-timer-task pattern) are high-priority tasks that **block
between activations**: they wake (preempting lower work because they are
high-priority), do their brief periodic work, and block again. The high
priority gives prompt *latency*; the blocking is what makes them *regular
and non-starving*. A high-priority task that never blocks will run
constantly and starve everything below it — which leads to:

### 7.6 Strict priority; starvation is the task designer's responsibility

Strict fixed-priority: the scheduler **always** runs the highest-priority
ready task. A never-blocking high-priority task **will** starve lower
ones — by design. We do **not** add priority aging or guaranteed-minimum
heuristics: they trade away the O(1)/deterministic guarantee and add
nondeterminism a safety case must then reason about. The discipline
"high-priority tasks must block between activations" is load-bearing and
is the *task designer's* responsibility, documented as such. The
guarantee is one auditable sentence: *the scheduler always runs the
highest-priority ready task.* (This is the preemptive analogue of the
cooperative scheduler's existing critical-section-debt fairness
mechanism.)

### 7.7 Ecalls, blocking, traps run in the task's own thread

Unlike cooperative mode (where `vm_step` returns to the scheduler to
route an ecall), a preemptive VM task handles these *in its own thread*:

- **ecall** → the loop calls the ecall router inline, in-thread.
- **blocking ecall** (mailbox recv, sleep) → the thread actually *waits*
  (condition variable / event) — blocking is just the thread blocking,
  more natural than the cooperative "set block_reason and return".
- **trap** → handled in-thread (run the trap handler / self-terminate).

The systick is *not* involved in ecall routing or blocking — those happen
in the threads. The systick's only job is "stop whoever's running, pick
next, resume". **Consequence:** ecall handlers now run concurrently
across task threads, so any shared state they touch needs real
concurrency safety (the audio channel already is — lock-free SPSC;
mailboxes and other shared handlers need auditing as they're brought into
the preemptive model). This is the structural-non-concurrency-replaced-
by-real-concurrency change §2.3 warned about, made concrete.

### 7.8 The preemption primitive (and the honesty boundary)

- **Windows (the target sim):** `SuspendThread`/`ResumeThread` on the
  task threads, driven by a timer thread acting as the systick. Truly
  asynchronous suspend (stops a thread at any instruction). **Compile-
  checked only in the sandbox; the real async-suspend behaviour and the
  `SuspendThread` CRT-lock hazard under load are verified by the user on
  Windows** — same boundary as `channel_win32.c` and waveOut.
- **POSIX (in-sandbox validation):** a periodic timer (`timer_create` +
  `SIGEV_SIGNAL`) delivers a systick signal; the handler runs the
  scheduling decision. *Proven working* (a 1 ms timer reliably interrupts
  a running busy loop). This validates the scheduling *logic* (preemption
  happens, the scheduler picks correctly, no deadlock under TSan) but is
  **not** identical to `SuspendThread`: a POSIX signal runs the handler
  in the target thread's context at an OS-chosen point, whereas
  `SuspendThread` is a truly external async stop. The POSIX path proves
  *logic*; the Windows path is the real async-suspend, user-verified.

This is the same split that worked for audio: the sandbox proves
structure and logic under TSan; the platform-specific async primitive and
real-time timing are confirmed on the user's hardware.

### 7.9 Build order for the preemptive scheduler

1. **Bare round-robin skeleton** — N native-loop tasks at a single
   priority level, a systick that round-robin-preempts them. Proves the
   preemption mechanism + queue rotation + no deadlock (TSan). This *is*
   the native-task proof. *(POSIX-validated in-sandbox; Windows
   SuspendThread twin compile-checked.)*
2. **Priority dimension** — per-level ready queues + bitmap + `clz`
   selection + higher-preempts-lower. Critical/high-priority tasks become
   expressible.
3. **Block/wake** — what makes high-priority periodic tasks *regular*
   (and is the natural home for blocking ecalls).
4. **VM tasks** — drop the unrolled real-`VmCpu` interpreter loop in as a
   task entry function. No scheduler changes.
5. **Services + integration** — audio under preemption (service vs
   high-priority-task decision), the systick/service-exclusion invariant,
   ecall-handler concurrency audit.

Each step is independently verifiable. The cooperative `vm_sched` and the
whole working system stay untouched throughout, behind the `config.h`
mode flag.
