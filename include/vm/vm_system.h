/* ============================================================
 *  vm_system.h — top-level VM system composition
 *
 *  Owns instances of everything the VM needs:
 *    - The scheduler (vm_sched.h)
 *    - The ECALL router (vm_ecall.h) with standard handlers
 *      pre-registered
 *    - The shared-region slab allocator (memory/slab_stack.h)
 *    - The per-VM mailbox table (vm_mailbox.h, one per VM)
 *    - Per-VM CPU structs (vm_core.h)
 *    - A bump arena for all caller-managed VM-local storage
 *
 *  vm_system_init wires all of these together in the right order:
 *
 *    1. Initialize the slab over the shared-region storage
 *    2. Initialize the bump arena over the local-region storage
 *    3. Initialize the scheduler with the provided config
 *    4. Initialize the ECALL router and register standard handlers
 *
 *  After init, the system is empty (no VMs). The caller registers
 *  VMs one at a time via vm_system_load_vm: each call loads an
 *  ELF, allocates the VM's data region and mailbox storage from
 *  the bump arena, and registers it with the scheduler.
 *
 *  Once all VMs are loaded and any application-specific ECALL
 *  handlers are installed, the caller invokes vm_system_run.
 *
 *  ---------------------------------------------------------------
 *  Memory model
 *  ---------------------------------------------------------------
 *
 *  The system needs three pools of memory from the caller:
 *
 *    1. shared_storage  — RAM for the shared region. Sized at
 *                         boot, partitioned by the slab.
 *                         Typical: 32 KB - 1 MB.
 *
 *    2. local_storage   — RAM for per-VM data regions, mailbox
 *                         buffers, and copy-to-RAM code/rodata
 *                         for VMs that use VM_BACKING_COPY_RAM.
 *                         Managed by the system's bump arena.
 *                         Typical: 16 KB per VM × VM count, plus
 *                         mailbox storage.
 *
 *    3. The VmSystem struct itself — caller-declared, holds all
 *                         the fixed-size internal state.
 *
 *  All three are caller-owned. The system does not free them on
 *  destroy; the caller manages their lifetimes.
 *
 *  ---------------------------------------------------------------
 *  Defaults
 *  ---------------------------------------------------------------
 *
 *  The config struct is designed so zero-init produces a workable
 *  system: only the storage pointers/sizes are mandatory. For
 *  production, you'll typically set the scheduler quantum and the
 *  slab bin counts to match your workload.
 *
 *  Defaults applied when fields are zero at init:
 *    baseline_quantum:     5000 instructions
 *    max_critical_overrun: 50000 instructions (10x baseline)
 *    slab_config:          a balanced default (see vm_system.c)
 *    trap_handler:         NULL (built-in: terminate trapping VM)
 *    idle_handler:         NULL (busy-loop)
 *    mailbox slot_size:    32 bytes (per VM, used by load_vm
 *                          when caller doesn't override)
 *    mailbox depth:        8 slots
 *
 *  ---------------------------------------------------------------
 *  Standard ECALL handlers
 *  ---------------------------------------------------------------
 *
 *  Installed automatically by vm_system_init:
 *
 *    SYS_EXIT, SYS_SELF, SYS_YIELD, SYS_CRITICAL_ENTER,
 *    SYS_CRITICAL_EXIT, SYS_ALLOC, SYS_FREE, SYS_SEND, SYS_RECV,
 *    SYS_MAILBOX_INFO, SYS_WHITELIST_ADD, SYS_WHITELIST_REMOVE
 *
 *  The libc memory/string and fixed-point math syscall ranges
 *  (SYS_MEMCPY..SYS_STRCHR and SYS_FIX_SIN..SYS_FIX_TO_DOUBLE)
 *  are reserved in the ABI but not provided by this library.
 *  A guest that wants them implements them itself, or links
 *  against a separately-distributed accelerator package.
 *
 *  Caller can register additional handlers or override any of the
 *  standard ones via vm_ecall_register on the system's router
 *  (accessible as system->ecall_router).
 *
 *  ---------------------------------------------------------------
 *  Depends on: vm_core, vm_ecall, vm_mailbox, vm_loader, vm_sched,
 *              memory/slab_stack, memory/bump
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_SYSTEM_H
#define VM_SYSTEM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "vm/vm_core.h"
#include "vm/vm_ecall.h"
#include "vm/vm_mailbox.h"
#include "vm/vm_loader.h"
#include "vm/vm_sched.h"
#include "vm/vm_sched_ops.h"
#include "vm/vm_pre.h"   /* VmPreCtx (empty unless GARBAGE_SCHED_PREEMPTIVE) */
#include "memory/slab_stack.h"

