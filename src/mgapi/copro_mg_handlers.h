/* ============================================================
 *  copro_mg_handlers.h — host-side handlers for SYS_MG_* ecalls.
 *
 *  Implementation detail of mgapi.dll / libmgapi.
 *
 *  Install with mg_handlers_install(sys); call from inside
 *  mgapi_vm_init after vm_host_install_copro. Pairs with
 *  copro_mg_state.{h,c}.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_COPRO_MG_HANDLERS_H
#define MGAPI_COPRO_MG_HANDLERS_H

#include <stdbool.h>

#include "vm/vm_system.h"   /* VmSystem (tag-less typedef) */

#ifdef __cplusplus
extern "C" {
#endif

/* Register all 30 SYS_MG_* handlers on the given VmSystem's ecall
 * router. Returns true on success. On any registration failure,
 * any handlers already installed are unregistered before return. */
bool mg_handlers_install(VmSystem *sys);

/* Unregister. Idempotent. */
void mg_handlers_uninstall(VmSystem *sys);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_COPRO_MG_HANDLERS_H */
