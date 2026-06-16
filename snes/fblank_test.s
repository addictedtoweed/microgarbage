; ============================================================
;  fblank_test.s — validate mid-frame forced-blank VRAM DMA.
;
;  THE QUESTION: on real SNES (and bsnes-plus), does asserting
;  forced blank (INIDISP bit 7) while the PPU is mid-active-display
;  grant CPU-DMA write access to VRAM, the same way vblank does?
;
;  If yes, the microgarbage kernel can run a "virtual NMI" DMA burst
;  during a bottom-letterbox force-blank region (V well inside the
;  visible 0..223 range) and reclaim those scanlines for VRAM
;  bandwidth. If no, only the real vblank window is usable.
;
;  METHOD:
;    * BG1 Mode 1, 4bpp. Tilemap = every cell -> tile 0.
;    * Tile 0's CHR (32 B) decides the whole screen's color.
;    * Real NMI (vblank):  DMA the RED  pattern into tile 0.
;    * V-IRQ at line 112:  force-blank, DMA the GREEN pattern into
;                          tile 0, unblank. Line 112 is dead center
;                          of the visible region -- the most hostile
;                          possible spot for the write.
;
;    The PPU fetches CHR per scanline, so lines 0..111 render with
;    the NMI's RED tile 0 and lines 112..223 render with whatever
;    tile 0 holds AFTER the IRQ. Therefore:
;
;      PASS  -> clean RED top / GREEN bottom split at line 112
;               (mid-frame forced-blank DMA landed).
;      FAIL  -> whole screen RED
;               (the line-112 write was dropped; only vblank works).
;
;    The RED-each-vblank + GREEN-each-line-112 reset makes the split
;    STABLE frame to frame instead of collapsing to all-green after
;    one frame.
;
;  Load in bsnes-plus (and ares for a hardware-accurate cross-check).
;
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"

TILE0_BYTES   = 32              ; one 4bpp tile
TILEMAP_WORD  = $7C00           ; BG1 tilemap VRAM word address
TILEMAP_BYTES = 32 * 32 * 2     ; 2048
PAL_BYTES     = 16 * 2          ; 32
IRQ_LINE      = 112             ; mid-visible scanline for the IRQ

.segment "CODE"

; ------------------------------------------------------------------
;  reset_handler
; ------------------------------------------------------------------
.proc reset_handler
    .a8
    .i8
    sei
    clc
    xce                         ; native mode
    rep #$38                    ; A/X/Y 16-bit, decimal off
    .a16
    .i16
    ldx #$1FFF
    txs
    lda #$0000
    tcd                         ; direct page 0
    sep #$20
    .a8
    lda #$00
    pha
    plb                         ; DBR = $00

    ; Force blank during init.
    lda #$80
    sta INIDISP

    ; BG mode 1; BG1 CHR base $0000; BG1 tilemap @ $7C00.
    lda #$01
    sta BGMODE
    lda #$7C                    ; BG1SC: (TILEMAP_WORD>>10)<<2 = 31<<2 = $7C
    sta BG1SC
    stz BG12NBA                 ; BG1 CHR base = word $0000
    lda #$01
    sta TM                      ; main screen = BG1

    ; DMA channel 0 source bank = $00 (ROM via HiROM mirror).
    stz A1B0

    ; --- palette upload ---
    stz CGADD
    stz DMAP0                   ; 1B -> 1 reg, addr inc
    lda #<CGDATA                ; $22
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(palette_data)
    sta A1T0L
    lda #PAL_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    ; --- VRAM write mode: word step, inc after high byte ---
    lda #$80
    sta VMAIN

    ; --- tilemap upload (every cell -> tile 0) ---
    rep #$20
    .a16
    lda #TILEMAP_WORD
    sta VMADDL
    sep #$20
    .a8
    lda #$01                    ; 2B -> 2 regs ascending
    sta DMAP0
    lda #<VMDATAL               ; $18
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(tilemap_data)
    sta A1T0L
    lda #TILEMAP_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    ; --- seed tile 0 = RED so frame 0's top isn't garbage ---
    jsr dma_tile0_red

    ; --- arm interrupts: NMI (vblank) + V-IRQ at line IRQ_LINE ---
    lda #<IRQ_LINE
    sta VTIMEL
    lda #>IRQ_LINE
    sta VTIMEH
    stz HTIMEL                  ; H target 0 (V-IRQ mode ignores H when bit4=0)
    stz HTIMEH

    ; Unblank for the visible region.
    lda #$0F
    sta INIDISP

    ; NMITIMEN: NMI on (bit7) + V-counter IRQ (bit5) = $A0.
    lda #$A0
    sta NMITIMEN

    cli                         ; allow IRQ

