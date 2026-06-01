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

/* MG_FS_* op codes (multiplexed onto SYS_MG_FRAME_STATE). Must match
 * mg_frame.c and the host handler. */
#define MG_FS_PANIC_READ  5
#define MG_FS_PANIC_CTX   6

uint32_t mg_panic_read(void *buf, uint32_t cap) {
    return _vm_sys3(SYS_MG_FRAME_STATE,
                    MG_FS_PANIC_READ, (uint32_t)buf, cap);
}

MgResult mg_panic_ctx_read(MgPanicCtx *out) {
    return (MgResult)(int32_t)_vm_sys2(SYS_MG_FRAME_STATE,
                                       MG_FS_PANIC_CTX, (uint32_t)out);
}
