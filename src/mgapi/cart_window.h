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

/* INIDISP HDMA table: SNES kernel reserves HDMA channel 7 at boot
 * pointing at this area. Format is mode-0 DIRECT segments — each
 * chunk is [line_count][value_byte_0][value_byte_1]...[value_byte_N-1]
 * with one value per scanline (line_count < 128). Terminator is 0x00.
 *
 * Why direct mode instead of repeat (which would fit comfortably in
 * 16 bytes): bsnes-plus's HDMA emulation always reads a fresh source
 * byte per scanline regardless of the repeat-mode bit, treating the
 * count byte's bit 7 (REPEAT) as having no effect on source-address
 * progression. A real-hardware repeat table renders wrong on bsnes-
 * plus — the channel reads garbage off the END of the table starting
 * at scanline 1 of each chunk. Direct mode encodes one value per
 * scanline explicitly, so the same table works on real hardware and
 * bsnes-plus identically.
 *
 * Size: 256 bytes covers any full 224-scanline configuration. A worst
 * case is 2 chunks of 127 + 97 + per-line data = 226 bytes + terminator
 * + top/bottom letterbox count bytes = ~230 bytes. */
#define CW_OFF_INIDISP_HDMA 0x7868u
#define CW_INIDISP_HDMA_BYTES 256u

/* HDMA channel control table — 7 channels x 8 bytes each. Channel 7
 * is reserved for INIDISP letterbox (see CW_OFF_INIDISP_HDMA); this
 * area covers channels 0..6. Layout per slot:
 *   +0  enabled  (0/1)
 *   +1  bbad     ($21xx destination low byte)
 *   +2  dmap     (SNES DMAP byte: bits 0..2 transfer mode, etc.)
 *   +3  reserved
 *   +4-5 a1t_off (table offset within COPRO_BANK)
 *   +6-7 reserved
 * The runtime fills this area every frame the channel config or
 * enabled state changes; kernel walks it at vblank, sets DMAP/BBAD/
 * A1T/A1B for each enabled channel, and computes HDMAEN. */
#define CW_OFF_HDMA_CONFIG    0x7968u
#define CW_HDMA_CHANNELS       7u
#define CW_HDMA_CFG_BYTES_EACH 8u
#define CW_HDMA_CONFIG_BYTES   (CW_HDMA_CHANNELS * CW_HDMA_CFG_BYTES_EACH)

/* Mode 7 register batch — 16 bytes the kernel writes to the M7*
 * registers each vblank when emit_mode7_batch has staged values
 * here. Layout:
 *   +0    M7SEL              ($211A — wrap/fill mode)
 *   +1    _reserved
 *   +2-3  M7A 16-bit value   ($211B, write-twice byte regs)
 *   +4-5  M7B                ($211C)
 *   +6-7  M7C                ($211D)
 *   +8-9  M7D                ($211E)
 *   +10-11 M7X               ($211F)
 *   +12-13 M7Y               ($2120)
 *   +14-15 _reserved
 *
 * Mode 7 scroll reuses BG1HOFS / BG1VOFS in the PPU batch — no
 * separate fields here. */
#define CW_OFF_MODE7_BATCH    0x79A0u
#define CW_MODE7_BATCH_BYTES  16u

/* HDMA tables area — game stages per-scanline tables for channels
 * 0..6 here via mg_hdma_upload_table. The runtime bump-allocates
 * within this 1280-byte slab each frame the same way it does with
 * payload-area DMA staging. */
#define CW_OFF_HDMA_TABLES    0x7A00u
#define CW_HDMA_TABLES_BYTES  0x500u   /* 1280 bytes — generous */

/* Both moved out of $7E00/$7F00 — those are now INSIDE the HDMA tables
 * pool ($7A00..$7EFF after v1.20's layout shift). Tucked into the gap
 * between Mode 7 batch ($79A0..$79AF) and HDMA tables ($7A00..). A
 * sufficiently large HDMA upload would otherwise have HDMA reads
 * trigger the strobe-boot state machine (or read status bits as table
 * data). Single-byte each so still 14 bytes of gap left for future
 * additions. */
#define CW_OFF_STROBE_BOOT  0x79B0u
#define CW_OFF_STATUS       0x79B1u

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

/* Mode 7 batch struct — see CW_OFF_MODE7_BATCH for layout. */
typedef struct {
    uint8_t  m7sel;          /* $211A: bit 7 horizontal flip, bit 6
                              * vertical flip, bits 1-0 wrap/fill    */
    uint8_t  _pad0;
    int16_t  m7a, m7b, m7c, m7d;
    int16_t  m7x, m7y;
    uint8_t  _pad1[2];
} Mode7Batch;
_Static_assert(sizeof(Mode7Batch) == CW_MODE7_BATCH_BYTES,
               "Mode7Batch must be exactly 16 bytes — keep in sync with copro.inc");

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

/* Frame-flow counters. _staged ticks per set_frame_ready(!=0).
 * _consumed ticks per SNES read of $7700 (joypad mailbox last byte,
 * end of the kernel's per-frame poll). h_frame_commit compares
 * the two to skip rebuilds while the kernel is still walking the
 * previous frame, so a tight guest loop can't overwrite a staged
 * frame before the SNES has finished consuming it. */
uint32_t cart_window_frame_staged(void);
uint32_t cart_window_frame_consumed(void);

/* Stage one DMA list slot. Slot indices 0..7. Writing all zeros
 * marks the slot empty (skipped by the kernel walker).
 */
void cart_window_set_dma_slot(unsigned index, const CartDmaSlot *slot);

/* Stage the PPU register batch the kernel applies at next vblank. */
void cart_window_set_ppu_batch(const PpuBatch *batch);

/* Stage the Mode 7 batch (M7SEL + matrix + center) the kernel
 * applies after the PPU register batch each vblank. */
void cart_window_set_mode7_batch(const Mode7Batch *batch);

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
