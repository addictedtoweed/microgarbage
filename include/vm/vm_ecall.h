/* ============================================================
 *  vm_ecall.h — host-side ECALL routing and the system call ABI
 *
 *  When a guest executes the ecall instruction, the dispatcher
 *  (vm_core.h) returns VM_STEP_ECALL with PC already advanced past
 *  the ecall and the trap state populated. The scheduler then
 *  invokes vm_ecall_dispatch, which looks up a7 in a table of
 *  registered handlers and calls the matching one.
 *
 *  This header defines:
 *    - The syscall number assignments (SYS_*)
 *    - The register ABI for each syscall (in per-call doc comments)
 *    - The error codes handlers may return (VM_E*)
 *    - The handler-registration table API
 *    - The dispatcher entry point
 *
 *  ---------------------------------------------------------------
 *  Register ABI
 *  ---------------------------------------------------------------
 *
 *  Standard RISC-V Linux convention:
 *
 *    a7   syscall number
 *    a0   argument 0       (and return value on success/error)
 *    a1   argument 1
 *    a2   argument 2
 *    a3   argument 3
 *    a4   argument 4
 *    a5   argument 5
 *
 *  Handlers read arguments from cpu->regs[VM_REG_A0..A5] and write
 *  the return value to cpu->regs[VM_REG_A0]. All other registers
 *  are preserved across the syscall (the handler must not modify
 *  a1..a6 or any other register unless the call's contract says
 *  otherwise — and currently no call does).
 *
 *  Returns are signed 32-bit values:
 *    - Non-negative value: success. Meaning is per-call (often 0,
 *      sometimes a count, sometimes a sender ID, etc.).
 *    - Negative value: error. The negated value matches a Linux
 *      errno (see VM_E* below). Guest code can detect errors with
 *      a single signed comparison (a0 < 0).
 *
 *  ---------------------------------------------------------------
 *  Why this ABI shape
 *  ---------------------------------------------------------------
 *
 *  The shape (a7=number, a0..a5=args, a0=return, negated errno on
 *  error) is the actual Linux RISC-V convention. Reusing it means:
 *
 *    - Inline-asm guest stubs look exactly like real Linux syscall
 *      stubs. Anyone who has written Linux syscall wrappers in
 *      RISC-V asm can read these without surprises.
 *
 *    - picolibc, newlib, and similar libc implementations have
 *      pluggable syscall backends. Pointing them at our handlers
 *      is mostly a no-op for the calls they recognize.
 *
 *    - The "negated errno" convention plays nicely with the
 *      classic userspace wrapper pattern:
 *
 *          long r = sys_foo(...);
 *          if (r < 0) { errno = -r; return -1; }
 *          return r;
 *
 *  We do NOT use Linux's actual syscall numbers for VM-specific
 *  calls. SYS_EXIT alone matches Linux (93) so that stock crt0
 *  exit code works without patching. Everything else lives in a
 *  high range (1024+) that will never collide with a Linux number.
 *  Unknown syscalls (numbers with no registered handler) return
 *  -ENOSYS, matching real Linux behavior.
 *
 *  ---------------------------------------------------------------
 *  Handler model
 *  ---------------------------------------------------------------
 *
 *  Handlers are pure functions of (cpu, system). They:
 *
 *    1. Read arguments from cpu->regs[a0..a5].
 *    2. Do their work.
 *    3. Write a return value (success or negated errno) to
 *       cpu->regs[a0].
 *    4. Optionally set scheduler-visible state (block_reason,
 *       halted, etc.) on the CPU if the call has scheduler
 *       implications. The scheduler reads this after the handler
 *       returns and acts accordingly.
 *    5. Return.
 *
 *  Handlers never directly suspend, sleep, or modify the
 *  scheduler's state. They communicate scheduling intent via flags
 *  on the CPU. This keeps the handler API trivial and the
 *  scheduler in charge of all policy.
 *
 *  Handlers are registered at boot via vm_ecall_register. The
 *  table is fixed-size (VM_ECALL_TABLE_SIZE) and shared across all
 *  VMs in the system — every VM dispatches through the same table.
 *  A fallback handler is called for any number with no registered
 *  entry; the default fallback returns -ENOSYS.
 *
 *  ---------------------------------------------------------------
 *  Threading
 *  ---------------------------------------------------------------
 *
 *  Single host thread by design (see vm_core.h). Only one handler
 *  runs at a time, across all VMs in the system. Handlers may
 *  freely read and write shared state (slab allocator, mailbox
 *  list, scheduler queues) without locking.
 *
 *  ---------------------------------------------------------------
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_ECALL_H
#define VM_ECALL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "vm/vm_core.h"   /* for VmCpu, VmEcallHandler */

