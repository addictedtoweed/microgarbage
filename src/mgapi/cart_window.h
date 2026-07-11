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
 * within this 2048-byte slab each frame the same way it does with
 * payload-area DMA staging.
 *
 * v2.04: moved from $6000 to $7000, shrunk from 4KB to 2KB so the
 * payload area can grow back to 28KB. demo_fmv's per-frame CHR
 * upload (24,960 B) + tilemap (2,048 B) + CGRAM (256 B) = ~27 KB
 * needs the bigger payload; 2KB is still enough for mode7_3d's
 * ~1800-byte M7A-D table set (the largest current HDMA user). The
 * old 4KB sizing had room for "more channels or smaller granularities"
 * but no demo actually used that headroom. */
#define CW_OFF_HDMA_TABLES    0x7000u
#define CW_HDMA_TABLES_BYTES  0x0800u  /* 2048 bytes — fits mode7_3d ~1800 */

/* v2.18: per-app custom NMI handler region. Guest stages 65816 bytes
 * here via SYS_MG_NMI_INSTALL; the kernel @loop polls
 * CW_OFF_NMI_VERSION, copies the region into WRAM at $0E00 when the
 * version changes, and updates RAMVEC_NMI to point at WRAM. Allows
 * each guest to install a minimal NMI tailored to its frame shape
 * (FMV: ~250-cycle handler with no Mode 7 / HDMA setup vs the
 * generic kernel's ~1100-cycle preamble).
 *
 * Carved from the high end of the payload area: payload was 28 KB,
 * now 27 KB. FMV's per-commit staged data is ~26.6 KB — 400 B
 * margin. The future 1/3-buffer streaming design will reclaim
 * substantial payload space (per-commit drops from ~27 KB to ~9 KB),
 * at which point this region can grow if needed. */
#define CW_OFF_NMI_CODE       0x6C00u
#define CW_NMI_CODE_BYTES     0x0400u  /* 1 KB */

/* v2.26 Phase 2.5: per-app HIRQ (HBLANK-triggered IRQ) handler region.
 * Same shape as the NMI region — guest stages 65816 bytes via
 * SYS_MG_HIRQ_INSTALL, kernel polls CW_OFF_HIRQ_VERSION, byte-copies
 * to WRAM at $1200, and rewrites the WRAM IRQ vector at $0202. The
 * HIRQ ISR fires per-scanline during the active-display siphon window
 * (start_line/end_line/bytes-per-line configured via SYS_MG_HIRQ_CONFIG
 * — Phase 2.5b) to do per-scanline CPU DMA siphons from cart-window
 * source bytes to WRAM, expanding the demo's per-frame DMA budget
 * beyond the vblank+force-blank baseline. See [[fmv-player-design]]
 * for the broader design.
 *
 * Carved from the existing payload's top — payload now $0000–$67FF
 * (26 KB), HIRQ code $6800–$6BFF (1 KB), NMI code $6C00–$6FFF (1 KB),
 * HDMA tables $7000–$77FF (2 KB). The siphon source pool layout
 * lands in Phase 2.5b once the actual byte budget per frame is
 * verified — the bytes can either share payload space (host stages
 * them like any other DMA payload) or get a dedicated region. */
#define CW_OFF_HIRQ_CODE      0x6800u
#define CW_HIRQ_CODE_BYTES    0x0400u  /* 1 KB */
#define CW_OFF_NMI_VERSION    0x79B2u  /* host bumps on install */
#define CW_OFF_HIRQ_VERSION   0x79B3u  /* v2.26 — host bumps on HIRQ install */

/* v2.26 Phase 2.5b: HIRQ schedule struct (5 bytes).
 *   +0  HTIMEL (low byte of $4207 H-counter target, 0..339)
 *   +1  HTIMEH (high byte of $4208, bit 0 only)
 *   +2  VTIMEL (low byte of $4209 V-counter target, 0..261)
 *   +3  VTIMEH (high byte of $420A, bit 0 only)
 *   +4  NMITIMEN_BITS — caller-supplied bits 4 (HIRQ) and 5 (VIRQ).
 *                       Kernel ORs with $80 (NMI enable) before
 *                       writing $4200, so the NMI bit is never lost.
 *
 * v2.27.8 moved from $7900 to $79B4: original location was INSIDE the
 * INIDISP HDMA table region ($7868-$7967, 256 bytes), which
 * emit_inidisp_table rewrites every frame — that silently clobbered
 * the schedule with INIDISP value bytes. The new location at $79B4
 * is past the INIDISP HDMA region and after the HIRQ_VERSION byte,
 * in clean per-byte-defined space.
 *
 * Programmed by the kernel every main-loop iteration so the guest
 * can dynamically reconfigure by writing the cart_window bytes; the
 * next iteration picks them up. Initial state = all zeros = HIRQ
 * disabled (kernel writes NMITIMEN=$80, just NMI). */
