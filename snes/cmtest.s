; ============================================================
;  cmtest.s — dual-layer color-math proof (foundation for the high-colour cube).
;
;  Mode 1: BG1 (4bpp) solid RED on the MAIN screen + BG3 (2bpp) solid BLUE on the
;  SUB screen, combined by half-add colour math. Expected result: the WHOLE screen
;  is PURPLE = (red+blue)/2. That proves the Mode-1 + TS/CGWSEL/CGADSUB pipeline
;  the 4bpp+2bpp cube will use.
;    red  only            -> colour math is OFF / sub not enabled
;    blue only            -> layers swapped
;    black                -> setup broken
;    dim purple           -> WORKS
;
;  Build: ca65 --cpu 65816 -o build/cmtest.o cmtest.s
;         ld65 -C cube3d.cfg -o build/cmtest.sfc build/cmtest.o
;         perl fixsum.pl build/cmtest.sfc 0x7FC0
;  Public domain (CC0).
; ============================================================
.p816
.smart +
.include "snes.inc"

; VRAM word layout (4096-word aligned char bases for 4bpp/2bpp BGs)
BG1_CHR  = $0000       ; BG1 4bpp tile 0
BG3_CHR  = $1000       ; BG3 2bpp tile 0 (char base unit 1)
BG1_MAP  = $2000       ; BG1 tilemap
BG3_MAP  = $2800       ; BG3 tilemap

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
    sta INIDISP            ; force-blank during setup

    ; --- mode 1: BG1/BG2 4bpp, BG3 2bpp ---
    lda #$01
    sta BGMODE
    stz BG12NBA            ; BG1 char base = BG1_CHR/$1000 = 0
    lda #$01
    sta BG34NBA            ; BG3 char base = BG3_CHR/$1000 = 1
    lda #$20              ; BG1SC: map base BG1_MAP/$400 = 8 -> 8<<2
    sta BG1SC
    lda #$28              ; BG3SC: map base BG3_MAP/$400 = 10 -> 10<<2
    sta BG3SC
    stz BG1HOFS
    stz BG1HOFS
    stz BG1VOFS
    stz BG1VOFS
    stz BG3HOFS
    stz BG3HOFS
    stz BG3VOFS
    stz BG3VOFS

    ; --- palette ---
    stz CGADD
    ; 0 = black backdrop
    stz CGDATA
    stz CGDATA
    ; 1 = red (BG1 colour 1)
    lda #$1F
    sta CGDATA
    lda #$00
    sta CGDATA
    ; 2,3,4 unused
    ldx #(3*2)
@pz:
    stz CGDATA
    dex
    bne @pz
    ; 5 = blue (BG3 palette 1, colour 1)
    lda #$00
    sta CGDATA
    lda #$7C
    sta CGDATA

    ; --- BG1 4bpp tile 0 = solid colour 1 (plane0 all $FF) ---
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda #BG1_CHR
    sta VMADDL
    sep #$20
    .a8
    ldx #8                ; 8 rows: plane0=$FF, plane1=$00
@b1r:
    lda #$FF
    sta VMDATAL
    stz VMDATAH
    dex
    bne @b1r
    ldx #8                ; planes 2,3 = 0
@b1z:
    stz VMDATAL
    stz VMDATAH
    dex
    bne @b1z

    ; --- BG3 2bpp tile 0 = solid colour 1 ---
    rep #$20
    .a16
    lda #BG3_CHR
    sta VMADDL
    sep #$20
    .a8
    ldx #8
@b3r:
    lda #$FF
    sta VMDATAL
    stz VMDATAH
    dex
    bne @b3r

    ; --- BG1 tilemap = all tile 0, palette 0 ($0000) ---
    rep #$20
    .a16
    lda #BG1_MAP
    sta VMADDL
    ldx #1024
    lda #$0000
@m1:
    sta VMDATAL
    dex
    bne @m1

    ; --- BG3 tilemap = all tile 0, palette 1 ($0400) ---
    lda #BG3_MAP
    sta VMADDL
    ldx #1024
    lda #$0400
@m3:
    sta VMDATAL
    dex
    bne @m3
    sep #$20
    .a8

    ; --- screens + colour math ---
    lda #$01
    sta TM                ; BG1 on main screen
    lda #$04
    sta TS                ; BG3 on sub screen
    lda #$02
    sta CGWSEL            ; colour math always; use sub screen (bit1)
    lda #$41
    sta CGADSUB          ; BG1 participates; half; add

    lda #$0F
    sta INIDISP           ; unblank
    stz NMITIMEN
@idle:
    wai
    bra @idle
.endproc

.proc stub
    rti
.endproc

.segment "HEADER"
    .byte "CMTEST               "
    .byte $20
    .byte $00, $05, $00, $01, $00, $00
    .word $AAAA, $5555

.segment "VECTORS"
    .word $0000,$0000,$0000
    .word stub
    .word $0000
    .word stub
    .word $0000,$0000
    .word $0000,$0000,$0000
    .word stub
    .word reset_handler
    .word stub