/* ============================================================
 *  Syscall numbers
 *
 *  SYS_EXIT matches Linux RISC-V (93) so that stock crt0 exit
 *  paths work without patching. Everything else is VM-specific
 *  and lives in the 1024+ range to avoid any Linux collision.
 *
 *  Numbers within a subsystem are grouped by 16 to leave room
 *  for additions without renumbering.
 * ============================================================ */

/* --- Linux-compatible (kept at Linux number for toolchain compat) --- */
#define SYS_EXIT              93   /* clean exit (matches Linux RISC-V) */

/* --- Identity / introspection (1024..1039) --- */
#define SYS_SELF            1024   /* get this VM's ID */

/* --- Cooperative scheduling (1040..1055) --- */
#define SYS_YIELD           1040   /* relinquish remainder of quantum */
#define SYS_CRITICAL_ENTER  1041   /* begin non-preemptible region */
#define SYS_CRITICAL_EXIT   1042   /* end non-preemptible region */

/* --- Shared-region allocator (1056..1071) --- */
#define SYS_ALLOC           1056   /* allocate from shared region */
#define SYS_FREE            1057   /* free to shared region */

/* --- Mailbox messaging (1072..1087) --- */
#define SYS_SEND            1072   /* send message to a VM's mailbox */
#define SYS_RECV            1073   /* receive a message from own mailbox */
#define SYS_MAILBOX_INFO    1074   /* query a target VM's mailbox shape */
#define SYS_WHITELIST_ADD   1075   /* allow a sender to message us */
#define SYS_WHITELIST_REMOVE 1076  /* revoke a sender's permission */

/* --- libc memory/string acceleration (1088..1119, 32 slots) ---
 *
 * These exist so the guest's libc can implement memcpy/memcmp/
 * strlen/etc. as a thin ecall wrapper. The host runs a native
 * implementation, which is dramatically faster than the guest
 * interpreting an instruction-at-a-time loop. The crossover
 * point where the ecall pays for itself is small (a few dozen
 * bytes); below that the guest's compiled loop would win, so
 * libc wrappers can short-circuit small sizes if they care.
 *
 * All pointers are guest addresses; the host translates and
 * bounds-checks before doing the operation. Operations that
 * straddle region boundaries are rejected as -EFAULT — each
 * call must operate within a single region. */
#define SYS_MEMCPY          1088   /* (dst, src, n) → 0 or -err */
#define SYS_MEMSET          1089   /* (dst, byte, n) → 0 or -err */
#define SYS_MEMMOVE         1090   /* (dst, src, n) → 0 or -err */
#define SYS_MEMCMP          1091   /* (a, b, n) → sign(memcmp) or -err */
#define SYS_STRLEN          1092   /* (s, maxlen) → length or -err */
#define SYS_STRCMP          1093   /* (a, b, maxlen) → sign(strcmp) or -err */
#define SYS_STRCHR          1094   /* (s, c, maxlen) → offset or -ENOENT */

