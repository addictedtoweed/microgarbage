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
COPRO_FRAME_RDY = $7800         ; [read]  1 byte: 0 = skip this frame, else process the list
COPRO_DMA_LIST  = $7808         ; [read]  8 slots * 8 bytes = $7808-$7847 (see below)
COPRO_PPU_BATCH = $7848         ; [read]  32 bytes: PPU register batch the kernel writes
                                ;         to PPU regs at the top of each vblank (see below)
COPRO_INIDISP_HDMA = $7868      ; [read]  16 bytes: HDMA mode-0 table the kernel reserves
                                ;         channel 7 for, INIDISP toggle for letterbox
                                ;         force-blank. Format: [0x80|N][value] segments
                                ;         then 0x00 terminator.
COPRO_STATUS    = $7F00         ; [read]  handshake status byte (copro -> SNES)

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
STROBE_BOOTED   = $7E00         ; [read] boot.s -> copro: running from RAM, go runtime

; long (24-bit) forms so reads hit COPRO_BANK with DBR left at $00
COPRO_STATUS_L    = (COPRO_BANK << 16) | COPRO_STATUS
COPRO_FRAME_RDY_L = (COPRO_BANK << 16) | COPRO_FRAME_RDY
COPRO_DMA_LIST_L  = (COPRO_BANK << 16) | COPRO_DMA_LIST
COPRO_PPU_BATCH_L = (COPRO_BANK << 16) | COPRO_PPU_BATCH

; --- PPU register batch layout (32 bytes at COPRO_PPU_BATCH) ----
; Single-byte regs in the first 16 bytes, write-twice 16-bit scrolls
; in the last 16. Matches PpuBatch in src/mgapi/cart_window.h — keep
; both in sync.
PB_BGMODE       = COPRO_PPU_BATCH + $00
PB_OBSEL        = COPRO_PPU_BATCH + $01
PB_BG1SC        = COPRO_PPU_BATCH + $02
PB_BG2SC        = COPRO_PPU_BATCH + $03
PB_BG3SC        = COPRO_PPU_BATCH + $04
PB_BG4SC        = COPRO_PPU_BATCH + $05
PB_BG12NBA      = COPRO_PPU_BATCH + $06
PB_BG34NBA      = COPRO_PPU_BATCH + $07
PB_TM           = COPRO_PPU_BATCH + $08
PB_TS           = COPRO_PPU_BATCH + $09
PB_MOSAIC       = COPRO_PPU_BATCH + $0A
PB_SCROLLS      = COPRO_PPU_BATCH + $10  ; 8 16-bit scrolls follow

; --- INIDISP HDMA channel reservation --------------------------
; The kernel sets up HDMA channel 7 at boot to read from
; COPRO_INIDISP_HDMA, writing INIDISP every scanline per the
; mode-0 repeat segments. The runtime rebuilds the table each
; frame from mg_force_blank values.
COPRO_HDMA_INIDISP_CHANNEL = 7
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
