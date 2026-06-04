; ============================================================
;  copro.inc — the SNES <-> coprocessor (STM32) interface.
;
;  The STM32 *is* the cartridge ROM. It decodes only a 16-bit
;  address (A0-A15), so one 64 KB window is MIRRORED across every
;  ROM bank (bank lines ignored, gated by ROM-select). Reads
;  return whatever the copro currently presents. The bus is
;  READ-ONLY: the SNES talks back to the copro purely by WHICH
;  address it reads -- the copro watches the address bus and decodes
;  the access (returned byte is don't-care). No write pins needed.
;
;  Boot: the copro holds the SNES in reset until the boot window
;  is primed, so at first fetch the window already holds the boot
;  image (boot.s + the RAM kernel blob + vectors, $8000-$FFFF).
;  After boot.s copies the kernel into WRAM and hands off, the
;  copro repurposes the window as the per-frame data channel.
;
;  CONTRACT the copro must honour at runtime:
;    * keep the TOP PAGE $FF00-$FFFF static (vectors + trampolines)
;    * serve the staged frame payload at COPRO_BANK:COPRO_DATA
;    * set ST_FRAME_RDY when a frame is staged
;    * decode the joypad/boot read-strobes (by address, on read)
;
;  Addresses below are PLACEHOLDERS — confirm against the mapper.
;  Public domain (CC0). No warranty.
; ============================================================

; --- runtime data window -------------------------------------
; The copro presents its 64 KB M7 DTCM as the cart ROM (16-bit decode,
; mirrored every bank). HiROM exposes the full $0000-$FFFF in banks
; $C0-$FF, so we reach the whole window flat through bank $C0 -- clear
; of WRAM/regs, which live in the low banks.
COPRO_BANK      = $C0           ; HiROM full-window bank

COPRO_DATA      = $0000         ; per-frame PPU payload the SNES DMAs (copro-laid)
; All of the copro-served addresses below are EXPLICITLY 24-bit
; (bank $C0 + offset), so any `lda f:<sym>` in kernel.s reads from
; the cart window. Previously these were 16-bit and ca65 silently
; zero-extended to bank $00 (open bus) -- which made every PPU
; register batch + HDMA config + DMA list read in the NMI handler
; return garbage, and the screen stayed black even with the kernel
; running and the demo committing frames.
COPRO_FRAME_RDY = (COPRO_BANK << 16) | $7800
COPRO_DMA_LIST  = (COPRO_BANK << 16) | $7808
COPRO_PPU_BATCH = (COPRO_BANK << 16) | $7848
COPRO_INIDISP_HDMA = (COPRO_BANK << 16) | $7868
; INIDISP HDMA area expanded from 16 to 256 bytes to hold a direct-mode
; table (one INIDISP value per scanline). bsnes-plus's HDMA emulation
; doesn't honor the repeat-mode bit, so we encode every line explicitly;
; the table doesn't fit in the old 16-byte slot. Shifts HDMA_CONFIG /
; MODE7_BATCH / HDMA_TABLES down by $F0 ($100 - $10) accordingly.
COPRO_HDMA_CONFIG  = (COPRO_BANK << 16) | $7968
COPRO_MODE7_BATCH  = (COPRO_BANK << 16) | $79A0
COPRO_HDMA_TABLES  = (COPRO_BANK << 16) | $7A00
; Moved from $7F00 to share the gap with STROBE_BOOTED (see below).
COPRO_STATUS    = (COPRO_BANK << 16) | $79B1

; --- per-frame DMA dispatch ------------------------------------------
; The kernel checks COPRO_FRAME_RDY in NMI; 0 means the copro is mid-stage
; (do nothing this vblank, the previous frame stays on screen). When the
; copro finishes staging it writes a non-zero byte there, and the kernel
; walks the 8-slot DMA list, programming channel 0 from each slot and
; firing one MDMAEN per slot.
;
; Each slot is 8 bytes:
;     +0  bbus  -- $21xx low byte (the PPU register the DMA writes to):
;                    $22 = CGDATA   -- prep writes the byte to CGADD ($2121)
;                    $18 = VMDATAL  -- prep writes the word to VMADDL/H + sets VMAIN=$80
;                    $04 = OAMDATA  -- prep writes the word to OAMADDL/H
;                  0 means "empty slot, skip" -- the kernel doesn't fire it.
;     +1  dmap  -- SNES DMAP byte (bit 7 direction; bits 0-2 transfer pattern;
;                  bit 4 fixed-A; etc.). Typical copro values:
;                    $00 = 1-byte-to-1-reg, A increments (CGRAM, OAM)
;                    $01 = 2-bytes-to-2-regs (lo/hi), A increments (VRAM)
;     +2-3  src  -- source offset within COPRO_BANK (16-bit; bank is implicit).
;     +4-5  size -- byte count (DAS, 16-bit).
;     +6-7  prep -- pre-transfer value the kernel writes to the PPU dest register
;                  (CGADD = low byte; VMADD = 16-bit word addr; OAMADDR = 16-bit).
;
; The kernel walks all 8 slots every "frame_ready" vblank, processing the ones
; where bbus != 0. Slots are independent: putting CGRAM in slot 0, tilemap in
; slot 1, and CHR in slot 2 (the FMV case) is the same shape as putting all
; three OAM updates into slots 0-2 (a sprite-only scene). The copro composes
; whatever it needs each frame.

; SNES -> copro: WRITE-FREE. The copro decodes which address we READ.
; Eight PAGE-ALIGNED joypad ports carry 4 pads x 2 bytes (multitap-capable).
; Reading base+byte with an 8-bit index can never cross the page (no 65816
; indexed dummy-read), so each access is a single clean bus read the copro
; latches as the pad's byte. Pad layout:
;     P0 = port 1 main         P1 = port 2 main
;     P2 = port 1 multitap #2  P3 = port 2 multitap #2
JOYPORT_P0_LO   = $7000         ; [read base + pad0 lo]  -> copro latches A0-A7
JOYPORT_P0_HI   = $7100
JOYPORT_P1_LO   = $7200
JOYPORT_P1_HI   = $7300
JOYPORT_P2_LO   = $7400
JOYPORT_P2_HI   = $7500
JOYPORT_P3_LO   = $7600
JOYPORT_P3_HI   = $7700
; Moved from $7E00 (now inside HDMA tables pool $7A00-$7EFF after v1.20).
STROBE_BOOTED   = $79B0         ; [read] boot.s -> copro: running from RAM, go runtime

; long (24-bit) forms so reads hit COPRO_BANK with DBR left at $00
COPRO_STATUS_L    = (COPRO_BANK << 16) | COPRO_STATUS
COPRO_FRAME_RDY_L = (COPRO_BANK << 16) | COPRO_FRAME_RDY
COPRO_DMA_LIST_L  = (COPRO_BANK << 16) | COPRO_DMA_LIST
COPRO_PPU_BATCH_L = (COPRO_BANK << 16) | COPRO_PPU_BATCH

; --- PPU register batch layout (32 bytes at COPRO_PPU_BATCH) ----
; Single-byte regs in the first 16 bytes, write-twice 16-bit scrolls
; in the last 16. Matches PpuBatch in src/mgapi/cart_window.h — keep
; both in sync.
;
; The PB_* symbols carry the full 24-bit (bank $C0) address so that
; `lda f:PB_*` from kernel.s reads from $C0:$78xx (the cart window
; the copro stages) instead of $00:$78xx (open bus in bank zero,
; which previously returned garbage and made every PPU register
; write a no-op).
PB_BGMODE       = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $00)
PB_OBSEL        = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $01)
PB_BG1SC        = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $02)
PB_BG2SC        = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $03)
PB_BG3SC        = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $04)
PB_BG4SC        = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $05)
PB_BG12NBA      = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $06)
PB_BG34NBA      = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $07)
PB_TM           = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $08)
PB_TS           = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $09)
PB_MOSAIC       = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $0A)
PB_SCROLLS      = (COPRO_BANK << 16) | (COPRO_PPU_BATCH + $10)

