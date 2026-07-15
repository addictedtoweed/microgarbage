/* ============================================================
 *  vm_mem_protect.h — tier-1 software VM-ownership map
 *
 *  Per-block memory protection for the SHARED region (region 3).
 *  Private code/rodata/data segments are per-VM and isolated by the
 *  interpreter's region bounds already; only the shared region is
 *  common address space, so only it needs an ownership check.
 *
 *  Tier 1 of a three-tier scheme selected by target capability (see
 *  config.h GARBAGE_MEM_PROTECT and docs/rpi-port.md):
 *    tier 0  — off; hooks compile out; zero cost (smallest micro)
 *    tier 1  — this software owner map (no MMU / TrustZone needed)
 *    tier 2/3— MMU (per-VM page ranges) / TrustZone (floor vs guests)
 *
 *  The map is a side table of one owner byte per 32-byte (SLAB_MIN_BLOCK)
 *  shared slot, indexed by (shared_offset >> 5). SYS_ALLOC stamps the
 *  owning vm_id across a block's slots; SYS_FREE and VM teardown clear
 *  them. The access check runs inside vm_core.c's xlat, so the
 *  interpreter AND every ecall shim inherit it from one place.
 *
 *  v1 semantics: DENY access to a slot owned by a DIFFERENT live VM;
 *  ALLOW own slots and UNOWNED slots. (Denying UNOWNED — catching
 *  cross-VM use-after-free — is a later tightening once a grant
 *  primitive exists; see SYS_MEM_GRANT in docs/rpi-port.md.)
 *
 *  vm_id fits in a byte: VM_SCHED_MAX_VMS (64) < VM_OWNER_UNOWNED.
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

/* Sentinel owner for a slot no live VM owns. */
#define VM_OWNER_UNOWNED  0xFFu

/* May `cpu` access [addr, addr+size) in the SHARED region? Called only
 * for shared-region accesses (addr >= 0xC000_0000); private segments
 * never reach here. Returns true when protection storage is absent, for
 * the L2 sub-region (not yet mapped), and for own/UNOWNED slots. */
bool vm_mem_shared_access_ok(const VmCpu *cpu, uint32_t addr,
                             uint32_t size, bool is_write);

/* Stamp `cpu` as owner of a shared block at `shared_off` (offset into
 * shared_storage) spanning `block_bytes` (the slab block size, a 32-byte
 * multiple). No-op when protection storage is absent. */
void vm_mem_stamp_owner(const VmCpu *cpu, uint32_t shared_off,
                        uint32_t block_bytes);

/* Clear ownership of a freed shared block back to UNOWNED. */
void vm_mem_clear_owner(const VmCpu *cpu, uint32_t shared_off,
                        uint32_t block_bytes);

/* Sweep the whole map, releasing every slot owned by cpu->vm_id.
 * Called on VM teardown so a reused vm_id never inherits stale slots. */
void vm_mem_release_vm(const VmCpu *cpu);

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_VM_MEM_PROTECT_H */
