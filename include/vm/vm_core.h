/* ============================================================
 *  vm_core.h — RISC-V RV32IMC virtual machine core
 *
 *  A small, portable interpreter for RV32IMC. Targeted at M0/M3/
 *  M4/M7-class hosts but builds and runs anywhere standard C does.
 *  The "core" is just the CPU state plus the step function; loader,
 *  ECALL routing, scheduling, and mailboxes live in sibling modules
 *  (vm_loader.h, vm_ecall.h, vm_sched.h, vm_mailbox.h).
 *
 *  ---------------------------------------------------------------
 *  Why RISC-V
 *  ---------------------------------------------------------------
 *
 *  Toolchain reuse. LLVM, lld, GDB, objdump, and addr2line all
 *  support RV32 out of the box. Programs are built with a stock
 *  riscv32-unknown-elf toolchain; debugging uses standard GDB
 *  against a remote serial stub (see vm_debug.h, if/when built).
 *
 *  Compressed instructions (C extension) are mandatory here: the
 *  default code emission for rv32imc is ~30% smaller than rv32im,
 *  and the dispatcher handles compressed encoding directly rather
 *  than requiring -mno-c. Multiply/divide (M extension) is also
 *  mandatory; no software-emulated MUL/DIV. The base set is I.
 *
 *  Not included: F/D (float/double), A (atomics), privilege levels
 *  beyond machine mode (no S/U/H, no CSRs except a small handful
 *  documented below). The VM is single-mode, single-hart per
 *  instance, no MMU.
 *
 *  ---------------------------------------------------------------
 *  Address space
 *  ---------------------------------------------------------------
 *
 *  The top 2 bits of every 32-bit virtual address select a region:
 *
 *    00xx_xxxx  region 0  CODE       executable, immutable at run
 *    01xx_xxxx  region 1  RODATA     read-only data and init values
 *    10xx_xxxx  region 2  DATA       per-VM private RAM
 *    11xx_xxxx  region 3  SHARED     cross-VM shared memory
 *
 *  Each region's base pointer and length are set by the loader.
 *  The lower 30 bits of an address are an offset into the chosen
 *  region; the offset is bounds-checked against the region's
 *  length on every access. Out-of-bounds traps to the host.
 *
 *  Regions 0 and 1 may be backed by flash (XIP) or RAM (copy on
 *  load). Region 2 is always RAM, per-VM. Region 3 is always RAM,
 *  shared across all VMs in the system, and only addressable via
 *  offsets handed out by the host's SYS_ALLOC ECALL.
 *
 *  Each region can be at most 1 GB (2^30 bytes), the natural limit
 *  of the 2-bit-region/30-bit-offset split. In practice each region
 *  is sized to the smallest power-of-2 that fits its content;
 *  bounds checking is a single compare per access.
 *
 *  Writes to region 0 (CODE) and region 1 (RODATA) trap as store
 *  faults. Execution from regions 2 or 3 is not currently trapped
 *  (the dispatcher will happily fetch from any region whose bytes
 *  decode as valid instructions); if this matters, the loader can
 *  point region 0 at a checksummed buffer and refuse to set up VMs
 *  whose code lies elsewhere.
 *
 *  ---------------------------------------------------------------
 *  Dispatcher strategy
 *  ---------------------------------------------------------------
 *
 *  Switch-on-opcode, plain portable C. No computed goto, no direct
 *  threading, no JIT. The outer switch is on the 7-bit major opcode;
 *  nested switches refine by funct3/funct7 where needed. GCC and
 *  Clang generate a TBB/TBH (table-branch) on M3/M4/M7 and a
 *  conditional-branch tree on M0; both are within 5-10% of any
 *  threaded alternative on these cores, and the portable C version
 *  builds cleanly under -Wpedantic and debugs without GNU-extension
 *  surprises.
 *
 *  Compressed (16-bit) instructions are handled by expand-then-
 *  execute: fetch 16 bits, inspect the quadrant, either expand to
 *  the equivalent 32-bit encoding or treat as the low half of a
 *  32-bit instruction. Each RISC-V operation's semantics live in
 *  exactly one place regardless of how it was encoded.
 *
 *  Throughput SWAGs (back-of-envelope, NOT measured — assume a
 *  worst-case mix, region access on every other instruction,
 *  -O2 -flto, no per-CPU tuning):
 *
 *    M0  @  50 MHz:  0.5 - 1.5 MIPS    (no hardware divide)
 *    M3  @  72 MHz:  2 - 5 MIPS
 *    M4  @ 100 MHz:  3 - 8 MIPS
 *    M7  @ 200 MHz:  10 - 20 MIPS
 *
 *  These are guesses pending real benchmarks. They exist to set
 *  rough expectations, not commitments. Replace with measurements
 *  once the dispatcher and a representative workload exist.
 *
 *  ---------------------------------------------------------------
 *  Step quanta
 *  ---------------------------------------------------------------
 *
 *  The dispatcher runs for a caller-specified instruction budget
 *  then returns control. This is the foundation of the cooperative
 *  scheduler in vm_sched.h: each VM gets a budget, runs that many
 *  instructions or until it traps/yields/halts, then the scheduler
 *  picks the next VM. The core itself has no concept of "time" —
 *  steps are the only unit of progress.
 *
 *  Single-host-thread by design. A VmCpu is not safe to share
 *  across threads, but a system with multiple VmCpus can be driven
 *  from a single host thread using the scheduler. The shared-region
 *  allocator (in vm_ecall.h) uses a no-op locker for this reason;
 *  if you ever drive the same VmCpu from multiple host threads, you
 *  will need to add locking everywhere and this comment is wrong.
 *
 *  Critical sections: a guest can request a non-preemptible run via
 *  SYS_CRITICAL_ENTER / SYS_CRITICAL_EXIT (see vm_ecall.h). The core
 *  itself is unaware of this — it always runs to budget or trap.
 *  The scheduler reads cpu->in_critical after each vm_step and, if
 *  set, re-calls vm_step rather than rotating to the next VM. Over-
 *  run instructions are tracked as scheduler-internal debt and
 *  debited from future quanta to maintain fairness. The full policy
 *  lives in vm_sched.h; the CPU struct holds only the flag.
 *
 *  ---------------------------------------------------------------
 *  Traps
 *  ---------------------------------------------------------------
 *
 *  When the dispatcher hits something it can't or shouldn't execute,
 *  it sets trap state on the CPU and returns from vm_step with a
 *  VM_STEP_TRAPPED result. The trap state captures cause, faulting
 *  PC, faulting address (for load/store faults), and the raw
 *  instruction bits (for illegal-instruction). The host inspects
 *  this state, decides what to do, and either resumes the VM (after
 *  fixing up state) or terminates it.
 *
 *  This is deliberately not the real-RISC-V trap model. There is no
 *  guest trap vector, no mtvec, no mcause-in-CSR, no return-from-
 *  interrupt instruction. Guests cannot catch their own exceptions.
 *  The model maps cleanly onto GDB's remote serial protocol: a trap
 *  becomes a stop-reply (T05 for SIGTRAP on ebreak/breakpoint, T0B
 *  for SIGSEGV on bad memory, T04 for SIGILL on illegal instr).
 *  The debug stub (vm_debug.h, if/when built) is a thin marshaler
 *  on top of the same trap state the host sees.
 *
 *  ECALL is NOT a trap in this model. It is a normal dispatcher
 *  exit with cause TRAP_ECALL, intended to be handled by the host's
 *  ECALL router (vm_ecall.h) and resumed at PC+4. The "trap" naming
 *  is uniform but the semantics are routine.
 *
 *  ebreak IS a trap (cause TRAP_BREAKPOINT). The debug stub uses
 *  this for software breakpoints: GDB writes an ebreak at the
 *  desired PC, the dispatcher traps, the stub reports the stop,
 *  GDB restores the original instruction for single-step or
 *  resume. If you are not using the debug stub, ebreak still traps
 *  and the host can treat it as a "panic" or "assertion failed"
 *  marker from guest code.
 *
 *  Misaligned loads and stores trap as LOAD_MISALIGNED / STORE_
 *  MISALIGNED. The RISC-V spec permits either trapping or emulation;
 *  this VM traps. Stock compilers don't generate misaligned accesses
 *  under normal flags, so this is rarely an issue in practice.
 *
 *  ---------------------------------------------------------------
 *  CSRs
 *  ---------------------------------------------------------------
 *
 *  Only a handful of CSRs are implemented. Most reads return 0,
 *  most writes are silently ignored, and CSR access via csrr*
 *  instructions is supported syntactically. The supported CSRs:
 *
 *    cycle    (0xC00) — running instruction counter, low 32
 *    cycleh   (0xC80) — running instruction counter, high 32
 *    instret  (0xC02) — same as cycle (one instr = one "cycle")
 *    instreth (0xC82) — same as cycleh
 *    mhartid  (0xF14) — returns the VM's vm_id (0..N)
 *
 *  Unknown CSR reads return 0 without trapping. Unknown CSR writes
 *  are ignored without trapping. This is a deliberate compatibility
 *  shim: stock crt0 startup code touches a few CSRs unconditionally
 *  on real hardware; we make those harmless rather than ratholing
 *  on full CSR emulation.
 *
 *  ---------------------------------------------------------------
 *  ECALL ABI
 *  ---------------------------------------------------------------
 *
 *  Standard RISC-V Linux convention, reused for system call into
 *  the host:
 *
 *    a7   — syscall number
 *    a0   — argument 0 / return value
 *    a1   — argument 1
 *    a2   — argument 2
 *    a3   — argument 3
 *    a4   — argument 4
 *    a5   — argument 5
 *
 *  After the host services the call, the dispatcher resumes at the
 *  instruction following ecall (PC + 4 for a 32-bit ecall; ecall
 *  has no compressed encoding so the increment is always 4).
 *
 *  Syscall numbers and per-syscall ABI live in vm_ecall.h. The core
 *  knows nothing about specific syscalls; it just routes them.
 *
 *  ---------------------------------------------------------------
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_CORE_H
#define VM_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ============================================================
 *  Constants
 * ============================================================ */