/* --- Fixed-point math acceleration (1120..1151, 32 slots) ---
 *
 * The guest's working numeric format is fixed-point (see
 * math/fixed_point.h). These syscalls accelerate transcendental
 * functions on q16_16_t values — operations the guest's own
 * fixed_point module doesn't provide because a guest-side
 * implementation would be a polynomial-approximation loop
 * dozens of instructions long, interpreted slowly.
 *
 * Inputs and outputs are q16_16_t passed as int32_t in a0 (and
 * sometimes a1 for two-argument calls). The host implements
 * these however it likes — using hardware double-precision via
 * <math.h>, using a fixed-point CORDIC, or a lookup table — and
 * the guest sees only fixed-point in, fixed-point out.
 *
 * No floats cross the ABI: floats only appear in the optional
 * conversion syscalls below, which exist for the rare case
 * where a guest must consume float-formatted external data. */
#define SYS_FIX_SIN         1120   /* angle (q16_16) → sin */
#define SYS_FIX_COS         1121   /* angle (q16_16) → cos */
#define SYS_FIX_TAN         1122   /* angle (q16_16) → tan */
#define SYS_FIX_ATAN2       1123   /* (y, x) q16_16 → atan2 */
#define SYS_FIX_SQRT        1124   /* value (q16_16, >=0) → sqrt */
#define SYS_FIX_EXP         1125   /* value (q16_16) → e^value */
#define SYS_FIX_LOG         1126   /* value (q16_16, >0) → ln(value) */
#define SYS_FIX_POW         1127   /* (base, exp) q16_16 → base^exp */

/* Format conversions for external-data boundaries. Most guests
 * don't need these — compile-time float constants should be
 * expressed via Q16_16_FROM_DOUBLE() in source, which evaluates
 * to a baked-in integer constant with no runtime conversion.
 * These syscalls are for the runtime case (parsing a float from
 * a file, receiving an IEEE-formatted sample over a wire, etc.). */
#define SYS_FLOAT_TO_FIX    1128   /* IEEE single in a0 → q16_16 */
#define SYS_FIX_TO_FLOAT    1129   /* q16_16 in a0 → IEEE single */
#define SYS_DOUBLE_TO_FIX   1130   /* IEEE double (a0:a1) → q16_16 */
#define SYS_FIX_TO_DOUBLE   1131   /* q16_16 → IEEE double (a0:a1) */

