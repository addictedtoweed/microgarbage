/* ============================================================
 *  vm_mem_protect.h — tier-1 software VM-ownership hooks
 *
 *  Per-block memory protection for the SHARED region (region 3 +
 *  the L2 sub-region). Private segments (code/rodata/data) are
 *  per-VM and isolated by the interpreter's region bounds already;
 *  only the shared region is common address space, so only it needs
 *  an ownership check.
 *
 *  This is tier 1 of a three-tier scheme selected by what the target
 *  has (see config.h GARBAGE_MEM_PROTECT and docs/rpi-port.md):
 *    tier 0  — off; hooks compile out; zero cost (smallest micro)
 *    tier 1  — this software owner map (no MMU / TrustZone needed)
 *    tier 2/3— MMU (per-VM page ranges) / TrustZone (floor vs guests)
 *
 *  The call sites live in exactly two places — vm_core.c's xlat
 *  (so the interpreter AND every ecall shim inherit the check) and
 *  vm_system.c's SYS_ALLOC (owner stamp). They are compiled in only
 *  when GARBAGE_MEM_PROTECT is nonzero. The bodies below are
 *  PERMISSIVE PLACEHOLDERS until the owner side-table is wired, so
 *  enabling the flag today is a localized, behavior-preserving step.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_VM_MEM_PROTECT_H
#define MICROGARBAGE_VM_MEM_PROTECT_H

#include <stdint.h>
#include <stdbool.h>
#include "vm/vm_core.h"   /* VmCpu */

#ifdef __cplusplus
extern "C" {
#endif

/* May `cpu` access [addr, addr+size) in the SHARED region? Called
 * only for shared-region accesses (addr >= 0xC000_0000); private
 * segments never reach here. When the owner side-table lands this
 * will verify every 32-byte slot in the range is owned by, or granted
 * to, cpu->vm_id. Returns true today (permissive placeholder). */
bool vm_mem_shared_access_ok(const VmCpu *cpu, uint32_t addr,
                             uint32_t size, bool is_write);

/* Record `cpu` as owner of a freshly-allocated shared block at
 * `shared_off` (offset into shared_storage) spanning `size` bytes.
 * Will stamp cpu->vm_id across the 32-byte slots the block occupies.
 * No-op today (placeholder). */
void vm_mem_stamp_owner(const VmCpu *cpu, uint32_t shared_off,
                        uint32_t size);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_VM_MEM_PROTECT_H */
