; ============================================================
;  hdma_vram_test.s — HDMA VRAM streaming (v3, per Gemini/TmEE guidance).
;
;  TWO HDMA data ops + auto-increment (NOT per-line VMADD rewrite):
;    ch0  INIDISP HDMA = $80 (force-blank, repeat)   -- frees VRAM each H-blank
;    ch2  VMDATA  HDMA  mode 5 (4 B/line), CONTINUOUS -- streams diagonal CHR
;    ch7  INIDISP HDMA = $0F (unblank, repeat)        -- restores display
;  NO ch1: VMADD is auto-incremented by ch2's writes. It's RE-SEEDED once per
;  frame by a tiny NMI (vblank, VRAM-safe) back to STREAM_VRAM. Single mode-5
;  channel only (multi-channel B-bus contention is what mangles).
;
;  SINGLE buffer, race-free: stream lines 0..119 write row-25 CHR long before
;  the beam shows row 25 at V~200.
;
;  Streams 15 tiles. PASS = bottom row's first 15 tiles = clean continuous
;  diagonals, whole screen rendered, in ares AND bsnes-accuracy.
; ============================================================
.p816
.include "snes.inc"

TW            = 30
TH            = 26
NTILES        = TW * TH
BURST_TILES   = 25 * TW            ; 750
STREAM_TILES  = 15
TILE_WORDS    = 16
STREAM_VRAM   = BURST_TILES * TILE_WORDS   ; 12000
STREAM_LINES  = STREAM_TILES * 8           ; 120

CHR_A_WORD    = $0000
TM_A_WORD     = $7C00

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
    sta INIDISP

    lda #$01
    sta BGMODE
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
    lda #(16*2)
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    lda #$80
    sta VMAIN

    ; static top -> CHR_A
    rep #$20
    .a16
    lda #CHR_A_WORD
    sta VMADDL
    lda #.loword(chr_top)
    sta A1T0L
    lda #(BURST_TILES*32)
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; tilemap -> TM_A
    rep #$20
    .a16
    lda #TM_A_WORD
    sta VMADDL
    lda #.loword(tilemap_data)
    sta A1T0L
    lda #(32*32*2)
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    lda #$7C               ; BG1SC TM_A
    sta BG1SC
    lda #$00               ; BG12NBA CHR_A
    sta BG12NBA

    ; ===== HDMA: ch0 blank, ch2 data, ch7 unblank (NO ch1) =====
    lda #$00
    sta $4300              ; DMAP0 mode 0
    lda #<INIDISP
    sta $4301              ; BBAD0 $00
    rep #$20
    .a16
    lda #.loword(ib_blank)
    sta $4302
    sep #$20
    .a8
    stz $4304

    lda #$05
    sta $4320              ; DMAP2 mode 5
    lda #<VMDATAL
    sta $4321              ; BBAD2 $18
    rep #$20
    .a16
    lda #.loword(hdma_chr)
    sta $4322
    sep #$20
    .a8
    stz $4324

    lda #$00
    sta $4370
    lda #<INIDISP
    sta $4371
    rep #$20
    .a16
    lda #.loword(ib_show)
    sta $4372
    sep #$20
    .a8
    stz $4374

    ; seed VMADD once (NMI re-seeds it each frame)
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda #STREAM_VRAM
    sta VMADDL
    sep #$20
    .a8

    lda #$0F
    sta INIDISP
    lda #($01|$04|$80)     ; HDMAEN ch0, ch2, ch7
    sta HDMAEN
    lda #$80
    sta NMITIMEN           ; NMI on (vblank) to re-seed VMADD
    cli
@idle:
    wai
    bra @idle
.endproc

; NMI: re-seed VMADD to the stream start each frame (vblank = VRAM-safe).
.proc nmi_handler
    rep #$30
    .a16
    .i16
    pha
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda #STREAM_VRAM
    sta VMADDL
    sep #$20
    .a8
    lda RDNMI             ; ack NMI ($4210)
    rep #$30
    .a16
    .i16
    pla
    rti
.endproc

.proc nmi_stub
    rti
.endproc

; ------------------------------------------------------------------
.macro diag4 PH
    .repeat 8, R
        .byte ($80 >> ((R + PH) & 7)), $00
    .endrepeat
    .res 16, $00
.endmacro

palette_data:
    .word $0000,$001F,$03E0,$7C00,$03FF,$7C1F,$7FE0,$7FFF
    .word $4210,$001A,$0340,$6800,$0210,$0000,$0000,$03FF

chr_top:
    .repeat BURST_TILES, I
        diag4 I
    .endrepeat

tilemap_data:
    .repeat 32, R
        .repeat 32, C
            .if (R >= 1) && (R <= TH) && (C >= 1) && (C <= TW)
                .word ((R - 1) * TW + (C - 1))
            .else
                .word NTILES
            .endif
        .endrepeat
    .endrepeat

ib_blank:
    .byte STREAM_LINES, $80
    .byte 0
ib_show:
    .byte STREAM_LINES, $0F
    .byte 0

; ch2 mode 5 CONTINUOUS: inline diagonal CHR, 4 B/line, A1T advances -> streams.
hdma_chr:
    .byte $80 | STREAM_LINES
    .repeat STREAM_TILES, I
        diag4 (BURST_TILES + I)
    .endrepeat
    .byte 0

.segment "HEADER"
    .byte "HDMA VRAM TEST       "
    .byte $31
    .byte $00, $07, $00, $01, $00, $00
    .word $AAAA, $5555

.segment "VECTORS"            ; starts $FFE4
    .word $0000,$0000,$0000   ; FFE4,FFE6,FFE8
    .word nmi_handler         ; FFEA native NMI
    .word $0000               ; FFEC
    .word nmi_stub            ; FFEE native IRQ
    .word $0000,$0000         ; FFF0,FFF2
    .word $0000,$0000,$0000   ; FFF4,FFF6,FFF8
    .word nmi_stub            ; FFFA emulation NMI (unused in native)
    .word reset_handler       ; FFFC
    .word nmi_stub            ; FFFE emulation IRQ