/* ============================================================
 *  Per-syscall ABI documentation
 *
 *  Format: arguments in a0..a5 (only those actually used), return
 *  in a0. "→" marks return values. Errors are always negative
 *  numbers matching the VM_E* errno values defined below.
 *
 *  -----------------------------------------------------------
 *  SYS_EXIT          (a7 = 93)
 *  -----------------------------------------------------------
 *    a0 = exit code (informational; not currently propagated
 *                    beyond setting cpu->halted = true)
 *    → does not return (cpu->halted is set, next vm_step returns
 *      VM_STEP_HALTED)
 *
 *  -----------------------------------------------------------
 *  SYS_SELF          (a7 = 1024)
 *  -----------------------------------------------------------
 *    (no arguments)
 *    → a0 = this VM's vm_id (always non-negative)
 *
 *  -----------------------------------------------------------
 *  SYS_YIELD         (a7 = 1040)
 *  -----------------------------------------------------------
 *    (no arguments)
 *    → a0 = 0 always
 *    Side effect: the scheduler should treat this as "remainder
 *    of quantum is voluntarily relinquished" and rotate to the
 *    next VM. The handler sets cpu->block_reason = BLOCK_YIELDED.
 *
 *  -----------------------------------------------------------
 *  SYS_CRITICAL_ENTER  (a7 = 1041)
 *  -----------------------------------------------------------
 *    (no arguments)
 *    → a0 = 0 on success
 *    → a0 = -EBUSY if already in a critical section (no nesting)
 *    Side effect: sets cpu->in_critical = true. The scheduler
 *    will not rotate away from this VM at quantum expiry until
 *    SYS_CRITICAL_EXIT (or a trap) clears the flag.
 *
 *  -----------------------------------------------------------
 *  SYS_CRITICAL_EXIT   (a7 = 1042)
 *  -----------------------------------------------------------
 *    (no arguments)
 *    → a0 = 0 on success
 *    → a0 = -EINVAL if not currently in a critical section
 *    Side effect: clears cpu->in_critical.
 *
 *  -----------------------------------------------------------
 *  SYS_ALLOC         (a7 = 1056)
 *  -----------------------------------------------------------
 *    a0 = size in bytes (must be > 0 and <= max bin size)
 *    → a0 = guest address in shared region (always 0xC0000000 +
 *           offset, never NULL on success)
 *    → a0 = -ENOMEM if the slab can't satisfy the request
 *    → a0 = -EINVAL if size is 0 or unreasonably large
 *
 *  -----------------------------------------------------------
 *  SYS_FREE          (a7 = 1057)
 *  -----------------------------------------------------------
 *    a0 = guest address previously returned by SYS_ALLOC
 *    → a0 = 0 on success
 *    → a0 = -EINVAL if address is not in the shared region or
 *           does not correspond to a live allocation (double-free,
 *           foreign pointer, corrupted header)
 *
 *  -----------------------------------------------------------
 *  SYS_SEND          (a7 = 1072)
 *  -----------------------------------------------------------
 *    a0 = target vm_id
 *    a1 = guest address of payload (in sender's region 10)
 *    a2 = payload size in bytes (must match target's slot_size)
 *    → a0 = 0 on success
 *    → a0 = -ENOENT  if target vm_id does not exist
 *    → a0 = -EPERM   if sender is not on target's whitelist
 *    → a0 = -EINVAL  if size != target's slot_size
 *    → a0 = -EFAULT  if payload pointer is not in valid memory
 *    → a0 = -EAGAIN  if target's mailbox is full (reject-on-full)
 *
 *  -----------------------------------------------------------
 *  SYS_RECV          (a7 = 1073)
 *  -----------------------------------------------------------
 *    a0 = guest address to write payload to (in this VM's
 *         region 10; must have at least slot_size bytes available)
 *    a1 = timeout in step-quanta (0 = poll only, UINT32_MAX = wait
 *         forever)
 *    → a0 = sender's vm_id (non-negative, ≤ 65535) on success
 *    → a0 = -EAGAIN    if timeout=0 and mailbox is empty
 *    → a0 = -ETIMEDOUT if timeout > 0 and elapsed before a message
 *                       arrived
 *    → a0 = -EFAULT    if destination pointer is not in valid
 *                       memory
 *    → a0 = -EBUSY     if called while in a critical section with
 *                       timeout > 0 (blocking inside critical is
 *                       forbidden; use timeout=0 to poll)
 *    Side effect (only when blocking and not immediately satisfied):
 *    the handler sets cpu->block_reason = BLOCK_MAILBOX_RECV and
 *    stores enough state on the CPU for the scheduler / mailbox
 *    delivery code to write the result when a message arrives or
 *    the timeout expires.
 *
 *  -----------------------------------------------------------
 *  SYS_MAILBOX_INFO  (a7 = 1074)
 *  -----------------------------------------------------------
 *    a0 = target vm_id
 *    → a0 = slot_size of target's mailbox in bytes (positive)
 *    → a0 = -ENOENT if target does not exist
 *    → a0 = -EPERM  if caller is not on target's whitelist
 *    → a1 = current depth available in target's mailbox (free
 *           slots), useful for "should I bother sending"
 *    Note: a1 is meaningful only on success.
 *
 *  -----------------------------------------------------------
 *  SYS_WHITELIST_ADD     (a7 = 1075)
 *  SYS_WHITELIST_REMOVE  (a7 = 1076)
 *  -----------------------------------------------------------
 *    a0 = sender vm_id to allow / revoke
 *    → a0 = 0 on success
 *    → a0 = -ENOENT if sender vm_id is out of range
 *    Whitelist is per-receiver (i.e., this VM). Modifies only
 *    its own permissions, not the sender's.
 *
 *  -----------------------------------------------------------
 *  SYS_MEMCPY          (a7 = 1088)
 *  -----------------------------------------------------------
 *    a0 = dst guest address
 *    a1 = src guest address
 *    a2 = n bytes
 *    → a0 = 0 on success
 *    → a0 = -EFAULT if either range is invalid or straddles regions
 *    Overlapping regions are not defined (use SYS_MEMMOVE).
 *
 *  -----------------------------------------------------------
 *  SYS_MEMSET          (a7 = 1089)
 *  -----------------------------------------------------------
 *    a0 = dst guest address
 *    a1 = byte value (only low 8 bits used)
 *    a2 = n bytes
 *    → a0 = 0 on success
 *    → a0 = -EFAULT on bad range
 *
 *  -----------------------------------------------------------
 *  SYS_MEMMOVE         (a7 = 1090)
 *  -----------------------------------------------------------
 *    a0 = dst guest address
 *    a1 = src guest address
 *    a2 = n bytes
 *    → a0 = 0 on success
 *    → a0 = -EFAULT on bad range
 *    Handles overlap correctly (uses host memmove).
 *
 *  -----------------------------------------------------------
 *  SYS_MEMCMP          (a7 = 1091)
 *  -----------------------------------------------------------
 *    a0 = a guest address
 *    a1 = b guest address
 *    a2 = n bytes
 *    → a0 = -1, 0, or 1 (sign of memcmp result)
 *    → a0 = -EFAULT on bad range
 *    Note: clamped to -1/0/1 rather than the raw memcmp delta
 *    so the return value never collides with the -EFAULT range.
 *
 *  -----------------------------------------------------------
 *  SYS_STRLEN          (a7 = 1092)
 *  -----------------------------------------------------------
 *    a0 = s guest address
 *    a1 = maxlen (search bound, prevents runaway on missing NUL)
 *    → a0 = string length in bytes (0 to maxlen-1)
 *    → a0 = maxlen if no NUL found within maxlen bytes
 *    → a0 = -EFAULT if s..s+maxlen is not in valid memory
 *
 *  -----------------------------------------------------------
 *  SYS_STRCMP          (a7 = 1093)
 *  -----------------------------------------------------------
 *    a0 = a guest address
 *    a1 = b guest address
 *    a2 = maxlen (search bound on both strings)
 *    → a0 = -1, 0, or 1 (sign of strcmp result, clamped)
 *    → a0 = -EFAULT on bad range
 *    Stops at min(strlen(a), strlen(b), maxlen).
 *
 *  -----------------------------------------------------------
 *  SYS_STRCHR          (a7 = 1094)
 *  -----------------------------------------------------------
 *    a0 = s guest address
 *    a1 = character to find (only low 8 bits used)
 *    a2 = maxlen (search bound)
 *    → a0 = offset from s where character was found (0 to maxlen-1)
 *    → a0 = -ENOENT if not found within maxlen
 *    → a0 = -EFAULT if range is invalid
 *    Returns an OFFSET, not an absolute address, so the result
 *    can't collide with the negative error range.
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_SIN / COS / TAN     (a7 = 1120 / 1121 / 1122)
 *  -----------------------------------------------------------
 *    a0 = angle (q16_16, radians)
 *    → a0 = sin / cos / tan of angle (q16_16)
 *    Wraps modulo 2*pi internally; any finite input is accepted.
 *    SYS_FIX_TAN: undefined behavior near pi/2 + k*pi (host
 *    decides; typically returns saturated q16_16 max or min).
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_ATAN2       (a7 = 1123)
 *  -----------------------------------------------------------
 *    a0 = y (q16_16)
 *    a1 = x (q16_16)
 *    → a0 = atan2(y, x) (q16_16, radians, in [-pi, pi])
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_SQRT        (a7 = 1124)
 *  -----------------------------------------------------------
 *    a0 = value (q16_16, must be >= 0)
 *    → a0 = sqrt(value) (q16_16)
 *    → a0 = -EINVAL if value < 0
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_EXP         (a7 = 1125)
 *  -----------------------------------------------------------
 *    a0 = value (q16_16)
 *    → a0 = e^value (q16_16, saturated to q16_16 max on overflow)
 *    The overflow point is around value ≈ 10.4 in q16_16.
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_LOG         (a7 = 1126)
 *  -----------------------------------------------------------
 *    a0 = value (q16_16, must be > 0)
 *    → a0 = ln(value) (q16_16)
 *    → a0 = -EINVAL if value <= 0
 *
 *  -----------------------------------------------------------
 *  SYS_FIX_POW         (a7 = 1127)
 *  -----------------------------------------------------------
 *    a0 = base (q16_16)
 *    a1 = exp  (q16_16)
 *    → a0 = base^exp (q16_16, saturated on overflow)
 *    → a0 = -EINVAL if base < 0 and exp is not an integer
 *           (matches host pow() behavior on negative base)
 *
 *  -----------------------------------------------------------
 *  SYS_FLOAT_TO_FIX / SYS_FIX_TO_FLOAT    (1128 / 1129)
 *  -----------------------------------------------------------
 *    a0 = IEEE single (bit pattern) OR q16_16
 *    → a0 = q16_16 OR IEEE single (bit pattern)
 *    Out-of-range float-to-fix saturates to q16_16 min/max.
 *    NaN converts to 0 (debatable choice; documented as such).
 *
 *  -----------------------------------------------------------
 *  SYS_DOUBLE_TO_FIX / SYS_FIX_TO_DOUBLE  (1130 / 1131)
 *  -----------------------------------------------------------
 *    Same as the single-precision pair but the double is split
 *    across a0:a1 (low 32 bits in a0, high 32 bits in a1) per
 *    the standard RV32 calling convention for 64-bit values.
 *    Out-of-range and NaN: same handling as the single version.
 * ============================================================ */

