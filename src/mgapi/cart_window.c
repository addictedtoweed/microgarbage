/* ============================================================
 *  cart_window.c — the 64 KB SNES cart window, with side effects.
 *
 *  The byte array. The status / frame-ready / DMA-list / mailbox
 *  state. The read-decode dispatcher. Nothing else.
 *
 *  See cart_window.h for the contract; snes/copro.inc for the
 *  address constants this mirrors.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "cart_window.h"
#include "copro_mg_state.h"   /* mg_state_advance_subframe (v2.05) */

#include <stdio.h>
#include <string.h>

/* ----------------------------------------------------------------
 *  State (file-static; one window per process).
 * ----------------------------------------------------------------
 *  This is a singleton because the cart bus is a singleton — there
 *  is exactly one SNES, exactly one mapper, exactly one window.
 *  The mgapi.dll instance owns it.
 */

static uint8_t  g_window[CART_WINDOW_BYTES];
static uint8_t  g_status;            /* served at $7F00      */
static uint8_t  g_frame_ready;       /* served at $7800      */
/* v2.05: count $7800 reads ever. Each read corresponds to one NMI
 * starting (kernel reads it at the top of every NMI handler). Used by
 * sub-frame chaining to figure out how many NMIs have already fired
 * since the most recent commit, so the port-7 advance trigger doesn't
 * race ahead of the actual slot-walk completions. */
static uint32_t g_frame_ready_reads;
static uint32_t g_subframe_init_reads;   /* read count at commit time */

/* Frame-flow counters. g_frame_staged ticks every time we set
 * g_frame_ready to non-zero (= the guest committed a frame).
 * g_frame_consumed ticks every time the SNES reads from the
 * last joypad mailbox port ($7700) -- that's the kernel's
 * end-of-frame ack in its @loop sequence. A guest in
 * h_frame_commit can compare the two to see whether the prior
 * frame has been picked up yet, and skip the rebuild if the
 * kernel is still walking the previous one. */
static uint32_t g_frame_staged;
static uint32_t g_frame_consumed;
static uint16_t g_pads[4];           /* served via mailbox   */
static unsigned g_last_pad_port;     /* last polled, 0..7    */
static uint32_t g_reset_count;       /* bumped on reset_begin */

/* v2.30.7 Phase 3b: optional hook for frame_consumed bumps —
 * lets the VM scheduler wake BLOCK_FRAME_CONSUMED waiters. */
static CartWindowFrameConsumedHook g_frame_consumed_hook;
static void                       *g_frame_consumed_hook_userdata;

void cart_window_set_frame_consumed_hook(CartWindowFrameConsumedHook hook,
                                         void *userdata) {
    g_frame_consumed_hook         = hook;
    g_frame_consumed_hook_userdata = userdata;
}

/* DMA list lives at $7808-$7847 directly inside g_window so reads
 * in that range can just hit the array. Producers go through
 * cart_window_set_dma_slot which writes into the same memory. */

static inline void put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)(v >> 8);
}

/* ----------------------------------------------------------------
 *  Lifecycle
 * ---------------------------------------------------------------- */

void cart_window_init(void) {
    memset(g_window, 0, sizeof g_window);
    g_status        = CW_STATUS_KERNEL_RDY;
    g_frame_ready   = 0;
    g_pads[0] = g_pads[1] = g_pads[2] = g_pads[3] = 0;
    g_last_pad_port = (unsigned)-1;
    g_reset_count   = 0;
}

void cart_window_shutdown(void) {
    /* Nothing to free; cart_window_init resets everything. */
    g_status        = 0;
    g_frame_ready   = 0;
    g_last_pad_port = (unsigned)-1;
}

/* ----------------------------------------------------------------
 *  Producers
 * ---------------------------------------------------------------- */

void cart_window_store_u16_le(uint32_t offset, uint16_t value) {
    if (offset + 2u > CART_WINDOW_BYTES) return;
    /* Cast through uint16_t* so the compiler emits a single 16-bit
     * store. Unaligned 16-bit stores on x86/x64 are single uops and
     * are atomic at the byte level — meaning the SNES side reading
     * via `rep #$20 / lda f:abs` (a 16-bit read) sees either the
     * full old value or the full new value, never a mix. */
    uint8_t *p = g_window + offset;
    *(uint16_t *)p = value;
}

void cart_window_load_blob(uint32_t offset, const void *src, uint32_t len) {
    if (offset >= CART_WINDOW_BYTES) return;
    uint32_t avail = CART_WINDOW_BYTES - offset;
    if (len > avail) len = avail;
    memcpy(g_window + offset, src, len);
}

void cart_window_set_frame_ready(uint8_t byte) {
    g_frame_ready = byte;
    /* v2.05: latch the current NMI-tick counter so port-7's advance
     * trigger can tell how many NMIs have fired since this commit. */
    if (byte != 0) g_subframe_init_reads = g_frame_ready_reads;
    if (byte != 0) g_frame_staged++;
}
uint8_t cart_window_get_frame_ready(void)      { return g_frame_ready; }

