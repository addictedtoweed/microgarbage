/* ============================================================
 *  mg_frame.c — guest-side frame pacing stubs.
 *  See mg_frame.h for the contract.
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_frame.h"
#include "vm_runtime.h"

void mg_wait_frame(void) {
    /* SYS_COPRO_WAIT_VBLANK is the existing 1184 — the runtime's
     * producer publishes a fresh frame and we return. */
    (void)_vm_sys0(SYS_COPRO_WAIT_VBLANK);
}

void mg_frame_commit(void) {
    /* SYS_COPRO_FRAME_COMMIT (1182) toggles the frame-ready byte the
     * SNES kernel polls. The runtime picks up the staging tables on
     * its next DMA prep tick. */
    (void)_vm_sys1(SYS_COPRO_FRAME_COMMIT, /*frame_ready=*/1);
}

/* Frame-state op codes; must match copro_mg_handlers.c. */
#define MG_FS_GET_SLOTS  0
#define MG_FS_GET_BYTES  1
#define MG_FS_GET_TOP    2
#define MG_FS_GET_BOT    3
#define MG_FS_SET_BLANK  4

uint8_t mg_frame_slots_remaining(void) {
    return (uint8_t)_vm_sys1(SYS_MG_FRAME_STATE, MG_FS_GET_SLOTS);
}

uint16_t mg_frame_bytes_remaining(void) {
    return (uint16_t)_vm_sys1(SYS_MG_FRAME_STATE, MG_FS_GET_BYTES);
}

uint8_t mg_force_blank_top(void) {
    return (uint8_t)_vm_sys1(SYS_MG_FRAME_STATE, MG_FS_GET_TOP);
}

uint8_t mg_force_blank_bottom(void) {
    return (uint8_t)_vm_sys1(SYS_MG_FRAME_STATE, MG_FS_GET_BOT);
}

void mg_force_blank(uint8_t top, uint8_t bottom) {
    (void)_vm_sys3(SYS_MG_FRAME_STATE, MG_FS_SET_BLANK, top, bottom);
}
