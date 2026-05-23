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
all plain reject-when-full (no priority; see below), all owner-tagged to
the requesting app, all reclaimed on app exit (or ELF swap-out, when
that ends the task), all `config.h`-configurable and feeding the
footprint math (§4):

| resource | scarcity / limit | rejection point | config knob |
|---|---|---|---|
| **resident SFX** | audio-pool block RAM | `load` fails when RAM full | pool size (block count) |
| **playing voices** | mixer track count | `trigger` fails when voice-set full (FCFS) | track count (default 16) |
| **filesystem music streams** | global stream budget across *all* apps | stream request returns error when budget exhausted | max streams (default 3) |

- **Resident SFX** — the audio pool (block-based, non-contiguous, 8 KB
  blocks, refcounted, out-of-band metadata) is exactly the cheap
  add/remove churn an ELF-swapping game needs. No count cap beyond RAM;
  an app loads SFX until blocks run out, then `load` fails. *(Built.)*
- **Playing voices** — the arbiter places any app's triggered voices on
  the shared tracks, FCFS, reject-on-full. Owner-tagged; swept per-app.
  *(Built — unchanged.)*
- **Music streams** — a filesystem PCM stream with primed intro + loop
  heads for seamless looping (built: `prime_intro`/`prime_loop`, pinned
  head buffers, gap-free two-file model). The budget is **global across
  all apps** (not per-app), owner-tracked so an app's streams release on
  exit. Requesting one when the budget is full returns an error — no
  priority, no eviction (evicting live music is jarring, and streams are
  long-lived). The existing `AUDIO_SERVICE_MAX_MUSIC=3` becomes the
  configurable default; the addition is formalizing it as a global,
  owner-tracked, swept budget. Each stream slot's buffers scale the
  streaming-RAM footprint (§4).

**Priority is deferred.** SFX could in principle carry priority to cut
off a lower-priority playing voice when the track set is full (the
arbiter's `priority` field is reserved for this). It is **not in scope**
— no current use case justifies the complexity. Everything rejects
plainly when full. The field stays reserved-and-unused; priority-evict
is a possible future refinement if a use case (e.g. a system alert that
must cut through) appears. No code for it now.

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

### 4.1 `config.h` declares the bin shape

The allocator has 16 bins, bin *i* of block size `SLAB_MIN_BLOCK << i`
(32 B … 1 MB). `config.h` declares the **default block count per bin**:

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