/* Number of memory regions. Fixed at 4 (2 bits of address selector). */
#define VM_REGION_COUNT  4

/* Region indices. The values are also the top-2-bits of any
 * address in that region. */
#define VM_REGION_CODE    0u   /* 0x0000_0000 - 0x3FFF_FFFF */
#define VM_REGION_RODATA  1u   /* 0x4000_0000 - 0x7FFF_FFFF */
#define VM_REGION_DATA    2u   /* 0x8000_0000 - 0xBFFF_FFFF */
#define VM_REGION_SHARED  3u   /* 0xC000_0000 - 0xFFFF_FFFF */

/* Number of general-purpose registers in RV32. x0 is hardwired
 * zero; the dispatcher writes through to regs[0] but always
 * re-zeros it before the next read (or, equivalently, treats
 * writes to x0 as no-ops). Choose whichever is faster on your
 * target; the implementation comment in vm_core.c explains. */
#define VM_GP_REGS  32

/* Register name aliases (ABI names) for use by callers. The core
 * itself indexes regs[] by number; these are convenience macros
 * for ECALL handlers and tests. */
#define VM_REG_ZERO  0
#define VM_REG_RA    1
#define VM_REG_SP    2
#define VM_REG_GP    3
#define VM_REG_TP    4
#define VM_REG_T0    5
#define VM_REG_T1    6
#define VM_REG_T2    7
#define VM_REG_S0    8     /* also FP (frame pointer) */
#define VM_REG_S1    9
#define VM_REG_A0   10
#define VM_REG_A1   11
#define VM_REG_A2   12
#define VM_REG_A3   13
#define VM_REG_A4   14
#define VM_REG_A5   15
#define VM_REG_A6   16
#define VM_REG_A7   17
#define VM_REG_S2   18
#define VM_REG_S3   19
#define VM_REG_S4   20
#define VM_REG_S5   21
#define VM_REG_S6   22
#define VM_REG_S7   23
#define VM_REG_S8   24
#define VM_REG_S9   25
#define VM_REG_S10  26
#define VM_REG_S11  27
#define VM_REG_T3   28
#define VM_REG_T4   29
#define VM_REG_T5   30
#define VM_REG_T6   31

