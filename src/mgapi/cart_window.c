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

void cart_window_load_blob(uint32_t offset, const void *src, uint32_t len) {
    if (offset >= CART_WINDOW_BYTES) return;
    uint32_t avail = CART_WINDOW_BYTES - offset;
    if (len > avail) len = avail;
    memcpy(g_window + offset, src, len);
}

void cart_window_set_frame_ready(uint8_t byte) {
    g_frame_ready = byte;
    if (byte != 0) g_frame_staged++;
}
uint8_t cart_window_get_frame_ready(void)      { return g_frame_ready; }

uint32_t cart_window_frame_staged(void)  { return g_frame_staged; }
uint32_t cart_window_frame_consumed(void){ return g_frame_consumed; }

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

    /* Frame-ready byte. */
    if (off == CW_OFF_FRAME_READY) return g_frame_ready;

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
            g_frame_consumed++;
            g_frame_ready = 0;
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
