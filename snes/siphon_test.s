; ============================================================
;  siphon_test.s — END-TO-END per-scanline VRAM siphon.
;
;  The displayed CHR is cleared to black at init, then filled ONLY by the
;  per-scanline siphon: every visible scanline an H-IRQ fires EARLY (so the
;  prep finishes), then the minimal critical section lands in this line's
;  right-pillar / H-blank: force-blank, fire DMA (24 B into VRAM), unblank.
;  All DMA register setup happens BEFORE the force-blank (during the last
;  visible dots). Fire early enough (HTIME=100) that force-blank lands at
;  ~H=248 of THIS line, not the next line's visible region.
;
;    PASS -> full-screen 8-colour rainbow appears (siphoned in), stable,
;            no whole-line blanking. The per-scanline VRAM siphon works.
;    FAIL -> black / heavy blanking (force-blank mistimed -> sweep HTIME).
;
;  Verify in ares (cycle-accurate) as well as bsnes-plus.
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"

NTILES        = 128
CHR_BYTES     = NTILES * 32        ; 4096 (siphon fills this each frame-1)
TILEMAP_WORD  = $7C00
TILEMAP_BYTES = 32 * 32 * 2        ; 2048
PAL_BYTES     = 16 * 2             ; 32
HTIME_VAL     = 100                ; fire EARLY; force-blank lands ~H=248

src_off = $00                      ; DP $00-$01: 16-bit source byte offset

.segment "CODE"

.proc reset_handler
    .a8
    .i8
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
    sta INIDISP

    lda #$01
    sta BGMODE
    lda #$7C
    sta BG1SC
    stz BG12NBA
    lda #$01
    sta TM

    stz A1B0

    ; palette
    stz CGADD
    stz DMAP0
    lda #<CGDATA
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

    lda #$80
    sta VMAIN

    ; tilemap (cell C -> tile C&127)
    rep #$20
    .a16
    lda #TILEMAP_WORD
    sta VMADDL
    sep #$20
    .a8
    lda #$01
    sta DMAP0
    lda #<VMDATAL
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

    ; clear the displayed CHR (word 0..) to black via fixed-source zero
    rep #$20
    .a16
    stz VMADDL
    lda #.loword(zero_word)
    sta A1T0L
    lda #CHR_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$09                    ; 2 regs + fixed A-bus source
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; init siphon: source offset 0, VRAM write addr 0 (displayed CHR)
    rep #$20
    .a16
    stz src_off
    stz VMADDL
    sep #$20
    .a8

    lda #<HTIME_VAL
    sta HTIMEL
    lda #>HTIME_VAL
    sta HTIMEH
    stz VTIMEL
    stz VTIMEH

    lda #$0F
    sta INIDISP                 ; unblank

    lda #$10                    ; NMITIMEN: H-IRQ only (no NMI -> write-once fill)
    sta NMITIMEN

    cli
@idle:
    wai
    bra @idle
.endproc

; ------------------------------------------------------------------
;  IRQ — every scanline at H=HTIME_VAL. Until the CHR is filled: set up
;  the DMA OUTSIDE the force-blank, then force-blank / fire / unblank in
;  H-blank. VMADD auto-increments across lines (set to 0 once). No NMI =
;  write-once: after frame 1 the CHR is full, src_off caps, lines render
;  with no force-blank -> static clean rainbow.
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

    rep #$20
    .a16
    lda src_off
    cmp #CHR_BYTES
    bcs @done                   ; CHR filled -> no DMA, no force-blank
    ; setup OUTSIDE force-blank: A1T0 = chr_source + src_off, size 24
    clc
    adc #.loword(chr_source)
    sta A1T0L
    lda #24
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    ; --- critical section, lands in H-blank ---
    lda #$8F
    sta INIDISP                 ; force-blank
    lda #$01
    sta MDMAEN                  ; 24 B into displayed CHR (VMADD auto-incs)
    lda #$0F
    sta INIDISP                 ; unblank
    ; advance source offset
    rep #$20
    .a16
    lda src_off
    clc
    adc #24
    sta src_off
@done:
    sep #$20
    .a8
    lda $4211                   ; ack IRQ
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

.proc nmi_stub
    rti
.endproc

; ------------------------------------------------------------------
;  Data
; ------------------------------------------------------------------
.macro solid4 C
    .repeat 8
        .byte (C & 1) * $FF, ((C >> 1) & 1) * $FF
    .endrepeat
    .repeat 8
        .byte ((C >> 2) & 1) * $FF, ((C >> 3) & 1) * $FF
    .endrepeat
.endmacro

zero_word:
    .word $0000

palette_data:
    .word $0000
    .word $001F
    .word $03E0
    .word $7C00
    .word $03FF
    .word $7C1F
    .word $7FE0
    .word $7FFF
    .word $0210
    .repeat 7
        .word $0000
    .endrepeat

chr_source:
    .repeat NTILES, I
        solid4 ((I & 7) + 1)
    .endrepeat
    .repeat 24
        .byte $00
    .endrepeat

tilemap_data:
    .repeat 1024, I
        .word (I & 127)
    .endrepeat

; ------------------------------------------------------------------
;  HiROM header + vectors
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "SIPHON ENDTOEND TEST "    ; 21 chars
    .byte $31
    .byte $00
    .byte $07
    .byte $00
    .byte $01
    .byte $00
    .byte $00
    .word $AAAA
    .word $5555

.segment "VECTORS"
    .word $0000
    .word $0000
    .word $0000
    .word nmi_stub
    .word $0000
    .word irq_handler
    .word $0000
    .word $0000
    .word $0000
    .word $0000
    .word $0000
    .word nmi_stub
    .word reset_handler
    .word irq_handler
