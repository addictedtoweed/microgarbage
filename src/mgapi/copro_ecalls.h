/* ============================================================
 *  copro_ecalls.h — install the SYS_COPRO_* ecall handlers.
 *
 *  The five copro ecalls let a guest stage per-frame PPU payload
 *  + DMA descriptor list into the cart window, commit a frame, read
 *  the latest joypads, and wait for the SNES side to consume the
 *  frame.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_COPRO_ECALLS_H
#define MGAPI_COPRO_ECALLS_H

#include <stdbool.h>

#include "vm/vm_system.h"   /* VmSystem is an anonymous-struct typedef,
                             * so we need the full include — no fwd decl */

#ifdef __cplusplus
extern "C" {
#endif

bool mgapi_install_copro_ecalls(VmSystem *sys);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_COPRO_ECALLS_H */
