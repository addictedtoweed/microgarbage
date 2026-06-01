/* ============================================================
 *  cart_window.h — internal: the 64 KB SNES cart window backing.
 *
 *  Implementation detail of mgapi.dll / libmgapi. NOT exposed in
 *  the public mgapi.h ABI.
 *
 *  The window is the byte array the SNES side sees as cart ROM.
 *  HiROM mirrors the full 64 KB across every bank, so we maintain
 *  a single buffer + a tiny set of side-effect registers (status,
 *  frame-ready, DMA list, joypad mailbox snapshot) and dispatch
 *  reads through a thin decode table.
 *
 *  Producer (other modules) writes via the explicit setters:
 *      cart_window_load_blob(offset, src, len);
 *      cart_window_set_frame_ready(byte);
 *      cart_window_set_dma_slot(i, ...);
 *      cart_window_post_pads(pads);
 *
 *  Consumer (the cart-bus reader, ultimately mgapi_cart_read):
 *      cart_window_read(addr) -> uint8_t.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MGAPI_CART_WINDOW_H
#define MGAPI_CART_WINDOW_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The window the mapper exposes; matches MGAPI_CART_WINDOW_BYTES.
 * (Repeated here so this header has no dependency on mgapi.h, which
 * lets us unit-test the window module in isolation.)
 */
#define CART_WINDOW_BYTES   (64u * 1024u)

/* Address constants mirror snes/copro.inc. The kernel side is the
 * source of truth; if you change one here, change copro.inc too. */
#define CW_OFF_FRAME_READY  0x7800u
#define CW_OFF_DMA_LIST     0x7808u   /* 8 slots * 8 bytes = 64 bytes */
#define CW_OFF_DMA_LIST_END (CW_OFF_DMA_LIST + 8u * 8u)

/* PPU register batch: 32 bytes the SNES kernel writes to PPU regs
 * every vblank BEFORE walking the DMA list. Lets the host control
 * BGMODE / OBSEL / per-BG SC + NBA + scroll / TM / TS / MOSAIC
 * dynamically per frame, replacing the kernel's boot-time hardcoded
 * Mode-1-BG1 init. Layout: see PpuBatch struct below. */
#define CW_OFF_PPU_BATCH    0x7848u
#define CW_PPU_BATCH_BYTES  32u

#define CW_OFF_STROBE_BOOT  0x7E00u
#define CW_OFF_STATUS       0x7F00u

#define CW_OFF_JOY_BASE     0x7000u
#define CW_OFF_JOY_END      0x7800u   /* exclusive; covers P0..P3 LO/HI */
#define CW_JOY_PAGE_SHIFT   8         /* each port = 256-byte page     */
#define CW_JOY_PORT_COUNT   8         /* P0 LO,HI ... P3 LO,HI         */

/* Status bits (mirrors copro.inc ST_KERNEL_RDY). */
#define CW_STATUS_KERNEL_RDY  0x80u

/* DMA descriptor — 8 bytes per slot, kernel-visible layout. */
typedef struct {
    uint8_t  bbus;        /* $21xx low byte (0 = empty slot)        */
    uint8_t  dmap;        /* SNES DMAP byte                          */
    uint16_t src;         /* offset into COPRO_BANK (this window)    */
    uint16_t size;        /* byte count for the DMA                  */
    uint16_t prep;        /* dest-register prep word                 */
} CartDmaSlot;

/* PPU register batch — 32 bytes at $7848 the kernel walks at vblank
 * before the DMA dispatch. Single-byte registers in bytes 0..15;
 * write-twice 16-bit scroll registers in bytes 16..31. */
typedef struct {
    uint8_t  bgmode;      /* $2105 BGMODE                            */
    uint8_t  obsel;       /* $2101 OBSEL                              */
    uint8_t  bg1sc;       /* $2107 BG1SC                              */
    uint8_t  bg2sc;       /* $2108 BG2SC                              */
    uint8_t  bg3sc;       /* $2109 BG3SC                              */
    uint8_t  bg4sc;       /* $210A BG4SC                              */
    uint8_t  bg12nba;     /* $210B BG12NBA                            */
    uint8_t  bg34nba;     /* $210C BG34NBA                            */
    uint8_t  tm;          /* $212C main-screen designation            */
    uint8_t  ts;          /* $212D sub-screen designation             */
    uint8_t  mosaic;      /* $2106 MOSAIC                             */
    uint8_t  _reserved[5];
    /* Scrolls: each is 16-bit value the kernel writes low then high
     * (the write-twice PPU registers). H first, then V. */
    uint16_t bg1hofs, bg1vofs;
    uint16_t bg2hofs, bg2vofs;
    uint16_t bg3hofs, bg3vofs;
    uint16_t bg4hofs, bg4vofs;
} PpuBatch;
_Static_assert(sizeof(PpuBatch) == CW_PPU_BATCH_BYTES,
               "PpuBatch must be exactly 32 bytes — keep in sync with copro.inc");

