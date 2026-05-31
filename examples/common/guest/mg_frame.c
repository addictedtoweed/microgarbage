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