/* ============================================================
 *  Result codes from vm_step
 *
 *  Every call to vm_step ends in exactly one of these. The host's
 *  scheduler reads the result and decides what to do next.
 * ============================================================ */

typedef enum {
    /* The instruction budget for this call was used up. The VM is
     * still runnable; pc and regs are in a consistent state at the
     * next instruction. Call vm_step again to continue. */
    VM_STEP_QUANTUM_EXPIRED = 0,

    /* The VM hit an ECALL. trap_cause is TRAP_ECALL; trap_pc is
     * the address of the ecall instruction. The host's ECALL
     * router should service the call (reading args from regs[a0..a5]
     * and the syscall number from regs[a7]), write any return
     * value to regs[a0], and either resume (with pc already
     * advanced past the ecall by the dispatcher) or mark the VM
     * blocked/halted/etc. */
    VM_STEP_ECALL,

    /* The VM hit an ebreak, an illegal instruction, a bad memory
     * access, or a misaligned access. trap_cause distinguishes.
     * trap_pc points at the faulting instruction (not advanced).
     * trap_addr and trap_insn are populated where relevant. The
     * host decides whether to terminate the VM, surface the trap
     * to a debugger, or attempt to recover. */
    VM_STEP_TRAPPED,

    /* The VM executed a clean exit (typically SYS_EXIT ECALL,
     * which the ECALL router can synthesize into this result by
     * setting cpu->halted = true before returning from the
     * handler). The VM is no longer runnable; subsequent vm_step
     * calls return immediately. */
    VM_STEP_HALTED,
} VmStepResult;