/* ============================================================
 *  Configuration
 *
 *  Zero-init produces a usable config for small experiments. For
 *  real deployments, set at least the storage pointers/sizes and
 *  consider tuning the scheduler quantum and slab config.
 * ============================================================ */

typedef struct {
    /* === Storage pools (REQUIRED — no defaults) === */

    /* Shared region. The slab carves this up. */
    void   *shared_storage;
    size_t  shared_storage_size;

    /* L2 sub-region (upper half of SHARED, 0xE000_0000+).
     *
     * Optional system-wide PSRAM backing applied to every VM the
     * system loads. NULL/0 leaves the L2 half absent — accesses to
     * 0xE000_0000+ fault, matching the pre-split behavior bit-for-
     * bit (lets existing consumers ignore this field). The embedder
     * is responsible for the actual allocator over this region;
     * the VM core only does address translation. */
    void   *l2_shared_storage;
    size_t  l2_shared_storage_size;

    /* Local storage: per-VM data regions, mailbox buffers, and
     * copy-to-RAM segments. Managed by the system's bump arena.
     * Should be large enough for all VMs the system will hold. */
    void   *local_storage;
    size_t  local_storage_size;

    /* === Slab configuration (zero = balanced default) === */
    SlabConfig slab_config;

    /* === Scheduler tuning (zero = defaults) === */
    uint32_t baseline_quantum;       /* default 5000  */
    uint32_t max_critical_overrun;   /* default 50000 */

    /* === External tick source (NULL = step-counted ticks) ===
     *
     * Lets the host back ticks with a real-time source — a 1 ms
     * SysTick on a microcontroller, clock_gettime on a PC. See
     * VmSchedConfig for full details. The scheduler reads from
     * this callback on each step and reports its rate via
     * SYS_TICK_HZ to guests.
     *
     * If tick_source is NULL, ticks accumulate as retired
     * instructions (today's default). In that mode SYS_TICK_HZ
     * returns 0 so guests can detect the absence of real-time. */
    uint32_t (*tick_source)(void *userdata);
    void     *tick_source_userdata;
    uint32_t  ticks_per_second;

    /* === Handler callbacks (NULL = defaults) === */
    VmTrapHandler trap_handler;
    VmIdleHandler idle_handler;

    /* === Default mailbox shape (zero = defaults) ===
     *
     * Used by vm_system_load_vm when the caller doesn't pass an
     * explicit mailbox shape. Per-VM overrides are still
     * supported via vm_system_load_vm_with_mailbox. */
    uint16_t default_mailbox_slot_size;   /* default 32 bytes */
    uint16_t default_mailbox_depth;       /* default 8 slots  */

    /* === Per-VM sizing (drives the local slab's bin sizes) ===
     *
     * These are surfaced because they correspond directly to the
     * vm.cfg knobs the user thinks about: 'how many VMs can run
     * concurrently' and 'how much data each gets.'
     *
     * max_vms is an upper bound on concurrent active VMs. Sets
     * the slot count of every per-VM bin in the local slab. If
     * you load more VMs than this, vm_system_load_vm returns
     * VM_SYS_ERR_FULL.
     *
     * spawn_data_kb is the size in KB of the per-VM data region.
     * Rounded up to a power of 2 internally to fit a slab bin.
     * Zero = use a small default (currently 16 KB) suitable for
     * non-TUI guests. */
    uint16_t max_vms;            /* default 8 */
    uint32_t spawn_data_kb;      /* default 16 */

} VmSystemConfig;

