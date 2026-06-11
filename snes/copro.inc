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
; MODE7_BATCH down by $F0 ($100 - $10) accordingly.
COPRO_HDMA_CONFIG  = (COPRO_BANK << 16) | $7968
COPRO_MODE7_BATCH  = (COPRO_BANK << 16) | $79A0
; HDMA tables relocated to $6000 (carved from the back of the payload area)
; with size bumped from 1280 to 4096 bytes so mode7_3d's hybrid-encoded
; M7A-D tables (~1800 bytes for 4 channels) fit. Payload area shrinks to
; $0000-$5FFF (24KB, still ample for typical demo per-frame DMA staging).
COPRO_HDMA_TABLES  = (COPRO_BANK << 16) | $6000
; Moved from $7F00 to share the gap with STROBE_BOOTED (see below).
COPRO_STATUS    = (COPRO_BANK << 16) | $79B1

; v2.18: per-app custom NMI handler region. The host stages 65816 bytes
; at COPRO_NMI_CODE_L when the guest calls SYS_MG_NMI_INSTALL, and bumps
; COPRO_NMI_VERSION_L. Kernel @loop polls the version, copies the region
; into WRAM at K_NMI_CODE_BASE when changed, and updates RAMVEC_NMI.
COPRO_NMI_CODE_L      = (COPRO_BANK << 16) | $6C00
COPRO_NMI_CODE_BYTES  = $0400          ; 1 KB
COPRO_NMI_VERSION_L   = (COPRO_BANK << 16) | $79B2

; v2.26 Phase 2.5: per-app HIRQ handler region. Same shape as the NMI
; region — guest stages bytes via SYS_MG_HIRQ_INSTALL, kernel polls
; COPRO_HIRQ_VERSION_L, byte-copies the region into WRAM at
; K_HIRQ_CODE_BASE, and rewrites RAMVEC_IRQ.
COPRO_HIRQ_CODE_L     = (COPRO_BANK << 16) | $6800
COPRO_HIRQ_CODE_BYTES = $0400          ; 1 KB
COPRO_HIRQ_VERSION_L  = (COPRO_BANK << 16) | $79B3

; v2.26 Phase 2.5b: HIRQ schedule struct (see cart_window.h
; CW_OFF_HIRQ_SCHED). Kernel reads these 5 bytes each main-loop
; iteration and programs $4207-$420A + $4200 (NMITIMEN).
; v2.27.8 moved from $7900 → $79B4 to escape the INIDISP HDMA table
; region ($7868-$7967) which the host rewrites every frame.
COPRO_HIRQ_SCHED_L            = (COPRO_BANK << 16) | $79B4
COPRO_HIRQ_SCHED_HTIMEL_L     = (COPRO_BANK << 16) | $79B4
COPRO_HIRQ_SCHED_HTIMEH_L     = (COPRO_BANK << 16) | $79B5
COPRO_HIRQ_SCHED_VTIMEL_L     = (COPRO_BANK << 16) | $79B6
COPRO_HIRQ_SCHED_VTIMEH_L     = (COPRO_BANK << 16) | $79B7
COPRO_HIRQ_SCHED_NMITIMEN_L   = (COPRO_BANK << 16) | $79B8

; v2.29 Phase 3a: kernel layout + siphon config — see cart_window.h
; CW_OFF_KERNEL_LAYOUT / CW_OFF_SIPHON_CONFIG. Read by the NMI handler
; at vblank to set up the next frame's HIRQ schedule.
COPRO_LAYOUT_TOP_LB_L         = (COPRO_BANK << 16) | $79B9
COPRO_LAYOUT_BOT_LB_L         = (COPRO_BANK << 16) | $79BA
COPRO_SIPHON_BYTES_L          = (COPRO_BANK << 16) | $79BB
COPRO_SIPHON_SRC_LO_L         = (COPRO_BANK << 16) | $79BC
COPRO_SIPHON_SRC_HI_L         = (COPRO_BANK << 16) | $79BD
COPRO_SIPHON_WRAM_LO_L        = (COPRO_BANK << 16) | $79BE
COPRO_SIPHON_WRAM_MID_L       = (COPRO_BANK << 16) | $79BF
COPRO_SIPHON_WRAM_HI_L        = (COPRO_BANK << 16) | $79C0

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
K_NMI_VERSION   = $022A         ; v2.18: cached COPRO_NMI_VERSION (last value
                                ; we acted on by copying the cart-window NMI
                                ; region to K_NMI_CODE_BASE). Initialized to
                                ; $00 by boot.s (WRAM is zeroed).
K_NMI_CODE_BASE = $0E00         ; v2.18: runnable custom NMI handler lives in
                                ; this 1 KB WRAM window. Initial RAMVEC_NMI
                                ; still points at the kernel.s nmi proc; the
                                ; first SYS_MG_NMI_INSTALL triggers a copy
                                ; here + RAMVEC_NMI rewrite to this address.
K_NMI_DEFAULT   = $022C         ; v2.21: cached address of the default kernel
                                ; NMI proc, saved at kmain init. The @loop
                                ; version-poll restores RAMVEC_NMI from this
                                ; on "uninstall" (cart version rolls back to
                                ; 0, which the host writes on VM unload), so
                                ; the next demo gets a clean default handler
                                ; instead of inheriting the previous demo's.
K_HIRQ_VERSION  = $022E         ; v2.26: cached COPRO_HIRQ_VERSION (last value
                                ; we acted on). Zeroed at kmain init.
K_HIRQ_CODE_BASE = $1200        ; v2.26: runnable HIRQ handler lives in this
                                ; 1 KB WRAM window. Initial RAMVEC_IRQ is
                                ; 0 (no handler); first SYS_MG_HIRQ_INSTALL
                                ; triggers a copy here + RAMVEC_IRQ rewrite.

; v2.29 Phase 3a: kernel layout + siphon WRAM cache. The NMI handler
; reads cart_window once per vblank and caches here; the default HIRQ
; ISR uses these values without further cart_window reads (cheaper
; per-fire). All zeros = no letterbox, no siphon.
K_LAYOUT_TOP_LB     = $0230     ; cached top letterbox line count
K_LAYOUT_BOT_LB     = $0231     ; cached bottom letterbox line count
K_LAYOUT_VIS_END    = $0232     ; computed: 224 - bot_lb (first bottom-LB line)

K_SIPHON_BYTES      = $0233     ; cached bytes_per_line (0 = disabled)
K_SIPHON_SRC_LO     = $0234     ; current source pointer (within bank $C0)
K_SIPHON_SRC_HI     = $0235
K_SIPHON_SRC_BASE_LO= $0236     ; reset value (NMI reloads SRC_LO from here)
K_SIPHON_SRC_BASE_HI= $0237
K_SIPHON_WRAM_LO    = $0238     ; WMADDR value to set at siphon start
K_SIPHON_WRAM_MID   = $0239
K_SIPHON_WRAM_HI    = $023A

K_FRAME_STATE       = $023B     ; per-frame ISR state (see kernel.s default_hirq)
K_CURRENT_LINE      = $023C     ; cached V counter target (for write-only VTIMEL)
K_HIRQ_DEFAULT      = $023D     ; cached address of default_hirq_handler so the
                                ; @loop's HIRQ uninstall path can restore it
                                ; (2 bytes — same shape as K_NMI_DEFAULT)