#define CW_OFF_HIRQ_SCHED       0x79B4u
#define CW_OFF_HIRQ_SCHED_HTIMEL    (CW_OFF_HIRQ_SCHED + 0u)
#define CW_OFF_HIRQ_SCHED_HTIMEH    (CW_OFF_HIRQ_SCHED + 1u)
#define CW_OFF_HIRQ_SCHED_VTIMEL    (CW_OFF_HIRQ_SCHED + 2u)
#define CW_OFF_HIRQ_SCHED_VTIMEH    (CW_OFF_HIRQ_SCHED + 3u)
#define CW_OFF_HIRQ_SCHED_NMITIMEN  (CW_OFF_HIRQ_SCHED + 4u)

/* v2.29 Phase 3a: unified kernel layout + siphon config.
 *
 * Kernel layout (2 bytes at $79B9-$79BA):
 *   +0  top_lb     (0..127 — scanlines of top force-blank)
 *   +1  bottom_lb  (0..127 — scanlines of bottom force-blank)
 *
 * The kernel's NMI handler reads these at vblank, caches in WRAM, and
 * programs HIRQ to fire INIDISP transitions at the appropriate
 * scanlines. Default (0, 0) = full 224-line visible, no letterbox.
 *
 * Siphon config (7 bytes at $79BB-$79C1):
 *   +0  bytes_per_line  (0..32 — 0 disables siphon)
 *   +1  src_offset_lo   (low byte of source pool offset within bank $C0)
 *   +2  src_offset_hi
 *   +3  wram_dst_lo     (low byte of WRAM destination, where bytes accumulate)
 *   +4  wram_dst_mid    (mid byte; WRAM address is 17 bits)
 *   +5  wram_dst_hi     (high byte / bit 0)
 *   +6  reserved
 *
 * Siphon fires per-scanline during the visible region (between
 * top_lb and (224 - bottom_lb)), via the kernel's default HIRQ
 * handler. Each fire reads `bytes_per_line` bytes from cart_window
 * starting at src_offset (advancing src per line) and writes to
 * WMDATA ($2180), with WMADDR set to wram_dst at start of siphon.
 *
 * All values 0 = siphon disabled. */
#define CW_OFF_KERNEL_LAYOUT          0x79B9u
#define CW_OFF_KERNEL_LAYOUT_TOP_LB   (CW_OFF_KERNEL_LAYOUT + 0u)
#define CW_OFF_KERNEL_LAYOUT_BOT_LB   (CW_OFF_KERNEL_LAYOUT + 1u)

#define CW_OFF_SIPHON_CONFIG          0x79BBu
#define CW_OFF_SIPHON_BYTES           (CW_OFF_SIPHON_CONFIG + 0u)
#define CW_OFF_SIPHON_SRC_LO          (CW_OFF_SIPHON_CONFIG + 1u)
#define CW_OFF_SIPHON_SRC_HI          (CW_OFF_SIPHON_CONFIG + 2u)
#define CW_OFF_SIPHON_WRAM_LO         (CW_OFF_SIPHON_CONFIG + 3u)
#define CW_OFF_SIPHON_WRAM_MID        (CW_OFF_SIPHON_CONFIG + 4u)
#define CW_OFF_SIPHON_WRAM_HI         (CW_OFF_SIPHON_CONFIG + 5u)

/* v2.34 virtual-NMI chainer: frame-done strobe. The kernel reads this
 * once it has walked the last slot of the current sub-frame (the read
 * IS the signal). The host advances to the next sub-frame, or closes
 * the logical frame (bump frame_consumed + clear frame_ready). Replaces
 * the old port-7 advance trigger. */
#define CW_OFF_FRAME_DONE             0x79C1u