/* ============================================================
 *  VmSystem
 *
 *  Caller declares one. The internal arrays are sized to
 *  VM_SCHED_MAX_VMS. The fields below "Public read-only" can be
 *  inspected; the rest are internal.
 * ============================================================ */

/* Per-VM tracking of SYS_ALLOC blocks. The auto-cleanup path on
 * vm_system_unload_vm walks this list and frees each entry — so
 * a guest that crashes or exits without freeing its allocations
 * doesn't leak shared-slab memory.
 *
 * Storage: a fixed-size array of host pointers per VM. Cap at
 * VM_PER_VM_ALLOC_CAP entries; SYS_ALLOC returns -ENOMEM if the
 * VM already holds that many. The cap is intentional — it makes
 * the tracking memory bounded at startup and turns "guest leaks
 * unboundedly" into a fail-loud condition. */
#define VM_PER_VM_ALLOC_CAP  32

typedef struct {
    /* host pointers (NOT guest addresses). NULL entry = free slot. */
    void   *blocks[VM_PER_VM_ALLOC_CAP];
    uint8_t count;       /* number of non-NULL entries */
} VmAllocTracking;

typedef struct {
    /* === Public read-only — pointers to internal subsystems === */

    /* The ECALL router. Use vm_ecall_register on this to install
     * additional or override standard handlers. */
    VmEcallRouter *ecall_router;

    /* The scheduler. Use vm_sched_* on this for introspection. */
    VmSched *sched;

    /* The shared-region slab. Use slab_* on this to inspect usage. */
    SlabAllocator *shared_slab;

    /* The local-region slab. Per-VM allocations (VmCpu, mailbox
     * storage, copy-to-RAM segments, data region) all come from
     * here. Use slab_* to inspect usage.
     *
     * Replaces the previous bump_arena. Slabs support real
     * per-allocation free, so spawned VMs can be unloaded
     * independently and in any order without fragmenting the
     * arena — essential for the multi-VM concurrent use case
     * the window manager and program manager will exercise. */
    SlabAllocator *local_slab;

    /* === Public read-only — config snapshot === */
    VmSystemConfig config;

    /* === Internal — owned subsystem instances === */

    /* The subsystems are stored by value so the public pointers
     * above are stable and the caller doesn't have to manage
     * lifetimes. */
    VmEcallRouter _ecall_router;
    VmSched       _sched;
    SlabAllocator _shared_slab;
    SlabAllocator _local_slab;

    /* The scheduler seam. A by-value copy of the chosen backend's
     * VmSchedOps with .ctx bound to this system's scheduler, set in
     * vm_system_init. vm_system calls scheduler ops through this
     * (sys->ops.fn(sys->ops.ctx, ...)) rather than vm_sched_*
     * directly, so the syscall core is scheduler-agnostic. */
    VmSchedOps    ops;

#if GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE
    /* The preemptive backend's context (PreSched, vm<->task map,
     * per-mailbox mutexes). ops.ctx points here under preemption.
     * Absent in the cooperative build. */
    VmPreCtx      _pre;

    /* Mutex backing the shared/local slab locker under preemption.
     * Lives here (not in _pre) because slab_init takes its locker by
     * value before _pre is set up. */
    pthread_mutex_t _slab_mtx;
#endif

    /* === Per-VM state ===
     *
     * Indexed by vm_id. Slot is "in use" iff vms[i] is non-NULL
     * (mirrors the scheduler's registration state). */

    /* CPU structs are allocated from the local slab at load time
     * and the pointers stored here. */
    VmCpu *vms[VM_SCHED_MAX_VMS];

    /* Per-VM mailboxes. The VmMailbox struct itself lives here
     * (no extra indirection); its storage buffer comes from the
     * bump arena. */
    VmMailbox mailboxes[VM_SCHED_MAX_VMS];

    /* Per-VM tracking of SYS_ALLOC blocks (shared-slab pointers).
     * vm_system_unload_vm walks each VM's list and frees any
     * entries the guest didn't free explicitly. */
    VmAllocTracking alloc_tracking[VM_SCHED_MAX_VMS];

    /* Unload hooks. Each registered hook is called by
     * vm_system_unload_vm before the VM's CPU and regions are
     * torn down — useful for host services that hold per-VM
     * state (like the TUI canvas owner). Up to 8 hooks; in
     * practice we use only one (vm_host_tui). */
    void (*unload_hooks[8])(uint16_t vm_id, void *userdata);
    void  *unload_hook_userdata[8];
    uint8_t unload_hook_count;

} VmSystem;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Initialize a system. Wires up the slab, bump arena, scheduler,
 * and ECALL router with standard handlers.
 *
 *   sys: caller-owned VmSystem struct
 *   cfg: configuration. Mandatory fields are the storage pools;
 *        everything else has a sensible default applied to zero
 *        values.
 *
 * Returns true on success. Failures:
 *   - shared_storage NULL or too small for slab_config
 *   - local_storage NULL or zero size
 *   - mismatched/invalid config values
 *
 * On failure, the system is left in an uninitialized state. */
