/* ============================================================
 *  stream_ecalls.h — install SYS_STREAM_* handlers on a router
 *
 *  The handlers translate guest pointers, then forward to the
 *  global stream_arbiter (see stream_arbiter.h). Install once at
 *  runtime startup; the arbiter itself is initialized separately
 *  by stream_arbiter_init().
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef IO_STREAM_ECALLS_H
#define IO_STREAM_ECALLS_H

#include <stdbool.h>

#include "vm/vm_system.h"

#ifdef __cplusplus
extern "C" {
#endif

bool mgapi_install_stream_ecalls(VmSystem *sys);

#ifdef __cplusplus
}
#endif

#endif /* IO_STREAM_ECALLS_H */