/* v2.40: live siphon HTIME knob (1 byte). The H-counter value at which the
 * per-scanline State-SIPHON IRQ fires — tune so the force-blank lands in the
 * right pillar / H-blank (after this line's visible pixels), not mid-visible.
 * Driven by $env:MG_SIPHON_HTIME so it can be swept without a rebuild. */
#define CW_OFF_SIPHON_HTIME           0x79C2u

/* v2.46: framebuffer VECTOR-SWAP mode flag (see copro.inc COPRO_FB_MODE). When
 * non-zero, the kernel hands the V-IRQ to fb_finish/fb_start (60-colour 3D band
 * transport) instead of the state-machine frame_dma. The copro sets it AFTER the
 * first full frame has applied the PPU batch/tilemap/palette (those persist). */
#define CW_OFF_FB_MODE                0x79C3u

/* DEBUG (v2.37m): burst-start budget probe window. The kernel reads
 * CW_OFF_DBG_BUDGET + (K_BYTES_REM>>8) on the first slot of each burst;
 * the host logs the offset (= budget in 256-byte units) to find why
 * depth-2 bursts start over-budget. 64-byte read-only window in the free
 * 0x79C2-0x7FFF gap. Remove with the kernel strobe once diagnosed. */
#define CW_OFF_DBG_BUDGET             0x7A00u

/* DEBUG (v2.40): siphon ground-truth strobe window (0x7A40-0x7A7F). The
 * kernel strobes CW_OFF_DBG_SIPHON + K_SIPHON_BYTES at every State B (arm
 * check), and CW_OFF_DBG_SIPHON + 40 once per State-SIPHON scanline. The
 * host logs both: confirms whether the config reaches the kernel (bytes) and
 * whether the per-scanline siphon actually fires. Remove once diagnosed. */
#define CW_OFF_DBG_SIPHON             0x7A40u

/* Sprite overlay (cursor + bullethole pool over FMV). Dedicated CLEAN region
 * 0x7A80-0x7D5F: above the payload (0x0000-0x6FFF = MG_FRAME_PAYLOAD_BYTES) so
 * promote_current's payload blit never touches it, and past the DBG strobe
 * windows (end 0x7A7F) so cart_window_read serves these as plain bytes. mgapi
 * writes them once at FMV start (M1) / per-frame for the live cursor (M2/M3);
 * the FMV producer adds DMA slots that push them to VRAM / CGRAM / OAM.
 *   CHR   : 2x 8x8 4bpp tiles (cursor 269, bullethole 270) -> VRAM word 28880
 *           (OBSEL base 0x6000; tile 269 = base + 269*16). Word 28864 (tile 268)
 *           is AVOIDED: at BG1 CHR base 0x4000 the FMV's BLANK_TILE (780) maps to
 *           28864, so a sprite tile there paints the crosshair into the FMV
 *           border on odd frames. BG1 never references tiles past 780, so 28880+
 *           is truly free.
 *   CGRAM : OBJ palettes 0-3 (64 entries) -> CGADD 128
 *   OAM   : full 544 B (sprite 0 = cursor, 1-127 hidden at y=240)            */
#define CW_OFF_SPR_CHR    0x7A80u   /* 64 B  (2 tiles)            */
#define CW_OFF_SPR_CGRAM  0x7AC0u   /* 128 B (OBJ pal 0-3)        */
#define CW_OFF_SPR_OAM    0x7B40u   /* 544 B (128 sprites)        */
#define CW_SPR_CHR_BYTES   64u
#define CW_SPR_CGRAM_BYTES 128u
#define CW_SPR_OAM_BYTES   544u
#define CW_SPR_OAM_ACTIVE_BYTES 256u   /* sprites 0-63 low table — 60 Hz push (cursor+holes 0-31, FFT bars 32-63) */
#define CW_SPR_VRAM_WORD   28880u   /* tile 269 @ OBSEL base 0x6000 (24576+269*16) */
#define CW_SPR_TILE_CURSOR 269u     /* OAM tile number for the cursor (CHR @ 28880) */
#define CW_SPR_TILE_HOLE   270u     /* OAM tile number for the bullethole          */
#define CW_SPR_OBSEL       0x03u    /* size pair 8/16, namesel 0, base 3 (0x6000) */

/* FFT spectrum-meter overlay (increment 2): fill-level + cap tiles uploaded
 * ONCE (FMV frame 0) to OBJ tile 271+, then driven by per-frame OAM only. The
 * OBJ region (word 28880+) is never touched by the FMV BG CHR (tops out at word
 * 28864), so a one-time upload persists. These regions sit above the OAM table
 * (0x7B40+544 = 0x7D60) in otherwise-free cart-window space. */
