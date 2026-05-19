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
#include "memory/slab_stack.h"
#include "memory/bump.h"

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

} VmSystemConfig;

/* ============================================================
 *  VmSystem
 *
 *  Caller declares one. The internal arrays are sized to
 *  VM_SCHED_MAX_VMS. The fields below "Public read-only" can be
 *  inspected; the rest are internal.
 * ============================================================ */

typedef struct {
    /* === Public read-only — pointers to internal subsystems === */

    /* The ECALL router. Use vm_ecall_register on this to install
     * additional or override standard handlers. */
    VmEcallRouter *ecall_router;

    /* The scheduler. Use vm_sched_* on this for introspection. */
    VmSched *sched;

    /* The shared-region slab. Use slab_* on this to inspect usage. */
    SlabAllocator *shared_slab;

    /* The bump arena for VM-local storage. Use bump_used / _peak /
     * _remaining to track headroom. */
    BumpAllocator *local_arena;

    /* === Public read-only — config snapshot === */
    VmSystemConfig config;

    /* === Internal — owned subsystem instances === */

    /* The subsystems are stored by value so the public pointers
     * above are stable and the caller doesn't have to manage
     * lifetimes. */
    VmEcallRouter _ecall_router;
    VmSched       _sched;
    SlabAllocator _shared_slab;
    BumpAllocator _local_arena;

    /* === Per-VM state ===
     *
     * Indexed by vm_id. Slot is "in use" iff vms[i] is non-NULL
     * (mirrors the scheduler's registration state). */

    /* CPU structs are allocated from the bump arena at load time
     * and the pointers stored here. */
    VmCpu *vms[VM_SCHED_MAX_VMS];

    /* Per-VM mailboxes. The VmMailbox struct itself lives here
     * (no extra indirection); its storage buffer comes from the
     * bump arena. */
    VmMailbox mailboxes[VM_SCHED_MAX_VMS];

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
    return sys ? bump_used(sys->local_arena) : 0;
}

static inline size_t vm_system_local_bytes_peak(const VmSystem *sys) {
    return sys ? bump_peak(sys->local_arena) : 0;
}

#endif /* VM_SYSTEM_H */