/* ============================================================
 *  Trap causes
 *
 *  When a VmStepResult of VM_STEP_TRAPPED or VM_STEP_ECALL is
 *  returned, cpu->trap_cause is one of these.
 * ============================================================ */

typedef enum {
    TRAP_NONE = 0,

    /* Synchronous exception causes. */
    TRAP_ILLEGAL_INSTR,        /* unrecognized encoding             */
    TRAP_LOAD_FAULT,           /* load address out of bounds        */
    TRAP_STORE_FAULT,          /* store address out of bounds       */
    TRAP_LOAD_MISALIGNED,      /* load address not naturally aligned */
    TRAP_STORE_MISALIGNED,     /* store address not naturally aligned */
    TRAP_STORE_RO,             /* store into CODE or RODATA region  */
    TRAP_INSTR_FETCH_FAULT,    /* fetch from out-of-bounds PC       */
    TRAP_INSTR_MISALIGNED,     /* branch target with bad alignment  */

    /* Cooperative exits — not really "errors". */
    TRAP_ECALL,                /* environment call to host          */
    TRAP_BREAKPOINT,           /* ebreak instruction                */
    TRAP_HALT,                 /* clean exit set by ECALL handler   */

    /* Debugger-injected. */
    TRAP_WATCHPOINT,           /* load/store hit a watch addr       */
    TRAP_SINGLESTEP,           /* one-instruction step completed    */
} VmTrapCause;

/* ============================================================
 *  Block reasons (scheduler-visible)
 *
 *  Set by ECALL handlers when a syscall determines the VM should
 *  pause until some condition is met. The scheduler reads this
 *  after dispatch and moves the VM out of the ready set into the
 *  blocked set.
 *
 *  The wake-up condition is implicit in the reason:
 *    BLOCK_YIELDED       — VM relinquished the rest of its
 *                          quantum but is otherwise runnable;
 *                          wakes on the next scheduler cycle.
 *    BLOCK_MAILBOX_RECV  — wakes when a message arrives in this
 *                          VM's mailbox, OR when block_deadline
 *                          is reached (whichever first).
 *    BLOCK_SLEEP         — wakes when block_deadline is reached.
 *
 *  The scheduler maintains a global step-tick counter that
 *  increments by the number of instructions retired across all
 *  VMs each quantum. block_deadline is a snapshot of that tick
 *  counter + the requested timeout; the scheduler wakes a VM
 *  when the global tick has caught up.
 *
 *  Like in_critical, this field is the scheduler's domain. The
 *  core dispatcher never reads or writes it. ECALL handlers
 *  write it; the scheduler clears it on wake-up (and writes the
 *  appropriate result to regs[a0] before doing so).
 * ============================================================ */

typedef enum {
    BLOCK_NONE = 0,
    BLOCK_YIELDED,           /* SYS_YIELD — wake next cycle      */
    BLOCK_MAILBOX_RECV,      /* SYS_RECV waiting for a message   */
    BLOCK_SLEEP,             /* SYS_SLEEP waiting for ticks      */
    BLOCK_ON_CHILD,          /* SYS_SPAWN_AND_WAIT — wakes when   *
                              * the spawned child VM halts        */
} VmBlockReason;

