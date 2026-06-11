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
    /* v2.29 Phase 3a: mg_force_blank is now a thin wrapper around
     * mg_kernel_layout. The old SYS_MG_FRAME_STATE/SET_BLANK path is
     * also called for back-compat with the legacy emit_inidisp_table
     * machinery — that path stays until Phase 3b cleanup, so demos
     * built against old vs new hosts both work during the transition. */
    (void)_vm_sys3(SYS_MG_FRAME_STATE, MG_FS_SET_BLANK, top, bottom);
    (void)_vm_sys2(SYS_MG_KERNEL_LAYOUT, top, bottom);
}

/* v2.29 Phase 3a: unified kernel layout — drives the kernel's built-in
 * HIRQ ISR to insert top/bottom force-blank regions via INIDISP
 * transitions. Replaces (and back-compat-wrapped by) mg_force_blank.
 * Effective range: 0..112 each (clamped by the host). */
void mg_kernel_layout(uint8_t top_lb, uint8_t bottom_lb) {
    (void)_vm_sys2(SYS_MG_KERNEL_LAYOUT, top_lb, bottom_lb);
}

/* v2.29 Phase 3a: configure per-scanline siphon. The kernel's HIRQ
 * ISR fires a CPU DMA each scanline during the visible region,
 * pulling `bytes_per_line` bytes from cart_window[src_off + N*bytes]
 * (where N is the scanline index relative to visible start) and
 * writing them to WRAM starting at wram_dst. Pass bytes=0 to disable. */
void mg_siphon_configure(uint8_t bytes_per_line, uint16_t src_off,
                         uint32_t wram_dst) {
    (void)_vm_sys3(SYS_MG_SIPHON_CONFIGURE,
                   (uint32_t)bytes_per_line, (uint32_t)src_off, wram_dst);
}