@idle:
    wai
    bra @idle
.endproc

; ------------------------------------------------------------------
;  dma_tile0_red / dma_tile0_green — push 32 B into tile 0's CHR
;  (VRAM word $0000). Tiny DMA (~16 word writes), sub-scanline.
;  Caller guarantees VRAM is writable (vblank or forced blank).
;  Clobbers A (16/8), assumes DBR=$00. Leaves A 8-bit.
; ------------------------------------------------------------------
.proc dma_tile0_red
    .a8
    rep #$20
    .a16
    lda #$0000
    sta VMADDL                  ; tile 0 @ word 0
    lda #.loword(tile_red)
    sta A1T0L
    lda #TILE0_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    lda #$01                    ; 2B -> 2 regs ascending
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN
    rts
.endproc

.proc dma_tile0_green
    .a8
    rep #$20
    .a16
    lda #$0000
    sta VMADDL
    lda #.loword(tile_green)
    sta A1T0L
    lda #TILE0_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    lda #$01
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN
    rts
.endproc

; ------------------------------------------------------------------
;  NMI — fires at vblank. Reset tile 0 to RED so the next frame's
;  top half renders red. Ack via $4210.
; ------------------------------------------------------------------
.proc nmi_handler
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8
    lda RDNMI                   ; ack NMI
    jsr dma_tile0_red           ; vblank: VRAM writable naturally
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

; ------------------------------------------------------------------
;  IRQ — fires at line IRQ_LINE (mid visible). THE TEST: force blank,
;  DMA GREEN into tile 0, unblank, ack via $4211.
; ------------------------------------------------------------------
.proc irq_handler
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8

    lda #$80
    sta INIDISP                 ; force blank mid-active-display
    jsr dma_tile0_green         ; <-- does this VRAM write land?
    lda #$0F
    sta INIDISP                 ; unblank; lines below render the result

    lda $4211                   ; ack IRQ (read TIMEUP)

    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

; ------------------------------------------------------------------
;  Data
; ------------------------------------------------------------------

; Solid-color 4bpp tile builder: 8 rows of (p0,p1) then 8 of (p2,p3).
.macro solid_tile p0, p1, p2, p3
    .repeat 8
        .byte p0, p1
    .endrepeat
    .repeat 8
        .byte p2, p3
    .endrepeat
.endmacro

tile_red:                       ; palette index 1 (plane 0 only)
    solid_tile $FF, $00, $00, $00
tile_green:                     ; palette index 2 (plane 1 only)
    solid_tile $00, $FF, $00, $00

palette_data:
    .word $0000                 ; 0 black backdrop
    .word $001F                 ; 1 red
    .word $03E0                 ; 2 green
    .repeat 13
        .word $0000
    .endrepeat

tilemap_data:
    .repeat 1024
        .word $0000             ; every cell -> tile 0
    .endrepeat

; ------------------------------------------------------------------
;  HiROM header + vectors
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "FBLANK MIDFRAME TEST "    ; 21 chars
    .byte $31                          ; HiROM, FastROM
    .byte $00
    .byte $07
    .byte $00
    .byte $01
    .byte $00
    .byte $00
    .word $AAAA
    .word $5555

.segment "VECTORS"
    .word $0000              ; FFE4 native COP
    .word $0000              ; FFE6 native BRK
    .word $0000              ; FFE8 native ABORT
    .word nmi_handler        ; FFEA native NMI
    .word $0000              ; FFEC reserved
    .word irq_handler        ; FFEE native IRQ
    .word $0000              ; FFF0 reserved
    .word $0000              ; FFF2 reserved
    .word $0000              ; FFF4 emul COP
    .word $0000              ; FFF6 reserved
    .word $0000              ; FFF8 emul ABORT
    .word nmi_handler        ; FFFA emul NMI
    .word reset_handler      ; FFFC emul RESET
    .word irq_handler        ; FFFE emul IRQ/BRK
