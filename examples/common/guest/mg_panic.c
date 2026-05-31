/* ============================================================
 *  mg_panic.c — guest-side panic stub.
 *  See mg_panic.h for the contract.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_panic.h"
#include "vm_runtime.h"

void mg_panic(const char *msg) {
    /* The handler is __attribute__((noreturn)) on the host side; this
     * loop is just to satisfy GCC's flow analysis if the handler
     * somehow returns (e.g. before the runtime wires the reset path). */
    _vm_sys1(SYS_MG_PANIC, (uint32_t)msg);
    for (;;) { }
}