/* ============================================================
 *  Memory region descriptor
 *
 *  The loader fills four of these into the VmCpu. The dispatcher
 *  uses them on every load, store, and fetch:
 *
 *    addr = some user address (32-bit)
 *    region = addr >> 30
 *    offset = addr & 0x3FFFFFFF
 *    if offset + access_size > regions[region].length: trap
 *    actual_host_ptr = regions[region].base + offset
 *
 *  base may point into flash (XIP) or RAM. Writability is governed
 *  by the writable flag, not by where base points; writes to a
 *  non-writable region trap as TRAP_STORE_RO regardless of whether
 *  the underlying memory could physically tolerate the write.
 *
 *  length is the size of the backing storage. It does NOT have to
 *  be 1 GB even though the region's address space is 1 GB; an
 *  out-of-bounds offset traps, which is the desired behavior.
 *
 *  Setting base to NULL and length to 0 marks the region as absent:
 *  any access to it traps. The loader uses this for systems that
 *  don't have a shared region, or VMs that don't need rodata, etc.
 * ============================================================ */

typedef struct {
    uint8_t *base;       /* host pointer to the region's backing storage */
    uint32_t length;     /* size of backing storage in bytes (0 = absent) */
    bool     writable;   /* CODE/RODATA: false. DATA/SHARED: true.        */
} VmRegion;

/* ============================================================
 *  CPU state
 *
 *  Caller declares one VmCpu per virtual machine. The struct holds
 *  all execution state — register file, PC, region descriptors,
 *  trap state, counters. There is no separate "buffer" the user
 *  supplies; embedding the register file in the struct lets the
 *  compiler reach all of it through a single base pointer (the
 *  cpu argument), which matters in the hot dispatch loop.
 *
 *  Public read-only fields are documented as such. The rest are
 *  internal; do not read or write them outside the VM modules.
 * ============================================================ */