/* All 12 OBJ tiles (cursor 269, hole 270, fill 0-8 = 271..279, cap 280) live in
 * ONE contiguous region uploaded as a single early CHR slot to VRAM word 28880
 * — the cursor/hole have always landed reliably there, and folding the FFT fill
 * tiles into the same slot lets them land too (a separate late slot gets starved
 * by the cycle-budgeted chainer). */
#define CW_OFF_SPR_FFT_CHR   0x7D60u   /* 384 B (12 tiles: cursor,hole,fill0-8,cap) */
#define CW_OFF_SPR_FFT_CGRAM 0x7EE0u   /* 128 B: OBJ palettes 4-7 (gradient+cap)  */
#define CW_SPR_FFT_CHR_BYTES   384u
#define CW_SPR_FFT_CGRAM_BYTES 128u
#define CW_SPR_FFT_TILE0       271u    /* tiles 271..279 = fill level 0..8        */
#define CW_SPR_FFT_TILE_CAP    280u    /* peak-cap tile                           */
#define CW_SPR_FFT_VRAM_WORD   28880u  /* tile 269 @ OBSEL base 0x6000 — combined start */
#define CW_SPR_FFT_CGADD       192u    /* OBJ palette 4 = CGRAM word 128 + 4*16   */

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
    uint8_t  cgwsel;      /* $2130 CGWSEL — colour-math source select */
    uint8_t  cgadsub;     /* $2131 CGADSUB — colour-math designation  */
    uint8_t  _reserved[3];
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

/* v2.18: bump the per-app NMI version byte at CW_OFF_NMI_VERSION.
 * Called by SYS_MG_NMI_INSTALL after staging the new handler bytes.
 * Wraps at 256 — the kernel @loop only checks for inequality vs its
 * cached value, so wrap-around still triggers correctly. */
void cart_window_bump_nmi_version(void);

/* v2.30.9 Phase 3b: write a 16-bit value into the cart window at
 * `offset` (low byte at offset, high byte at offset+1). Done as a
 * single 16-bit store (compiles to one mov on x86) so it's
 * byte-atomic against the SNES side's reads. Use this instead of
 * cart_window_load_blob for any 2-byte struct the SNES kernel reads
 * with `rep #$20 / lda f:abs` to avoid torn reads on host/SNES
 * thread overlap. */
void cart_window_store_u16_le(uint32_t offset, uint16_t value);

/* v2.30.7 Phase 3b: hook called when g_frame_consumed bumps.
 * Wires the cart_window's frame-ack signal to the VM scheduler so
 * BLOCK_FRAME_CONSUMED waiters wake. Registered by mgapi_vm_init. */
typedef void (*CartWindowFrameConsumedHook)(uint32_t now_consumed, void *userdata);
void cart_window_set_frame_consumed_hook(CartWindowFrameConsumedHook hook,
                                         void *userdata);

/* v2.21: clear the NMI version byte to 0. The kernel @loop reads this
 * as "uninstall — restore RAMVEC_NMI to the default kernel proc" so
 * the next demo gets a clean default handler instead of inheriting
 * the previous demo's installed NMI. Called from the VM unload hook
 * (vm_init.c) at demo teardown. */
void cart_window_clear_nmi_version(void);

/* v2.26: HIRQ version (CW_OFF_HIRQ_VERSION). Same semantics as the
 * NMI version pair — host bumps on install, clears to 0 on unload;
 * kernel polls and copies code into WRAM + rewrites the IRQ vector
 * at $0202 when the version changes. */
void cart_window_bump_hirq_version(void);
void cart_window_clear_hirq_version(void);

/* Read one pad word back. i = 0..3 (P0..P3); out-of-range returns 0.
 * Used by the SYS_COPRO_READ_PADS ecall handler. */
uint16_t cart_window_get_pads(unsigned i);

/* Port-2 mouse mailbox (see cart_window.c). post accumulates dx/dy deltas +
 * latches buttons (bit0=left, bit1=right); consume returns the accumulated
 * delta + buttons and zeroes the delta. */
void cart_window_post_mouse(int dx, int dy, uint8_t buttons);
void cart_window_consume_mouse(int *dx, int *dy, uint8_t *buttons);

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
