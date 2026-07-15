/* ============================================================
 *  vm_mem_protect.c — tier-1 software VM-ownership map
 *  See vm/vm_mem_protect.h for the contract.
 *
 *  The map (a uint8 owner id per 32-byte shared slot) is carved from
 *  local_storage in vm_system_init and pointed at by each VmCpu at
 *  load. These functions are only *called* from GARBAGE_MEM_PROTECT
 *  gated sites (vm_core.c xlat, vm_system.c SYS_ALLOC/FREE/teardown),
 *  but are always compiled; they early-return when the map is absent.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_mem_protect.h"

/* 32-byte slot granularity (SLAB_MIN_BLOCK); slot = shared_offset >> 5. */
#define OWNER_SLOT_SHIFT  5u

bool vm_mem_shared_access_ok(const VmCpu *cpu, uint32_t addr,
                             uint32_t size, bool is_write) {
    (void)is_write;
    const uint8_t *map = cpu->shared_owner_map;
    if (!map || size == 0) return true;
    if (addr >= 0xE0000000u) return true;   /* L2 sub-region: not yet mapped */

    uint32_t off   = addr & 0x1FFFFFFFu;     /* offset into the shared region */
    uint32_t first = off >> OWNER_SLOT_SHIFT;
    uint32_t last  = (off + size - 1u) >> OWNER_SLOT_SHIFT;
    if (last >= cpu->shared_owner_slots) return true;  /* bounds handled upstream */

    uint8_t me = (uint8_t)cpu->vm_id;
    for (uint32_t s = first; s <= last; s++) {
        uint8_t o = map[s];
        if (o != me && o != VM_OWNER_UNOWNED) return false;  /* another VM's live block */
    }
    return true;
}

void vm_mem_stamp_owner(const VmCpu *cpu, uint32_t shared_off,
                        uint32_t block_bytes) {
    uint8_t *map = cpu->shared_owner_map;
    if (!map || block_bytes == 0) return;
    uint32_t first  = shared_off >> OWNER_SLOT_SHIFT;
    uint32_t nslots = (block_bytes + 31u) >> OWNER_SLOT_SHIFT;
    uint8_t  me     = (uint8_t)cpu->vm_id;
    for (uint32_t s = first; s < first + nslots && s < cpu->shared_owner_slots; s++)
        map[s] = me;
}

void vm_mem_clear_owner(const VmCpu *cpu, uint32_t shared_off,
                        uint32_t block_bytes) {
    uint8_t *map = cpu->shared_owner_map;
    if (!map || block_bytes == 0) return;
    uint32_t first  = shared_off >> OWNER_SLOT_SHIFT;
    uint32_t nslots = (block_bytes + 31u) >> OWNER_SLOT_SHIFT;
    for (uint32_t s = first; s < first + nslots && s < cpu->shared_owner_slots; s++)
        map[s] = VM_OWNER_UNOWNED;
}

void vm_mem_release_vm(const VmCpu *cpu) {
    uint8_t *map = cpu->shared_owner_map;
    if (!map) return;
    uint8_t me = (uint8_t)cpu->vm_id;
    for (uint32_t s = 0; s < cpu->shared_owner_slots; s++)
        if (map[s] == me) map[s] = VM_OWNER_UNOWNED;
}
