/* ============================================================
 *  copro_ecalls.c — handlers for SYS_COPRO_STAGE_PAYLOAD,
 *  SYS_COPRO_STAGE_DMA_SLOT, SYS_COPRO_FRAME_COMMIT,
 *  SYS_COPRO_READ_PADS, SYS_COPRO_WAIT_VBLANK.
 *
 *  Each handler is a thin wrapper that translates guest pointers
 *  via vm_translate_read/write, then forwards to cart_window state.
 *  The vblank-wait is a no-op for stage 3b — full block-on-frame-
 *  consumed scheduling lands when the bsnes mapper is providing real
 *  consumption signals (stage 5+).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "copro_ecalls.h"

#include "cart_window.h"
#include "copro_mg_state.h"
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"
#include "vm/vm_system.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

/* SYS_COPRO_STAGE_PAYLOAD(window_off, guest_buf, size) → 0/-errno */
static void h_stage_payload(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t off       = cpu->regs[VM_REG_A0];
    uint32_t guest_buf = cpu->regs[VM_REG_A1];
    uint32_t size      = cpu->regs[VM_REG_A2];

    /* Refuse writes that would touch the static top page (vectors)
     * or any of the side-effect registers ($7000-$7FFF). The kernel
     * NEVER expects the copro to clobber these regions — they're
     * either signal-by-read mailboxes or read-only state. */
    if (size == 0 || off >= CART_WINDOW_BYTES ||
        off + size > CART_WINDOW_BYTES) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EINVAL;
        return;
    }
    if (off < 0x8000u && off + size > 0x7000u) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EINVAL;
        return;
    }

    const void *src = vm_translate_read(cpu, guest_buf, size);
    if (!src) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EFAULT;
        return;
    }
    cart_window_load_blob(off, src, size);
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_COPRO_STAGE_DMA_SLOT(slot, packed_lo, packed_hi) → 0/-errno
 *
 * Packed argument layout (two 32-bit words to avoid running out of
 * argument registers):
 *   packed_lo  : [bbus:8][dmap:8][src:16]
 *   packed_hi  : [size:16][prep:16]
 *
 * Matches the on-window descriptor format from snes/copro.inc. The
 * kernel walker pulls these bytes through the cart bus in the same
 * order the SNES DMA registers expect them. */
