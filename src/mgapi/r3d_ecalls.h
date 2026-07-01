/* ============================================================
 *  r3d_ecalls.h — install SYS_R3D_* handlers on a router.
 *
 *  Handlers forward to the native 3D renderer service (copro_r3d.h).
 *  Install once at runtime startup.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_R3D_ECALLS_H
#define MGAPI_R3D_ECALLS_H

#include <stdbool.h>

#include "vm/vm_system.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mgapi_install_r3d_ecalls(VmSystem *sys);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_R3D_ECALLS_H */