uint32_t cart_window_frame_staged(void)  { return g_frame_staged; }
uint32_t cart_window_frame_consumed(void){ return g_frame_consumed; }

void cart_window_bump_nmi_version(void) {
    /* Single byte. The kernel compares for inequality against a WRAM
     * cache. Skip 0 on wrap so we never falsely re-enter the
     * "uninstall" sentinel after a real install. */
    uint8_t v = (uint8_t)(g_window[CW_OFF_NMI_VERSION] + 1u);
    if (v == 0) v = 1;
    g_window[CW_OFF_NMI_VERSION] = v;
}

void cart_window_clear_nmi_version(void) {
    /* v2.21: signal "uninstall" to the kernel. Setting the byte to 0
     * causes the @loop's next version-poll to see a mismatch (cache
     * holds the previous install's value), and the "new value is 0"
     * branch restores RAMVEC_NMI to the default kernel proc.
     *
     * We DON'T zero the CW_OFF_NMI_CODE region — the kernel never
     * reads from there during the uninstall branch (it just restores
     * RAMVEC_NMI from K_NMI_DEFAULT). A subsequent install will
     * overwrite the region. */
    g_window[CW_OFF_NMI_VERSION] = 0;
}

/* v2.26: HIRQ version helpers — same shape as the NMI pair. */
void cart_window_bump_hirq_version(void) {
    uint8_t v = (uint8_t)(g_window[CW_OFF_HIRQ_VERSION] + 1u);
    if (v == 0) v = 1;
    g_window[CW_OFF_HIRQ_VERSION] = v;
}

void cart_window_clear_hirq_version(void) {
    g_window[CW_OFF_HIRQ_VERSION] = 0;
}

void cart_window_set_dma_slot(unsigned index, const CartDmaSlot *slot) {
    if (index >= 8 || !slot) return;
    uint8_t *p = g_window + CW_OFF_DMA_LIST + index * 8u;
    p[0] = slot->bbus;
    p[1] = slot->dmap;
    put_le16(p + 2, slot->src);
    put_le16(p + 4, slot->size);
    put_le16(p + 6, slot->prep);
}

void cart_window_set_ppu_batch(const PpuBatch *batch) {
    if (!batch) return;
    /* The struct layout matches the on-window bytes exactly (single-
     * byte regs first, then 16-bit scrolls little-endian) so memcpy
     * is faithful. PpuBatch is _Static_asserted to 32 bytes. */
    memcpy(g_window + CW_OFF_PPU_BATCH, batch, CW_PPU_BATCH_BYTES);
}

void cart_window_set_mode7_batch(const Mode7Batch *batch) {
    if (!batch) return;
    memcpy(g_window + CW_OFF_MODE7_BATCH, batch, CW_MODE7_BATCH_BYTES);
}

void cart_window_post_pads(const uint16_t pads[4]) {
    if (!pads) return;
    g_pads[0] = pads[0];
    g_pads[1] = pads[1];
    g_pads[2] = pads[2];
    g_pads[3] = pads[3];
}

uint16_t cart_window_get_pads(unsigned i) {
    return (i < 4) ? g_pads[i] : 0;
}

/* ----------------------------------------------------------------
 *  Read dispatcher
 * ---------------------------------------------------------------- */