typedef struct VmCpu {
    /* === Architectural state — read freely, write only via core fns === */

    uint32_t regs[VM_GP_REGS];   /* x0..x31. x0 should always read 0. */
    uint32_t pc;                 /* program counter                    */

    /* === Memory regions — set by the loader === */

    VmRegion regions[VM_REGION_COUNT];

    /* === Trap state — populated on every VM_STEP_TRAPPED/ECALL === */

    VmTrapCause trap_cause;
    uint32_t    trap_pc;         /* address of faulting instruction    */
    uint32_t    trap_addr;       /* faulting memory address, if any    */
    uint32_t    trap_insn;       /* raw instruction bits, if illegal   */

    /* === Halt latch === */

    /* Set by the ECALL router when servicing SYS_EXIT (or by the
     * host directly to forcibly stop the VM). When true, vm_step
     * returns VM_STEP_HALTED immediately on the next call. */
    bool halted;

    /* === Critical section flag (scheduler-visible) === */

    /* True while the VM is inside a SYS_CRITICAL_ENTER /
     * SYS_CRITICAL_EXIT pair. Set and cleared exclusively by the
     * ECALL router (vm_ecall.h). The core dispatcher does NOT
     * read or write this flag — budget expiry is unconditional in
     * vm_step. The scheduler reads this after vm_step returns and
     * decides whether to rotate to another VM or immediately
     * re-call with another budget.
     *
     * Policy summary (implemented by the scheduler, not the core):
     *
     *   - Quantum expiry while in_critical is set: scheduler
     *     re-calls vm_step instead of rotating. The over-run is
     *     tracked as scheduler-internal debt and debited from
     *     this VM's future quanta to maintain fairness.
     *
     *   - Debt is amortized across several future quanta rather
     *     than applied all to the next one, to avoid starvation
     *     when a VM does many short critical sections in a row.
     *
     *   - Scheduler caps the maximum critical-section duration to
     *     bound damage from runaway sections (guest bugs). Past
     *     the cap, the VM is force-terminated.
     *
     *   - Blocking ECALLs while in_critical is set are rejected
     *     by their handlers (a blocked critical section breaks
     *     the atomicity guarantee that motivated the section).
     *
     *   - On any trap, the scheduler clears in_critical when
     *     routing the trap — a trapped critical section is
     *     implicitly ended.
     *
     * The layering rule: the core treats this flag as opaque
     * state for the scheduler. Keeping the core unaware of
     * scheduling policy means the dispatcher hot path doesn't
     * pay for critical-section bookkeeping it doesn't need. */
    bool in_critical;

    /* === Block reason (scheduler-visible) === */

    /* Why this VM is paused. BLOCK_NONE means the VM is runnable
     * (or halted; check halted first). Any non-zero value tells
     * the scheduler to move this VM out of the ready set into
     * the blocked set, and how to know when to wake it.
     *
     * Set by ECALL handlers; cleared by the scheduler on wake-up.
     * The core dispatcher never reads or writes this. */
    VmBlockReason block_reason;

    /* Tick value (snapshot of the scheduler's global step counter)
     * at which a timed block expires. Only meaningful when
     * block_reason is BLOCK_MAILBOX_RECV or BLOCK_SLEEP. Use
     * UINT32_MAX for "no timeout, block indefinitely" (only
     * sensible for BLOCK_MAILBOX_RECV). */
    uint32_t block_deadline;

    /* When block_reason == BLOCK_ON_CHILD, the vm_id of the spawned
     * child this VM is waiting on. The child's exit path (in
     * vm_system_unload_vm) finds the waiting parent by scanning for
     * this value, delivers the child's exit code into the parent's
     * a0, and clears the parent's block. UINT16_MAX when not waiting. */
    uint16_t block_child_vm;

    /* === Periodic auto-reload timer (per-VM, optional) ===
     *
     * Backs SYS_SET_RELOAD_PERIOD / SYS_YIELD_UNTIL_RELOAD.
     * When the guest sets a period, the kernel advances
     * reload_next_deadline by that period on each yield, hiding
     * the per-frame arithmetic from the guest:
     *
     *     sys_set_reload_period(125);       // 125 ms
     *     while (running) {
     *         sys_yield_until_reload();      // wakes at next tick boundary
     *         do_frame();
     *     }
     *
     * Catch-up policy (FreeRTOS-style): if do_frame() runs over
     * budget, the kernel skips ahead to the next FUTURE boundary
     * rather than firing the missed events back-to-back. So one
     * slow frame drops a tick but keeps the phase regular —
     * essential for animations and any cadence that must align
     * with external time.
     *
     * Zero in either field means "not currently in reload mode".
     * Set by SYS_SET_RELOAD_PERIOD; cleared by passing period=0.
     * Untouched by any other syscall — SYS_SLEEP_* and the
     * reload mechanism are independent. */
    uint32_t reload_period;
    uint32_t reload_next_deadline;

    /* === Identity (set by the host at creation time) === */

    /* Visible to guest as mhartid CSR. Also used by mailbox and
     * scheduler as the canonical per-VM identifier. */
    uint16_t vm_id;

    /* === Public read-only counters === */

    /* Total instructions executed across all vm_step calls for this
     * VM. Wraps at 2^64. Visible to guest via the cycle/instret
     * CSR pair (low 32 in cycle, high 32 in cycleh). */
    uint64_t instructions_retired;

    /* Total traps taken (any cause). Useful for diagnostics. */
    uint32_t trap_count;

    /* Total ECALLs serviced. Distinct from trap_count because
     * ECALL is the routine exit, not an error. */
    uint32_t ecall_count;

    /* === Internal === */

    /* Reserved for future use by the dispatcher (cached decode
     * pointers, last-fetched PC for fast re-fetch on the same
     * instruction, etc.). Implementations may add fields here
     * without breaking the public API as long as VmCpu remains a
     * type the caller declares directly.
     *
     * Treat this as opaque; do not read or write. */
    uint32_t _internal[5];

} VmCpu;

/* ============================================================
 *  ECALL handler callback
 *
 *  When the dispatcher decodes an ecall, it advances PC past the
 *  ecall and returns VM_STEP_ECALL. The host's scheduler is
 *  expected to call the ECALL router (vm_ecall.h), which dispatches
 *  to handlers of this signature.
 *
 *  Handlers read arguments from cpu->regs[VM_REG_A0..A5] and the
 *  syscall number from cpu->regs[VM_REG_A7], and write the return
 *  value to cpu->regs[VM_REG_A0]. The 'system' pointer is the
 *  caller-supplied context (typically pointing to the VmSystem
 *  struct from vm_system.h, giving the handler access to the
 *  shared slab, mailboxes, etc.).
 *
 *  Returning from a handler resumes the VM at PC (already advanced
 *  past the ecall). To block the VM (e.g., on SYS_RECV with no
 *  pending message), the handler sets state in the scheduler
 *  side-band and the scheduler skips this VM until unblocked.
 *
 *  The core does not call these handlers itself; this typedef
 *  lives here so that vm_ecall.h and the ECALL router share a
 *  signature without inverting the include order. */

