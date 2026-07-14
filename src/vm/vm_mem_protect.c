/* ============================================================
 *  vm_mem_protect.c — tier-1 software VM-ownership (placeholders)
 *  See vm/vm_mem_protect.h for the contract.
 *
 *  These bodies are intentionally permissive/no-op today: the call
 *  sites exist (vm_core.c xlat, vm_system.c SYS_ALLOC) so turning on
 *  GARBAGE_MEM_PROTECT is a localized change, but the owner
 *  side-table is not wired yet, so behavior is unchanged.
 *
 *  When implemented, the side-table is sized to the SHARED region
 *  only, one owner id per 32-byte slot (SLAB_MIN_BLOCK). See the
 *  TODOs below and docs/rpi-port.md.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_mem_protect.h"

bool vm_mem_shared_access_ok(const VmCpu *cpu, uint32_t addr,
                             uint32_t size, bool is_write) {
    (void)cpu; (void)addr; (void)size; (void)is_write;
    /* TODO(mem-protect): map[(addr - shared_base) >> 5] must equal
     * cpu->vm_id (or be granted to it) for every 32-byte slot in
     * [addr, addr+size). Return false to trap on a violation. */
    return true;
}

void vm_mem_stamp_owner(const VmCpu *cpu, uint32_t shared_off,
                        uint32_t size) {
    (void)cpu; (void)shared_off; (void)size;
    /* TODO(mem-protect): stamp cpu->vm_id across the 32-byte slots the
     * block occupies (shared_off .. shared_off+size, block-rounded). */
}
