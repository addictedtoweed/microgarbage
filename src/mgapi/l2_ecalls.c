/* ============================================================
 *  l2_ecalls.c — SYS_L2_ALLOC / SYS_L2_FREE / SYS_L2_STATS.
 *
 *  The handlers thinly wrap the L2 allocator (l2_init.h) and
 *  translate host pointers <-> guest VAs. The convention:
 *
 *      guest VA = 0xE000_0000 + (host_pa - l2_base)
 *      host PA  = l2_base + (guest_va - 0xE000_0000)
 *
 *  All VMs see the same L2 backing — the VM's translation maps any
 *  L2 VA into this region — so a pointer returned to one VM by
 *  alloc_l2 IS dereferenceable from every other VM that's still
 *  alive. We trust the user-space l2_alloc to coordinate the carve.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "l2_ecalls.h"

#include "l2_init.h"
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* L2 VA range start, mirroring the split in vm_core.c's xlat_pick. */
#define L2_VA_BASE   0xE0000000u

static uint8_t *g_l2_base;
static size_t   g_l2_size;

static inline uint32_t host_to_va(const void *p) {
    if (!p || !g_l2_base) return 0;
    uintptr_t off = (uintptr_t)p - (uintptr_t)g_l2_base;
    if (off >= g_l2_size) return 0;
    return L2_VA_BASE + (uint32_t)off;
}

static inline void *va_to_host(uint32_t va) {
    if (!g_l2_base) return NULL;
    if ((va & 0xE0000000u) != L2_VA_BASE) return NULL;
    uint32_t off = va & 0x1FFFFFFFu;
    if (off >= g_l2_size) return NULL;
    return g_l2_base + off;
}

/* SYS_L2_ALLOC(size, align) → guest VA or 0 (OOM) */
static void h_l2_alloc(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t size  = cpu->regs[VM_REG_A0];
    uint32_t align = cpu->regs[VM_REG_A1];

    void *p = mgapi_l2_host_alloc(size, align);
    cpu->regs[VM_REG_A0] = host_to_va(p);
    /* If host_to_va returned 0 (alloc failed or out-of-range), the
     * guest reads 0 and treats it as OOM. */
}

/* SYS_L2_FREE(guest_va) → 0 */
static void h_l2_free(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t va = cpu->regs[VM_REG_A0];
    if (va != 0) {
        void *p = va_to_host(va);
        if (p) mgapi_l2_host_free(p);
    }
    cpu->regs[VM_REG_A0] = 0;
}

/* The guest-visible stats record (versioned, fields appended at the
 * tail in future). Returned by SYS_L2_STATS into a guest buffer. */
typedef struct {
    uint32_t version;          /* = 1                                   */
    uint32_t reserved;         /* padding to 8-byte align               */
    uint64_t total_bytes;      /* = pool size                           */
    uint64_t used_bytes;
    uint64_t free_bytes;
    uint64_t largest_free_bytes;
    uint32_t alloc_count;
    uint32_t free_block_count;
} GuestL2Stats;

/* SYS_L2_STATS(out_buf) → bytes_written or -EINVAL/-EFAULT */
static void h_l2_stats(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t guest_buf = cpu->regs[VM_REG_A0];

    void *dst = vm_translate_write(cpu, guest_buf, sizeof(GuestL2Stats));
    if (!dst) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EFAULT;
        return;
    }

    MgapiL2Stats s;
    if (mgapi_l2_stats(&s) != 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EAGAIN;
        return;
    }

    GuestL2Stats out = {
        .version             = 1,
        .reserved            = 0,
        .total_bytes         = (uint64_t)g_l2_size,
        .used_bytes          = (uint64_t)s.used_bytes,
        .free_bytes          = (uint64_t)s.free_bytes,
        .largest_free_bytes  = (uint64_t)s.largest_free_bytes,
        .alloc_count         = s.alloc_count,
        .free_block_count    = s.free_block_count,
    };
    memcpy(dst, &out, sizeof out);
    cpu->regs[VM_REG_A0] = (uint32_t)sizeof out;
}

/* ----------------------------------------------------------------
 *  Installation
 * ---------------------------------------------------------------- */

bool mgapi_install_l2_ecalls(VmSystem *sys, void *l2_base, size_t l2_size) {
    if (!sys || !sys->ecall_router) return false;
    g_l2_base = (uint8_t *)l2_base;
    g_l2_size = l2_size;

    VmEcallRouter *r = sys->ecall_router;
    if (!vm_ecall_register(r, SYS_L2_ALLOC, h_l2_alloc)) return false;
    if (!vm_ecall_register(r, SYS_L2_FREE,  h_l2_free))  return false;
    if (!vm_ecall_register(r, SYS_L2_STATS, h_l2_stats)) return false;
    return true;
}
