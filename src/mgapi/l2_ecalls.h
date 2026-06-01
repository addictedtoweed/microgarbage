/* ============================================================
 *  l2_ecalls.h — install the SYS_L2_* ecall handlers.
 *
 *  These hand the guest a real VA into the L2 sub-region of SHARED
 *  (0xE000_0000+). The VM's translation function dispatches L2 VAs
 *  into the PSRAM backing transparently, so the guest dereferences
 *  alloc_l2 pointers like any other guest pointer.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_L2_ECALLS_H
#define MGAPI_L2_ECALLS_H

#include <stdbool.h>
#include <stddef.h>

#include "vm/vm_system.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register the SYS_L2_* handlers. `l2_base` and `l2_size` describe
 * the system-wide PSRAM slice the upper half of SHARED maps to —
 * the handlers need them to convert between host pointers (what
 * the underlying l2_alloc gives them) and guest VAs (what the
 * guest sees). */
bool mgapi_install_l2_ecalls(VmSystem *sys, void *l2_base, size_t l2_size);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_L2_ECALLS_H */