/* ============================================================
 *  Error codes (negated errno values)
 *
 *  Returned in a0 as negative numbers. Values match the canonical
 *  Linux errno values so familiar names work as expected and so
 *  picolibc's errno-mapping code is happy.
 *
 *  This is NOT the full Linux errno set — just the ones any
 *  handler actually returns. New errors added here as new
 *  handlers need them.
 * ============================================================ */

#define VM_EPERM           1   /* not permitted (whitelist denial) */
#define VM_ENOENT          2   /* no such entity (vm_id, address)  */
#define VM_EAGAIN         11   /* try again later (mailbox full, poll empty) */
#define VM_ENOMEM         12   /* out of memory (slab exhausted)   */
#define VM_EFAULT         14   /* bad address (out-of-bounds ptr)  */
#define VM_EBUSY          16   /* resource busy (block-in-critical) */
#define VM_EINVAL         22   /* invalid argument                  */
#define VM_ENOSYS         38   /* function not implemented          */
#define VM_ETIMEDOUT     110   /* operation timed out               */

/* ============================================================
 *  Handler table
 *
 *  The dispatcher uses two flat arrays of 256 slots each, covering
 *  the two ranges of syscall numbers we care about:
 *
 *    linux_slots[0..255]:   numbers 0..255 (Linux-compat range;
 *                           today only SYS_EXIT at 93 is used)
 *    vm_slots[0..255]:      numbers 1024..1279 (our VM-specific
 *                           range; current syscalls all fit here,
 *                           with plenty of headroom for additions)
 *
 *  Any other syscall number routes to the fallback handler, which
 *  defaults to returning -ENOSYS in a0.
 *
 *  Memory cost: 512 slots × sizeof(VmEcallHandler) → 2 KB on a
 *  32-bit host. Dispatch cost: one range check, one array index,
 *  one indirect call. No hashing, no chain walks, no allocations.
 *
 *  The two ranges were chosen because they cleanly separate two
 *  classes of syscall:
 *    - low numbers: Linux-compatible (SYS_EXIT must be 93 for
 *      stock crt0; other Linux numbers can be added if you ever
 *      want to plug into existing libc syscall stubs unchanged).
 *    - 1024+ numbers: VM-specific, no risk of collision with
 *      Linux numbers should the libc-compat surface grow.
 *
 *  If you need numbers outside these two ranges, route via the
 *  fallback handler and dispatch yourself; the router does not
 *  support arbitrary numbers by design.
 *
 *  The per-handler signature is VmEcallHandler from vm_core.h:
 *  void (*)(VmCpu*, void*). The second parameter is the 'system'
 *  pointer passed to vm_ecall_dispatch, NOT a per-slot context —
 *  every handler in this router sees the same system pointer.
 *  Handlers that need additional state should reach it through
 *  the system pointer (typically a VmSystem struct holding the
 *  slab allocator, mailbox table, scheduler queues, etc.).
 * ============================================================ */

