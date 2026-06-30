; ============================================================
;  dma_rate_test.s — measure the real per-line GP-DMA rate during blank, the
;  hard ceiling for the 3D renderer's rolling-buffer top-band burst.
;
;  Deterministic, emulator-independent design (no fragile unblank-band / letterbox
;  geometry): at vblank start (NMI) fire a fill GP-DMA of BYTES_PER_LINE * VB_LINES
;  bytes, then read the vblank flag ($4212 bit7):
;     still in vblank  -> the burst fit the blank   -> WHOLE SCREEN GREEN
;     vblank ended      -> the burst overran active  -> WHOLE SCREEN RED
;  All BG layers are off (TM=0), so the screen is the backdrop (CGRAM word 0),
;  which the NMI repaints green/red each frame -> a solid, unmistakable result.
;
;  Because the burst is sized BYTES_PER_LINE * VB_LINES, it fits the blank iff
;  BYTES_PER_LINE <= the real rate. So the green->red flip point across the sweep
;  IS the achievable bytes/line. (DMA timing is identical in vblank vs a
;  force-blanked letterbox line, so this rate applies to the renderer's full
;  vblank+letterbox window: capacity = rate * window_lines.)
;
;  READING IT: run the sweep low->high. Highest GREEN = real rate. On
;  bsnes-accuracy expect ~163 (170 bus minus DRAM refresh); ares looser.
;    ca65 --cpu 65816 -D BYTES_PER_LINE=160 -o build/r.o dma_rate_test.s
;    ld65 -C siphon_hblank_test.cfg -o build/dma_160.sfc build/r.o
;  Public domain (CC0).
; ============================================================
.p816
.include "snes.inc"

.ifndef BYTES_PER_LINE
BYTES_PER_LINE = 160
.endif
.ifndef SLOTS
SLOTS         = 1                  ; 1 = raw rate. 3 = real top-band burst shape
.endif                             ;     (4bpp CHR + 2bpp CHR + tilemap setups).
VB_LINES      = 38                 ; NTSC vblank ~ lines 225..262
BURST_BYTES   = BYTES_PER_LINE * VB_LINES
PER_SLOT      = BURST_BYTES / SLOTS

SCRATCH_VRAM  = 16000              ; off-screen VRAM word the fill targets
COL_GREEN     = $03E0              ; BGR555 green
COL_RED       = $001F              ; BGR555 red

ramvec        = $10

.segment "CODE"

.proc reset_handler
    sei
    clc
    xce
    rep #$38
    .a16
    .i16
    ldx #$1FFF
    txs
    lda #$0000
    tcd
    sep #$20
    .a8
    lda #$00
    pha
    plb

    lda #$80
    sta INIDISP             ; force-blank during setup

    stz BGMODE
    stz TM                  ; no BG layers -> screen shows the backdrop (CGRAM 0)
    stz A1B0

    ; backdrop starts green; NMI repaints it each frame
    stz CGADD
    lda #<COL_GREEN
    sta CGDATA
    lda #>COL_GREEN
    sta CGDATA

    lda #$80
    sta VMAIN               ; VRAM addr +1 after high byte

    lda #$0F
    sta INIDISP             ; unblank -> backdrop visible
    lda #$80
    sta NMITIMEN            ; enable vblank NMI
    cli
@idle:
    wai
    bra @idle
.endproc

; vblank NMI: fire the burst, then read the vblank flag to see if it fit.
.proc nmi_handler
    rep #$30
    .a16
    .i16
    pha
    sep #$20
    .a8

    ; burst: SLOTS fills of PER_SLOT bytes, each paying a full DMA setup so the
    ; measured rate includes the real per-slot overhead of the top-band burst.
    lda #$09                ; 2 regs ($2118/9) + FIXED A-bus source (all slots)
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    ldx #SLOTS
@slot:
    rep #$20
    .a16
    lda #SCRATCH_VRAM       ; same target every slot — only setup cost matters
    sta VMADDL
    lda #.loword(fill_word)
    sta A1T0L
    lda #PER_SLOT
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN              ; <-- CPU halts here for PER_SLOT * 8 master cycles
    dex
    bne @slot

    ; verdict: $4212 bit7 = vblank. Set if the whole burst stayed in blank.
    lda HVBJOY
    and #$80
    bne @fit

    ; overran -> red
    stz CGADD
    lda #<COL_RED
    sta CGDATA
    lda #>COL_RED
    sta CGDATA
    bra @done
@fit:
    stz CGADD
    lda #<COL_GREEN
    sta CGDATA
    lda #>COL_GREEN
    sta CGDATA
@done:
    lda RDNMI               ; ack NMI
    rep #$30
    .a16
    .i16
    pla
    rti
.endproc

.proc irq_stub
    rti
.endproc

; ------------------------------------------------------------------
fill_word:
    .word $0000

.segment "HEADER"
    .byte "DMA RATE TEST        "
    .byte $31
    .byte $00, $07, $00, $01, $00, $00
    .word $AAAA, $5555

.segment "VECTORS"
    .word $0000,$0000,$0000
    .word nmi_handler       ; FFEA native NMI
    .word $0000
    .word irq_stub          ; FFEE native IRQ
    .word $0000,$0000
    .word $0000,$0000,$0000
    .word nmi_handler       ; FFFA emu NMI
    .word reset_handler     ; FFFC
    .word irq_stub          ; FFFE