bool vm_system_init(VmSystem *sys, const VmSystemConfig *cfg);

/* Tear down the system. Marks all VMs halted, clears internal
 * state. Does NOT free any of the storage pools — caller owns
 * those. Safe to call on a zero-initialized struct. */
void vm_system_destroy(VmSystem *sys);

/* Register a callback to be invoked from vm_system_unload_vm
 * before per-VM state is freed. Used by host services that
 * hold per-VM resources (e.g., vm_host_tui's canvas owner).
 *
 *   hook        called with the unloading VM's vm_id and the
 *               opaque userdata. Must be safe to call even if
 *               the VM never used the service.
 *   userdata    forwarded to the hook each call.
 *
 * Returns false if the hook table is full (max 8 hooks). */
bool vm_system_register_unload_hook(
    VmSystem *sys,
    void (*hook)(uint16_t vm_id, void *userdata),
    void *userdata);

/* ============================================================
 *  VM management
 *
 *  Loading a VM is a single call that:
 *    1. Allocates a VmCpu struct from the bump arena
 *    2. Calls vm_init on it
 *    3. Allocates mailbox storage from the bump arena
 *    4. Initializes the VM's mailbox
 *    5. Calls vm_load to parse the ELF and set up regions
 *       (using the system's bump arena and shared region)
 *    6. Registers the VM with the scheduler
 *
 *  On any failure, partial state may be left in the bump arena
 *  (no rollback). For development-time loops where you might
 *  retry, vm_system_destroy + vm_system_init resets everything.
 * ============================================================ */

typedef enum {
    VM_SYS_OK = 0,
    VM_SYS_ERR_FULL,             /* scheduler at capacity */
    VM_SYS_ERR_NO_ARENA_SPACE,   /* bump arena exhausted */
    VM_SYS_ERR_LOAD_FAILED,      /* see load_result for details */
    VM_SYS_ERR_INVALID_ARG,
} VmSystemResult;

/* Load result detail — populated on VM_SYS_ERR_LOAD_FAILED so
 * the caller can distinguish ELF problems from system problems. */
typedef struct {
    VmSystemResult code;
    VmLoadResult   load_result;      /* meaningful iff code == LOAD_FAILED */
    int            assigned_vm_id;   /* >= 0 on success, -1 on error */
} VmLoadVmResult;

/* Load a VM using the system's default mailbox shape.
 *
 *   sys:               the system
 *   elf_image:         pointer to ELF bytes
 *   elf_size:          ELF size in bytes
 *   data_region_size:  bytes of region 2 (data + bss + stack + heap)
 *   code_backing:      VM_BACKING_XIP or VM_BACKING_COPY_RAM
 *   rodata_backing:    VM_BACKING_XIP or VM_BACKING_COPY_RAM
 *
 * Returns a result struct with the assigned vm_id on success or
 * an error code with details. */
VmLoadVmResult vm_system_load_vm(VmSystem *sys,
                                 const void *elf_image, size_t elf_size,
                                 uint32_t data_region_size,
                                 VmBacking code_backing,
                                 VmBacking rodata_backing);