; --- Mode 7 batch offsets --------------------------------------
; Same bank-prefix story as PB_*.
M7B_SEL         = (COPRO_BANK << 16) | (COPRO_MODE7_BATCH + $00)
M7B_A_LO        = (COPRO_BANK << 16) | (COPRO_MODE7_BATCH + $02)
M7B_B_LO        = (COPRO_BANK << 16) | (COPRO_MODE7_BATCH + $04)
M7B_C_LO        = (COPRO_BANK << 16) | (COPRO_MODE7_BATCH + $06)
M7B_D_LO        = (COPRO_BANK << 16) | (COPRO_MODE7_BATCH + $08)
M7B_X_LO        = (COPRO_BANK << 16) | (COPRO_MODE7_BATCH + $0A)
M7B_Y_LO        = (COPRO_BANK << 16) | (COPRO_MODE7_BATCH + $0C)

; --- INIDISP HDMA channel reservation --------------------------
; The kernel sets up HDMA channel 7 at boot to read from
; COPRO_INIDISP_HDMA, writing INIDISP every scanline per the
; mode-0 repeat segments. The runtime rebuilds the table each
; frame from mg_force_blank values.
COPRO_HDMA_INIDISP_CHANNEL = 7

COPRO_HDMA_CONFIG_L   = (COPRO_BANK << 16) | COPRO_HDMA_CONFIG
COPRO_HDMA_CONFIG_STRIDE = 8
JOYPORT_P0_LO_L = (COPRO_BANK << 16) | JOYPORT_P0_LO
JOYPORT_P0_HI_L = (COPRO_BANK << 16) | JOYPORT_P0_HI
JOYPORT_P1_LO_L = (COPRO_BANK << 16) | JOYPORT_P1_LO
JOYPORT_P1_HI_L = (COPRO_BANK << 16) | JOYPORT_P1_HI
JOYPORT_P2_LO_L = (COPRO_BANK << 16) | JOYPORT_P2_LO
JOYPORT_P2_HI_L = (COPRO_BANK << 16) | JOYPORT_P2_HI
JOYPORT_P3_LO_L = (COPRO_BANK << 16) | JOYPORT_P3_LO
JOYPORT_P3_HI_L = (COPRO_BANK << 16) | JOYPORT_P3_HI
STROBE_BOOTED_L = (COPRO_BANK << 16) | STROBE_BOOTED

