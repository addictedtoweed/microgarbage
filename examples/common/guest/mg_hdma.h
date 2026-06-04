/* ============================================================
 *  mg_hdma.h — per-scanline tables for cart-side games.
 *
 *  Tables are staged into a reserved WRAM region during vblank (one
 *  DMA slot + table_len bytes per upload). The PPU consumes the
 *  table scanline-by-scanline during active display without further
 *  coprocessor involvement, freeing the cart side to run game logic.
 *
 *  Common uses:
 *    - parallax scroll (HOFS per scanline)
 *    - sky gradient (FIXED_COLOR per scanline)
 *    - Mode 7 perspective (M7A/M7B/M7C/M7D per scanline — F-Zero
 *      style canyon)
 *
 *  A future rearmed scanline-timer IRQ for mid-line register tweaks
 *  beyond what HDMA tables can express is deferred to mg_hdma_irq.h.
 *
 *  See docs/game-api.md for design rationale.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_HDMA_H
#define MG_HDMA_H

#include <stdint.h>
#include <stdbool.h>

#include "mg_panic.h"   /* MgResult */

#ifdef __cplusplus
extern "C" {
#endif

/* PPU register targets HDMA can safely write per scanline. The enum
 * values are the SNES PPU register low bytes (the high byte is $21
 * for all). Only the writable mid-frame ones are exposed here;
 * window masks etc. land if a customer asks. */
typedef enum {
    MG_HDMA_DEST_BG1_HOFS    = 0x0D,
    MG_HDMA_DEST_BG1_VOFS    = 0x0E,
    MG_HDMA_DEST_BG2_HOFS    = 0x0F,
    MG_HDMA_DEST_BG2_VOFS    = 0x10,
    MG_HDMA_DEST_BG3_HOFS    = 0x11,
    MG_HDMA_DEST_BG3_VOFS    = 0x12,
    MG_HDMA_DEST_BG4_HOFS    = 0x13,
    MG_HDMA_DEST_BG4_VOFS    = 0x14,
    MG_HDMA_DEST_FIXED_COLOR = 0x32,   /* sky-gradient classic       */
    MG_HDMA_DEST_M7A         = 0x1B,
    MG_HDMA_DEST_M7B         = 0x1C,
    MG_HDMA_DEST_M7C         = 0x1D,
    MG_HDMA_DEST_M7D         = 0x1E,
    MG_HDMA_DEST_M7X         = 0x1F,
    MG_HDMA_DEST_M7Y         = 0x20,
} MgHdmaDest;

/* Transfer mode determines bytes-per-scanline written to the
 * destination register(s) and the address increment. SNES has 8
 * modes total; the four below cover the common cases. */
typedef enum {
    MG_HDMA_XFER_1B_1R = 0,   /* 1 byte to 1 register                   */
    MG_HDMA_XFER_2B_1R = 1,   /* 2 bytes to same register (writes twice) */
    MG_HDMA_XFER_2B_2R = 2,   /* 2 bytes to two consecutive registers   */
    MG_HDMA_XFER_4B_2R = 3,   /* 4 bytes to two regs x2                 */
} MgHdmaXfer;

/* IMPORTANT: keep this struct exactly 4 packed bytes — the host
 * h_hdma_setup does a fixed sizeof(MgHdmaCfgHost)=4 read into a
 * uint8_t-fielded mirror struct. RISC-V GCC sizes enums as int (4
 * bytes), so if dest/xfer were typed as MgHdmaDest/MgHdmaXfer the
 * guest struct would grow to 16 bytes with 3 padding bytes after
 * channel — the host would then read {channel, 0, 0, 0} and set
 * BBAD=$00 (= INIDISP) and DMAP=$00 (mode 0) instead of the M7
 * destination + mode-2 the demo asked for. Visible symptom was
 * mode7_3d.elf rendering solid black because channel 1 HDMA was
 * silently writing INIDISP=$00 (brightness 0) every scanline. */
typedef struct {
    uint8_t  channel;         /* 0..7 — shares bank with general DMA   */
    uint8_t  dest;            /* MgHdmaDest value, narrowed to byte    */
    uint8_t  xfer;            /* MgHdmaXfer value, narrowed to byte    */
    uint8_t  indirect;        /* nonzero = table holds pointers (rare) */
} MgHdmaCfg;

/* Configure a channel. Idempotent across frames until reconfigured
 * or disabled. PPU register writes only, no DMA cost. */
void     mg_hdma_setup       (const MgHdmaCfg *cfg);

/* Stage the channel's per-scanline table into WRAM. The format is
 * SNES native:
 *
 *   for each segment:
 *     count_byte           lines this segment covers (1..127, MSB=repeat)
 *     payload[xfer_size]...
 *   terminator: 0x00
 *
 * Costs: 1 DMA slot + `len` bytes against the byte budget. */
MgResult mg_hdma_upload_table(uint8_t channel, const void *table, uint16_t len);

/* Enable / disable a channel. PPU register write at vblank, no DMA
 * cost. */
void     mg_hdma_enable      (uint8_t channel, bool on);

#ifdef __cplusplus
}
#endif

#endif /* MG_HDMA_H */