typedef void (*VmEcallHandler)(VmCpu *cpu, void *system);

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Initialize a VmCpu to a known-zero state. Clears registers, PC,
 * regions, trap state, halted flag, counters. Sets vm_id to the
 * given value.
 *
 * After init, the VM is not yet runnable — it has no code. Call
 * vm_load (from vm_loader.h) to populate the regions before
 * stepping. */
void vm_init(VmCpu *cpu, uint16_t vm_id);

/* Reset architectural state (registers, PC, trap state, counters)
 * without touching region descriptors. Useful for "restart this
 * VM with the same loaded program." */
void vm_reset(VmCpu *cpu);

/* ============================================================
 *  Execution
 * ============================================================ */

/* Run the VM for up to `budget` instructions or until it traps,
 * ECALLs, or halts.
 *
 *   cpu     — the VM to run
 *   budget  — maximum instructions to execute this call
 *   out_steps — if non-NULL, receives the number of instructions
 *               actually retired in this call. May be less than
 *               budget if the VM trapped/exited early.
 *
 * Returns one of VmStepResult. The cpu's trap_cause / trap_pc /
 * trap_addr / trap_insn are populated for VM_STEP_TRAPPED and
 * VM_STEP_ECALL results.
 *
 * Safe to call on a halted VM (returns VM_STEP_HALTED immediately,
 * no steps consumed). Safe to call with budget = 0 (returns
 * VM_STEP_QUANTUM_EXPIRED with zero steps consumed). */
VmStepResult vm_step(VmCpu *cpu, uint32_t budget, uint32_t *out_steps);

/* ============================================================
 *  Memory access helpers (public — for ECALL handlers, debuggers,
 *  and tests; the dispatcher uses internal inlined versions)
 *
 *  These do the same region-decode + bounds-check the dispatcher
 *  does, and return a host pointer on success. On out-of-bounds
 *  or wrong-direction (write to read-only) access, they return
 *  NULL and set trap state on the CPU.
 *
 *  The dispatcher uses internal versions of these for performance;
 *  the public versions are for non-hot-path callers (ECALL
 *  handlers reading a struct out of guest memory, debugger reads,
 *  test code).
 * ============================================================ */

/* Translate a guest address + size into a host pointer for reading.
 * Returns NULL if the access would fault. Sets trap state on
 * failure. size must be > 0. */
const void *vm_translate_read(VmCpu *cpu, uint32_t addr, uint32_t size);

/* Translate a guest address + size into a host pointer for writing.
 * Returns NULL if the access would fault or if the region is
 * read-only. Sets trap state on failure. size must be > 0. */
void *vm_translate_write(VmCpu *cpu, uint32_t addr, uint32_t size);

/* Read a 32-bit word from guest memory. Returns 0 and sets trap
 * state if the access would fault. Callers that care about the
 * difference between "guest stored 0" and "fault" should check
 * cpu->trap_cause after the call. */
uint32_t vm_read_u32(VmCpu *cpu, uint32_t addr);

/* Write a 32-bit word to guest memory. No-op on fault; sets trap
 * state. Returns true on success. */
bool vm_write_u32(VmCpu *cpu, uint32_t addr, uint32_t value);

/* Convenience for ECALL handlers that need to copy structures
 * to/from guest memory. Does region check, returns false on
 * fault. */
bool vm_copy_from_guest(VmCpu *cpu, void *dst, uint32_t guest_src, uint32_t n);
bool vm_copy_to_guest  (VmCpu *cpu, uint32_t guest_dst, const void *src, uint32_t n);

/* ============================================================
 *  Introspection (direct field access, no functions)
 *
 *  Users can read cpu->pc, cpu->regs[i], cpu->instructions_retired,
 *  etc. directly. These accessors exist for explicitness and
 *  symmetric API style.
 * ============================================================ */

static inline uint32_t vm_get_pc(const VmCpu *cpu) {
    return cpu ? cpu->pc : 0;
}

static inline uint32_t vm_get_reg(const VmCpu *cpu, int i) {
    if (!cpu || i < 0 || i >= VM_GP_REGS) return 0;
    return i == 0 ? 0 : cpu->regs[i];
}

static inline uint64_t vm_instructions_retired(const VmCpu *cpu) {
    return cpu ? cpu->instructions_retired : 0;
}

static inline bool vm_is_halted(const VmCpu *cpu) {
    return cpu ? cpu->halted : true;
}

#endif /* VM_CORE_H */