; status bits (read at COPRO_STATUS)
ST_KERNEL_RDY   = $80           ; boot:    kernel blob staged in the window
; runtime frame-ready signalling moved to the COPRO_DMACTRL byte: a non-zero
; value there means "do these DMAs", zero means "skip this frame". The kernel's
; per-frame joypad post doubles as "prev frame consumed, go" so the copro can
; safely restage the payload + control byte for the next frame.

; --- per-frame DMA payload layout inside COPRO_DATA ----------
; matches the 4bpp Mode-1 plan: CGRAM | tilemap | CHR  (OAM later)
PL_CGRAM        = COPRO_DATA + $0000    ; 256 B  (128 colors)
PL_CGRAM_LEN    = 256
PL_TMAP         = COPRO_DATA + $0100    ; tilemap words
PL_TMAP_LEN     = 1560
PL_CHR          = COPRO_DATA + $0718    ; 4bpp CHR
PL_CHR_LEN      = 24960

; --- WRAM layout (bank $00 low RAM) --------------------------
;   $0000-$00FF direct page   $0100-$01FF stack
;   $0200-$02FF kernel vars    $0400-$1FFF kernel code (copied here)
RAMVEC_NMI      = $0200         ; RAM kernel installs its NMI handler addr here
RAMVEC_IRQ      = $0202         ; ...and its IRQ handler
                                ; $0210-$021F: free (was DMA_CACHE in the bitmask design)
PADS            = $0220         ; 4 pads x 16 bits = 8 bytes (PADS+0..+7); see
                                ; read_joypads in kernel.s for layout. Order
                                ; mirrors auto-joypad register format ($4218):
                                ;   bit 15..4 = B Y Sel St U D L R A X L R
                                ;   bit  3..0 = signature (0 for std pad)
