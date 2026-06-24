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
#include <stdlib.h>
#include <string.h>

/* v2.36 diagnostic toggle: max frames in flight. 2 = commit-ahead
 * pipeline (20fps target); set env MG_NO_COMMIT_AHEAD=1 to force 1 =
 * the old single-frame behaviour, for A/B isolating render glitches.
 * Cached on first read. */
static unsigned pipeline_max_inflight(void) {
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("MG_NO_COMMIT_AHEAD");
        cached = (e && e[0] && e[0] != '0') ? 1 : 2;
    }
    return (unsigned)cached;
}

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
    /* v2.36 commit-ahead: allow up to TWO frames in flight (active +
     * queued) instead of one. Only drop a commit when the queue is
     * genuinely full (in-flight >= 2) — the guest's commit→wait loop
     * blocks at depth 2, so this drop path is defensive. Permitting the
     * second commit is what lets the guest stage frame N+1 while the SNES
     * is still bursting N, so the kernel never idles between frames. */
    if (byte != 0 &&
        (cart_window_frame_staged() - cart_window_frame_consumed())
            >= pipeline_max_inflight()) {
        /* Drop the HDMA-table bump pointer so the next iteration's
         * mg_hdma_upload_table calls start from offset 0 again — the
         * pool would otherwise accumulate this iteration's uploads on
         * top of the previous (un-acked) iteration's, overflowing on
         * iter N+1 even though each iter's tables fit on their own.
         * The kernel reads tables via hdma[].table_off which stays
         * stable across uploads, so overwriting at the same offsets
         * is safe. */
        mg_state_drop_hdma_tables();
        /* v2.17: also drop queued mg_chr_upload_transient bytes.
         * Without this, FMV's iter N's 4 transients (tilemap + 3 CHR
         * chunks = ~27 KB) sit in s_pending until the next successful
         * commit; iter N+1's 4 transients pile on top → ~54 KB attempt
         * → MG_PENDING_TRANSIENT_BUF_BYTES (32 KB) overflows → iter N+1's
         * CHR uploads silently rejected → next-displayed buffer has
         * iter N's CHR paired with iter N+1's tilemap = "every other
         * FMV frame is garbage" symptom. */
        mg_state_drop_pending_transients();
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

/* SYS_MG_READ_MOUSE() → packed: bits 0-7 = buttons (bit0 left, bit1 right),
 * bits 8-15 = dx (int8), bits 16-23 = dy (int8). Drains the port-2 mouse
 * mailbox — the same source the FMV overlay reads (cart_window_consume_mouse),
 * so a guest menu can act on clicks with no PuTTY shell. */
static void h_read_mouse(VmCpu *cpu, void *system) {
    (void)system;
    int dx = 0, dy = 0;
    uint8_t buttons = 0;
    cart_window_consume_mouse(&dx, &dy, &buttons);
    if (dx >  127) dx =  127; else if (dx < -128) dx = -128;
    if (dy >  127) dy =  127; else if (dy < -128) dy = -128;
    cpu->regs[VM_REG_A0] = (uint32_t)buttons
                         | ((uint32_t)(uint8_t)(int8_t)dx <<  8)
                         | ((uint32_t)(uint8_t)(int8_t)dy << 16);
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
 * v2.30.7 Phase 3b: proper implementation. Block guest until the
 * SNES kernel has consumed the last frame the guest committed —
 * specifically, until cart_window's g_frame_consumed counter
 * catches up to g_frame_staged. The cart_window port-7 read
 * callback (cart_window_read for offset CW_OFF_JOY_BASE + 7*256,
 * which the SNES kernel reads once per @loop iteration when a
 * staged frame has been processed) bumps frame_consumed and calls
 * vm_sched_wake_frame_consumed, which wakes any VM whose stored
 * target the consumed counter has reached.
 *
 * Fast path: if no staged commit is pending (g_frame_staged ==
 * g_frame_consumed), return immediately. Otherwise block on
 * BLOCK_FRAME_CONSUMED with block_deadline = g_frame_staged. */
static void h_wait_vblank(VmCpu *cpu, void *system) {
    (void)system;
    uint32_t staged   = cart_window_frame_staged();
    uint32_t consumed = cart_window_frame_consumed();
    /* v2.36 commit-ahead: block only when the 2-deep queue is FULL
     * (in-flight >= 2). With room for another frame the guest returns
     * immediately and goes on to stage N+1 while the SNES bursts N —
     * that overlap is the 20fps pipeline. Wake when consumed catches up
     * to staged-1 (i.e. in-flight drops back to 1). staged>=2 here, so
     * staged-1 never underflows. */
    unsigned maxinflight = pipeline_max_inflight();
    if ((staged - consumed) < maxinflight) {
        /* Room for another commit — return immediately. */
        cpu->regs[VM_REG_A0] = 0;
        return;
    }
    cpu->block_reason   = BLOCK_FRAME_CONSUMED;
    cpu->block_deadline = staged - (maxinflight - 1u);
    cpu->regs[VM_REG_A0] = 0;   /* set on wake, but be defensive */
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
    if (!vm_ecall_register(r, SYS_MG_READ_MOUSE,        h_read_mouse))      return false;
    if (!vm_ecall_register(r, SYS_COPRO_WAIT_VBLANK,    h_wait_vblank))     return false;
    if (!vm_ecall_register(r, SYS_COPRO_RESET_COUNT,    h_reset_count))     return false;
    return true;
}