uint8_t cart_window_read(uint32_t snes_addr_24) {
    /* HiROM, full-window mirror across every bank: the low 16 bits
     * select the byte. The kernel and boot stub reach the window
     * through both $00:8000-FFFF (boot/vectors) and $C0:0000-FFFF
     * (runtime) — same bytes, just different address forms. */
    uint16_t off = (uint16_t)(snes_addr_24 & 0xFFFFu);

    /* Diagnostic counters: surface what the SNES is actually doing
     * on the cart bus so we can tell "kernel not running" apart from
     * "kernel running but data wrong." Stderr-printed once per
     * transition by mgapi_diag_periodic() (called from mgapi_step). */
    extern void mgapi_diag_note_cart_read(uint16_t off, uint32_t full);
    mgapi_diag_note_cart_read(off, snes_addr_24);

    /* Status byte. */
    if (off == CW_OFF_STATUS) return g_status;

    /* Frame-ready byte. The kernel reads this at the top of every NMI
     * handler; counting reads gives us a reliable per-NMI tick the
     * sub-frame chaining can use to gate advance() against. */
    if (off == CW_OFF_FRAME_READY) {
        g_frame_ready_reads++;
        return g_frame_ready;
    }

    /* Boot strobe — one-shot side effect: clear kernel-ready bit so
     * the copro (us) knows the SNES is now running from RAM and we
     * can switch to runtime serving. Returned byte is don't-care. */
    if (off == CW_OFF_STROBE_BOOT) {
        g_status &= (uint8_t)~CW_STATUS_KERNEL_RDY;
        return 0;
    }

    /* Joypad mailbox: 8 page-aligned 256-byte ports at
     * $7000, $7100, ..., $7700. Each read latches "this port was
     * polled" (port = pad index + lo/hi byte derived from the high
     * byte of the address). The returned data byte is don't-care.
     *
     * Pad DATA comes through the mgapi_post_joypads host hook — for
     * bsnes-plus the cart-class Mgapi adapter polls bsnes-plus's
     * input.poll + system.interface->input_poll each frame and posts
     * the packed pad word; for real-hardware hosts the kernel's
     * bit-banged $4016/$4017 reads would carry pad bytes via the
     * address low byte, but bsnes-plus's $4016 handler doesn't
     * deliver mapped-keyboard state through our cart class (it
     * returns open-bus garbage that converges to $FFFF after a few
     * frames), so we don't try to derive pad data from the mailbox
     * address — we just record the port so port 7 can ack the frame. */
    if (off >= CW_OFF_JOY_BASE && off < CW_OFF_JOY_END) {
        unsigned port = (unsigned)(off - CW_OFF_JOY_BASE) >> CW_JOY_PAGE_SHIFT;
        g_last_pad_port = port;
        /* Port 7 = the LAST joypad mailbox read in the kernel's
         * @loop sequence (pad 3 high byte). The kernel reads it on
         * EVERY main-loop iteration regardless of whether NMI just
         * processed a staged frame — so blindly bumping consumed
         * every time would let it race far ahead of staged during
         * the shell's idle period (no demo running). When the first
         * demo finally commits, `staged > consumed` is already
         * FALSE (consumed >> 1), so the very next iter's commit
         * wouldn't early-return and would clear the just-staged
         * slot BEFORE NMI 1 dispatched it. Result: frame 1's CGRAM
         * DMA never lands in PPU; the screen is blank for demos
         * that only re-dirty CGRAM once at setup (the visible
         * symptom in mode7.elf).
         *
         * Fix: only bump consumed when frame_ready was 1 at the
         * time of this port-7 read — i.e., NMI just walked a real
         * staged frame. Also clear frame_ready to 0 so subsequent
         * port-7 reads (from idle NMI loop iterations after demo
         * exit, or before next commit) don't keep bumping. */
        if (port == 7 && g_frame_ready != 0) {
            /* v2.05: only advance once a real NMI has fired since the
             * commit that loaded the current sub-frame. The kernel's
             * @loop reads port 7 BEFORE its wai-for-NMI, so a naive
             * advance on every read would skip sub-frame 0 (the read
             * happens after commit but before NMI 1 processes it).
             * `nmis_seen` counts $7800 reads since commit; the first
             * port-7 read while nmis_seen == 0 must wait for NMI 1
             * to actually fire before advancing. */
            uint32_t nmis_seen = g_frame_ready_reads - g_subframe_init_reads;
            if (nmis_seen > 0) {
                /* NMI has fired (and processed the slots that were
                 * loaded). Either load the next sub-frame, or close
                 * out the logical frame if the queue is empty. */
                if (mg_state_advance_subframe()) {
                    /* keep g_frame_ready set */
                    /* Re-latch baseline so subsequent reads gate on
                     * the *next* NMI rather than the one that already
                     * fired. */
                    g_subframe_init_reads = g_frame_ready_reads;
                } else {
                    g_frame_consumed++;
                    g_frame_ready = 0;
                    if (g_frame_consumed_hook) {
                        g_frame_consumed_hook(g_frame_consumed,
                                              g_frame_consumed_hook_userdata);
                    }
                }
            }
            /* else: no NMI yet — leave the current sub-frame's slots
             * alone so NMI 1 still processes them. */
        }
        return 0;
    }

    /* DMA list (8 slots * 8 bytes). Producer writes into g_window
     * directly via cart_window_set_dma_slot, so a plain window
     * read covers this; we list the range for documentation. */
    /* fallthrough */

    /* Everything else: serve the window byte. */
    return g_window[off];
}

uint8_t  cart_window_peek_status(void)         { return g_status; }
unsigned cart_window_last_pad_port_polled(void) { return g_last_pad_port; }

void cart_window_reset_begin(const void *rom_bytes, uint32_t rom_size) {
    /* Restage the whole 64 KB from the ROM. We clear unconditionally
     * first so a short ROM (smaller than the window) leaves zeros in
     * the tail rather than stale bytes from the previous run. */
    memset(g_window, 0, sizeof g_window);
    if (rom_bytes && rom_size > 0) {
        uint32_t n = rom_size > CART_WINDOW_BYTES ? CART_WINDOW_BYTES
                                                  : rom_size;
        memcpy(g_window, rom_bytes, n);
    }

    /* Restore side-effect registers to their cold-boot values. */
    g_status        = CW_STATUS_KERNEL_RDY;
    g_frame_ready   = 0;
    g_pads[0] = g_pads[1] = g_pads[2] = g_pads[3] = 0;
    g_last_pad_port = (unsigned)-1;
    g_reset_count++;
}

uint32_t cart_window_reset_count(void) { return g_reset_count; }
