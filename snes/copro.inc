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
COPRO_STATUS    = $7F00         ; [read]  handshake status byte (copro -> SNES)

; SNES -> copro: WRITE-FREE. The copro decodes which address we READ.
; The joypad ports are PAGE-ALIGNED, so reading base+byte with an 8-bit index
; can never cross the page (no 65816 indexed dummy-read) -- one clean bus
; access per port that the copro latches. Two ports carry the 16-bit pad;
; the returned data is don't-care.
JOYPORT_LO      = $7C00         ; [read base + pad-lo]  copro latches A0-A7
JOYPORT_HI      = $7D00         ; [read base + pad-hi]  copro latches A0-A7
STROBE_BOOTED   = $7E00         ; [read] boot.s -> copro: running from RAM, go runtime

; long (24-bit) forms so reads hit COPRO_BANK with DBR left at $00
COPRO_STATUS_L  = (COPRO_BANK << 16) | COPRO_STATUS
JOYPORT_LO_L    = (COPRO_BANK << 16) | JOYPORT_LO
JOYPORT_HI_L    = (COPRO_BANK << 16) | JOYPORT_HI
STROBE_BOOTED_L = (COPRO_BANK << 16) | STROBE_BOOTED

; status bits (read at COPRO_STATUS)
ST_KERNEL_RDY   = $80           ; boot:    kernel blob staged in the window
ST_FRAME_RDY    = $01           ; runtime: next frame's payload is staged
; (the kernel's per-frame joypad post doubles as "prev frame consumed, go" --
;  no separate ack strobe needed. A frame counter would beat a single RDY bit
;  if the clear-race ever bites; TODO.)

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
KFRAME_FLAG     = $0210         ; main loop <-> NMI: 1 = run the DMA burst
