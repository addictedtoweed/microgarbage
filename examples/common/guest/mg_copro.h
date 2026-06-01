/* ============================================================
 *  mg_copro.h — guest-side wrappers for the cart-coprocessor ecalls.
 *
 *  A guest game uses these to drive the SNES side: stage a per-
 *  frame PPU payload into the cart window, fill in DMA descriptor
 *  slots, commit the frame so the SNES NMI walks the list, read the
 *  4 joypads' latest snapshot, and (later) block until the SNES
 *  reports the frame was consumed.
 *
 *  Cart window addressing (matches snes/copro.inc):
 *
 *      $0000-$7DFF   per-frame payload area (free-form;
 *                    your DMA descriptors point into here)
 *      $7000-$77FF   joypad mailbox (write-protected here)
 *      $7800         frame-ready byte
 *      $7808-$7847   8 DMA descriptor slots (use mg_stage_dma_slot)
 *      $7F00         status byte
 *      $8000-$FFFF   boot blob + RAM-kernel blob + vectors (don't touch)
 *
 *  The stage_payload helper rejects writes into the side-effect or
 *  vector regions; you only get to write the per-frame payload area.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef GUEST_MG_COPRO_H
#define GUEST_MG_COPRO_H

#include "vm_runtime.h"
#include <stdint.h>
#include <stddef.h>

/* B-bus address bytes the kernel walker recognizes (from copro.inc):
 *   $22 = CGDATA   (CGRAM upload)
 *   $18 = VMDATAL  (VRAM upload; word writes via VMAIN word-step)
 *   $04 = OAMDATA  (OAM upload)
 * A descriptor with bbus = 0 is treated as "empty slot, skip." */
#define MG_BBUS_CGDATA   0x22u
#define MG_BBUS_VMDATAL  0x18u
#define MG_BBUS_OAMDATA  0x04u

/* DMA pattern bytes: see SNES DMAP register documentation.
 *   $00 = 1 byte -> 1 reg, A increments (CGRAM, OAM)
 *   $01 = 2 bytes -> 2 regs (lo, hi), A increments (VRAM word writes)
 */
#define MG_DMAP_BYTE     0x00u
#define MG_DMAP_WORD     0x01u

/* Stage `size` bytes from `data` into the cart window at byte offset
 * `window_off`. Returns 0 on success, negative errno on failure
 * (e.g. -EINVAL if the range would overlap reserved regions). */
static inline int mg_stage_payload(uint32_t window_off, const void *data,
                                   uint32_t size) {
    return (int)_vm_sys3(SYS_COPRO_STAGE_PAYLOAD,
                         window_off, (uint32_t)(uintptr_t)data, size);
}

/* Fill DMA slot `i` (0..7). bbus = one of MG_BBUS_*; dmap one of
 * MG_DMAP_*; `src` is the byte offset inside the cart window where
 * this DMA's source data lives (typically in the payload area, i.e.
 * < $7000). `size` is the byte count; `prep` is the pre-transfer
 * value the SNES kernel writes to the PPU dest register before
 * firing the DMA (CGADD low byte for CGRAM; VMADDL/H word for VRAM;
 * OAMADDR word for OAM). Returns 0 on success. */
static inline int mg_stage_dma_slot(uint32_t slot,
                                    uint8_t bbus, uint8_t dmap,
                                    uint16_t src, uint16_t size,
                                    uint16_t prep) {
    uint32_t packed_lo = (uint32_t)bbus
                       | ((uint32_t)dmap << 8)
                       | ((uint32_t)src  << 16);
    uint32_t packed_hi = (uint32_t)size
                       | ((uint32_t)prep << 16);
    return (int)_vm_sys3(SYS_COPRO_STAGE_DMA_SLOT,
                         slot, packed_lo, packed_hi);
}

/* Commit this frame: set the frame-ready byte at window[$7800].
 * Non-zero = the SNES kernel processes the DMA list this NMI;
 * zero = skip (previous frame stays on screen).  Returns 0. */
static inline int mg_frame_commit(uint8_t frame_ready_byte) {
    return (int)_vm_sys1(SYS_COPRO_FRAME_COMMIT,
                         (uint32_t)frame_ready_byte);
}

/* Read the latest joypad snapshot into `pads[0..3]`. Word format:
 *   bit 15..4 = B Y Select Start Up Down Left Right A X L R
 *   bit  3..0 = controller-type signature (0 = standard pad)
 * Returns 0 on success, -EFAULT if `pads` is unreadable. */
static inline int mg_read_pads(uint16_t pads[4]) {
    return (int)_vm_sys1(SYS_COPRO_READ_PADS, (uint32_t)(uintptr_t)pads);
}

/* Block this VM until the SNES side reports it has consumed the
 * staged frame. Stage 3b: this returns immediately — full
 * scheduler-side blocking on a frame-consumed signal from the bsnes
 * mapper lands in a later stage. Treat it as a hint today: pace your
 * own frame timing with sys_sleep_until or similar. */
static inline int mg_wait_vblank(void) {
    return (int)_vm_sys0(SYS_COPRO_WAIT_VBLANK);
}

/* Monotonically-incrementing counter that bumps each time the SNES
 * is reset. Returns the current value (always >= 0). Compare against
 * a stashed snapshot:
 *
 *     static uint32_t last_seen = 0;
 *     uint32_t now = mg_copro_reset_count();
 *     if (now != last_seen) { reload_state(); last_seen = now; }
 *
 * The kernel reboots regardless of whether the guest reacts, so a
 * guest that ignores the counter just keeps pushing frames into a
 * window the SNES has just rebooted into — same effect as cold boot.
 */
static inline uint32_t mg_copro_reset_count(void) {
    return _vm_sys0(SYS_COPRO_RESET_COUNT);
}

#endif /* GUEST_MG_COPRO_H */