#define VM_ECALL_LINUX_RANGE_START      0
#define VM_ECALL_LINUX_RANGE_SIZE     256   /* covers 0..255       */
#define VM_ECALL_VM_RANGE_START      1024
#define VM_ECALL_VM_RANGE_SIZE        256   /* covers 1024..1279   */

typedef struct {
    VmEcallHandler  linux_slots[VM_ECALL_LINUX_RANGE_SIZE];
    VmEcallHandler  vm_slots   [VM_ECALL_VM_RANGE_SIZE];
    VmEcallHandler  fallback;  /* called for any other number     */
} VmEcallRouter;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Initialize a router. Clears all slots; sets fallback to a
 * default that writes -ENOSYS to a0. Safe to call on a struct
 * that has not been zero-initialized. */
void vm_ecall_router_init(VmEcallRouter *r);

/* Register a handler for the given syscall number. The handler
 * receives (cpu, system) where 'system' is the pointer passed to
 * vm_ecall_dispatch (typically a VmSystem struct holding all
 * shared state). There is no per-slot context — every handler
 * sees the same system pointer.
 *
 * syscall_num must be in one of the two supported ranges (0..255
 * or 1024..1279). Numbers outside these ranges return false.
 *
 * Returns false if syscall_num is out of range or if a handler
 * is already registered (use vm_ecall_unregister first). */