static void h_stage_dma_slot(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t slot      = cpu->regs[VM_REG_A0];
    uint32_t packed_lo = cpu->regs[VM_REG_A1];
    uint32_t packed_hi = cpu->regs[VM_REG_A2];

    if (slot >= 8) { cpu->regs[VM_REG_A0] = (uint32_t)-EINVAL; return; }

    CartDmaSlot s = {
        .bbus = (uint8_t) (packed_lo & 0xFFu),
        .dmap = (uint8_t)((packed_lo >> 8)  & 0xFFu),
        .src  = (uint16_t)((packed_lo >> 16) & 0xFFFFu),
        .size = (uint16_t)(packed_hi & 0xFFFFu),
        .prep = (uint16_t)((packed_hi >> 16) & 0xFFFFu),
    };
    cart_window_set_dma_slot(slot, &s);
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_COPRO_FRAME_COMMIT(byte) → 0
 *
 * Sets the COPRO_FRAME_RDY byte at window[$7800]. The SNES kernel's
 * NMI handler reads it: non-zero = walk the DMA list, zero = skip
 * this frame (the previous frame's image stays on screen). Sending
 * 0 explicitly is how a guest signals "this frame is cancelled." */
static void h_frame_commit(VmCpu *cpu, void *system) {
    (void)system;
    uint8_t byte = (uint8_t)(cpu->regs[VM_REG_A0] & 0xFFu);

    /* If the SNES kernel hasn't yet ack'd the previous staged frame
     * (joypad mailbox port 7 read), don't touch anything -- the
     * previous frame's DMA list is still in flight in cart_window
     * and a rebuild would clear dirty marks + reset payload bytes
     * the kernel is mid-walking. Tight guest loops (mg_frame_commit
     * + mg_wait_frame, which returns immediately today) call this
     * thousands of times per real SNES frame; we want at most ONE
     * effective commit per kernel @loop iteration so each staged
     * frame survives long enough to reach the PPU.
     *
     * "byte=0" is the explicit cancel path; still honor it so a
     * guest can pull a frame back if it decides to. */
    if (byte != 0 &&
        cart_window_frame_staged() > cart_window_frame_consumed()) {
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    /* Walk the mg_* shadow state, stage dirty regions into the cart
     * window's payload area, queue DMA slots — this is where SYS_MG_*
     * accumulated state gets turned into the per-frame DMA descriptor
     * list the SNES kernel consumes. Runs before the frame-ready byte
     * goes high so the kernel sees a consistent snapshot. */
    mg_state_build_frame();

    cart_window_set_frame_ready(byte);
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_COPRO_READ_PADS(out_buf_4xu16) → 0/-errno
 *
 * Copy the latest joypad snapshot into the guest's buffer. Word
 * format matches the SNES auto-joypad layout ($4218/$4219):
 *     bit 15..4 = B Y Select Start Up Down Left Right A X L R
 *     bit  3..0 = controller signature (0 for std pad). */
static void h_read_pads(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t guest_buf = cpu->regs[VM_REG_A0];

    void *dst = vm_translate_write(cpu, guest_buf, sizeof(uint16_t) * 4);
    if (!dst) {
        cpu->regs[VM_REG_A0] = (uint32_t)-EFAULT;
        return;
    }

    /* cart_window doesn't yet expose a public "read latest pads"
     * helper — the SNES side gets them via the page-aligned mailbox
     * reads, not by snooping the latched value. For the guest's
     * convenience we mirror the state directly. */
    extern uint16_t cart_window_get_pads(unsigned i);
    uint16_t pads[4];
    for (int i = 0; i < 4; i++) pads[i] = cart_window_get_pads((unsigned)i);
    memcpy(dst, pads, sizeof pads);
    cpu->regs[VM_REG_A0] = 0;
}

/* SYS_COPRO_RESET_COUNT() → uint32 reset counter
 *
 * Monotonically incrementing counter that bumps on every
 * mgapi_cart_reset_begin. Guests poll it to detect "the SNES was
 * reset; rebuild your state." Returns 0 forever if no reset has
 * happened. The counter is always in the non-negative half of int32
 * so signed-error semantics don't get in the way. */
static void h_reset_count(VmCpu *cpu, void *system) {
    (void)system;
    cpu->regs[VM_REG_A0] = cart_window_reset_count() & 0x7FFFFFFFu;
}

/* SYS_COPRO_WAIT_VBLANK() → 0
 *
 * Stage 3b: returns immediately. The full design has the scheduler
 * block this VM on BLOCK_FRAME_CONSUMED until the bsnes mapper reports
 * that the SNES NMI processed the staged frame. That requires both a
 * scheduler-side block reason and a real frame-consumed signal from
 * the mapper, which lands when stage 5's bsnes glue is in place.
 *
 * Returning immediately is safe: guests that rely on this for pacing
 * just spin-loop today, and the runtime falls back to whatever the
 * VM's existing sleep_until / yield mechanism provides. */
static void h_wait_vblank(VmCpu *cpu, void *system) {
    (void)system;
    cpu->regs[VM_REG_A0] = 0;
}

/* ----------------------------------------------------------------
 *  Installation
 * ---------------------------------------------------------------- */

bool mgapi_install_copro_ecalls(VmSystem *sys) {
    if (!sys || !sys->ecall_router) return false;
    VmEcallRouter *r = sys->ecall_router;

    if (!vm_ecall_register(r, SYS_COPRO_STAGE_PAYLOAD,  h_stage_payload))   return false;
    if (!vm_ecall_register(r, SYS_COPRO_STAGE_DMA_SLOT, h_stage_dma_slot))  return false;
    if (!vm_ecall_register(r, SYS_COPRO_FRAME_COMMIT,   h_frame_commit))    return false;
    if (!vm_ecall_register(r, SYS_COPRO_READ_PADS,      h_read_pads))       return false;
    if (!vm_ecall_register(r, SYS_COPRO_WAIT_VBLANK,    h_wait_vblank))     return false;
    if (!vm_ecall_register(r, SYS_COPRO_RESET_COUNT,    h_reset_count))     return false;
    return true;
}
