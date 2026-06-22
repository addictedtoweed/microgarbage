/* ============================================================
 *  fmv_ecalls.h — install SYS_FMV_* handlers on a router.
 *
 *  Handlers forward to the host FMV player (fmv_player.h). Install once
 *  at runtime startup; the stream arbiter must already be initialized.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_FMV_ECALLS_H
#define MGAPI_FMV_ECALLS_H

#include <stdbool.h>

#include "vm/vm_system.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mgapi_install_fmv_ecalls(VmSystem *sys);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_FMV_ECALLS_H */