/* ----------------------------------------------------------------
 *  Lifecycle
 * ---------------------------------------------------------------- */

/* Zero-initialize the window. Status starts with KERNEL_RDY set so
 * boot.s's wait-for-kernel loop falls through immediately on the
 * non-SMOKE_TEST build path; the smoke build doesn't touch status.
 */
void cart_window_init(void);

/* Wipe + de-init. (No allocs to free; here for symmetry.) */
void cart_window_shutdown(void);

/* ----------------------------------------------------------------
 *  Producers (called by mgapi_init, ecall handlers, etc.)
 * ---------------------------------------------------------------- */

/* Copy `len` bytes from `src` into window[offset..offset+len-1].
 * Out-of-range writes are silently clipped; the caller's intent is
 * always "stage this into the window" — clipping matches the real
 * cart bus's "high address bits are ignored" behavior.
 */
void cart_window_load_blob(uint32_t offset, const void *src, uint32_t len);

/* Set the frame-ready byte the SNES kernel polls in NMI. 0 = skip
 * this frame; non-zero = process the DMA list. The producer
 * (SYS_COPRO_FRAME_COMMIT handler) toggles this once per frame.
 */
void cart_window_set_frame_ready(uint8_t byte);
uint8_t cart_window_get_frame_ready(void);

/* Stage one DMA list slot. Slot indices 0..7. Writing all zeros
 * marks the slot empty (skipped by the kernel walker).
 */
void cart_window_set_dma_slot(unsigned index, const CartDmaSlot *slot);

/* Stage the PPU register batch the kernel applies at next vblank. */
void cart_window_set_ppu_batch(const PpuBatch *batch);

/* Latest joypad snapshot. Word format = SNES auto-joypad
 * ($4218/$4219). The cart-bus side serves these as side-effect
 * mailbox reads in $7000-$77FF.
 */
void cart_window_post_pads(const uint16_t pads[4]);

/* Read one pad word back. i = 0..3 (P0..P3); out-of-range returns 0.
 * Used by the SYS_COPRO_READ_PADS ecall handler. */
uint16_t cart_window_get_pads(unsigned i);

/* ----------------------------------------------------------------
 *  Consumer (the cart-bus read).
 * ---------------------------------------------------------------- */

/* Serve one cart-bus read. Address is the full SNES 24-bit address;
 * only the low 16 bits select (HiROM mirror).
 *
 * Side effects:
 *   - $7E00 read: clears CW_STATUS_KERNEL_RDY one-shot.
 *   - $7000-$77FF read: latches the high-address-bit-decoded
 *     pad-index + lo/hi as the "last polled" mailbox slot. Reads
 *     in this range return zero (don't-care, matches real bus).
 *
 * Returns the byte the cart bus should yield.
 */
uint8_t cart_window_read(uint32_t snes_addr_24);

/* For tests / diagnostics: peek at internal state without
 * triggering side effects. */
uint8_t cart_window_peek_status(void);
unsigned cart_window_last_pad_port_polled(void);  /* 0..7, or -1u */

/* ----------------------------------------------------------------
 *  Reset
 * ----------------------------------------------------------------
 *
 * The embedder (bsnes mapper) and the cold-boot path both fan in
 * here. begin: re-stage the whole 64 KB window from the given ROM,
 * clear all side-effect state, bump the reset counter the copro
 * guest reads via SYS_COPRO_RESET_COUNT.
 */
void     cart_window_reset_begin(const void *rom_bytes, uint32_t rom_size);
uint32_t cart_window_reset_count(void);

#ifdef __cplusplus
}
#endif

#endif /* MGAPI_CART_WINDOW_H */