bool vm_ecall_register(VmEcallRouter *r,
                       uint32_t syscall_num,
                       VmEcallHandler handler);

/* Unregister. Returns false if syscall_num is out of range or
 * no handler is registered. Safe to call on empty slots. */
bool vm_ecall_unregister(VmEcallRouter *r, uint32_t syscall_num);

/* Override the fallback handler (called for any number with no
 * registered handler, or any number outside the two supported
 * ranges). The default writes -ENOSYS to a0. */
void vm_ecall_set_fallback(VmEcallRouter *r, VmEcallHandler fallback);

/* ============================================================
 *  Dispatch
 *
 *  Call this from the scheduler when vm_step returns VM_STEP_ECALL.
 *  Reads cpu->regs[a7], looks up the handler, calls it with the
 *  cpu and system pointers. The handler reads args from
 *  cpu->regs[a0..a5] and writes the result to cpu->regs[a0].
 *
 *  'system' is the caller-defined system context. By convention
 *  it points to the VmSystem struct from vm_system.h (when that
 *  exists). The router itself does not interpret it; it just
 *  forwards it to the handler.
 *
 *  After dispatch, the scheduler should inspect:
 *    - cpu->halted          — exit requested?
 *    - cpu->in_critical     — critical region entered/exited?
 *    - cpu->block_reason    — should this VM be blocked? (defined
 *                             elsewhere; not vm_ecall's concern)
 *  and act accordingly. The router itself does NOT touch these.
 * ============================================================ */

void vm_ecall_dispatch(VmEcallRouter *r, VmCpu *cpu, void *system);

#endif /* VM_ECALL_H */