/* Same as vm_system_load_vm but with caller-specified mailbox
 * shape (instead of the system default). Useful for VMs that
 * need to receive larger messages or have deeper queues. */
VmLoadVmResult vm_system_load_vm_with_mailbox(VmSystem *sys,
                                              const void *elf_image,
                                              size_t elf_size,
                                              uint32_t data_region_size,
                                              VmBacking code_backing,
                                              VmBacking rodata_backing,
                                              uint16_t mailbox_slot_size,
                                              uint16_t mailbox_depth);

/* Look up a VM's mailbox by ID. Used by the SYS_SEND handler to
 * find the recipient. Returns NULL if vm_id is invalid or
 * unregistered. */
VmMailbox *vm_system_get_mailbox(VmSystem *sys, uint16_t vm_id);

/* Unload a previously-loaded VM. Releases all of its slab-arena
 * allocations (data region, mailbox storage, copy-to-RAM code
 * and rodata, VmCpu struct), unregisters it from the scheduler,
 * and clears its entry in sys->vms[].
 *
 * The slab allocator's per-block free means each VM's resources
 * are independently reclaimable — you can unload VMs in any
 * order without fragmenting the arena. This is what makes the
 * multi-VM concurrent use case (window manager, program manager)
 * possible.
 *
 * Safe to call on a halted VM. Calling on a running VM marks it
 * halted first, but doesn't gracefully shut it down — guests
 * that need cleanup should handle SYS_EXIT themselves.
 *
 * Returns true on success, false if vm_id is invalid or the
 * slot is empty. */
bool vm_system_unload_vm(VmSystem *sys, uint16_t vm_id);

/* Compute the local-region storage size needed to support the
 * given config. Use this to size the caller-provided buffer:
 *
 *     size_t bytes = vm_system_local_required(max_vms, spawn_data_kb);
 *     static uint8_t local_region[/ * bytes * /];
 *
 * Because the buffer must be statically allocated for embedded
 * targets, this helper is primarily for understanding the math
 * rather than runtime sizing. The returned value is an upper
 * bound including slab overhead. */
size_t vm_system_local_required(uint16_t max_vms,
                                uint32_t spawn_data_kb);

/* ============================================================
 *  Execution
 *
 *  The main entry point an application's main() calls. Loops
 *  forever (or until all VMs halted, or cycle cap reached), driving
 *  the scheduler.
 * ============================================================ */

/* Run the system until all VMs halt or max_cycles scheduling
 * cycles have elapsed. Returns true if all VMs halted, false if
 * the cycle cap was hit first. Pass 0 for unbounded.
 *
 * Equivalent to vm_sched_run(sys->sched, max_cycles). */
bool vm_system_run(VmSystem *sys, uint64_t max_cycles);

/* Run a single scheduling cycle. Useful for tests and for
 * applications that want to interleave VM execution with their
 * own work between cycles. */
VmSchedStepResult vm_system_step(VmSystem *sys);

/* ============================================================
 *  Introspection — pass-through to subsystems
 * ============================================================ */

static inline uint32_t vm_system_ready_count(const VmSystem *sys) {
    return sys ? vm_sched_ready_count(sys->sched) : 0;
}

static inline uint32_t vm_system_blocked_count(const VmSystem *sys) {
    return sys ? vm_sched_blocked_count(sys->sched) : 0;
}

static inline size_t vm_system_shared_bytes_used(const VmSystem *sys) {
    return sys ? slab_bytes_used(sys->shared_slab) : 0;
}

static inline size_t vm_system_local_bytes_used(const VmSystem *sys) {
    return sys ? slab_bytes_used(sys->local_slab) : 0;
}

/* No peak tracking on the slab; this returned the bump arena's
 * high-water-mark previously. Slab returns current usage as
 * peak for now — the slab allocator could grow a real peak
 * field if it becomes useful. */
static inline size_t vm_system_local_bytes_peak(const VmSystem *sys) {
    return sys ? slab_bytes_used(sys->local_slab) : 0;
}

#endif /* VM_SYSTEM_H */
