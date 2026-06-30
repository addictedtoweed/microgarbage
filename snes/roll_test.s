; ============================================================
;  roll_test.s — standalone rolling-buffer flip test (240x200 dual-layer).
;
;  STAGE 2: the 4-band rolling update + flip. The 240x200 image is 25 tile-rows,
;  one unique tile per row, split into 4 bands (top->bottom: tiles 0-5,6-11,
;  12-17,18-24 = 6/6/6/7 rows). Two baked frames A/B (shifted spectrum). Each
;  vblank writes ONE band's CHR (both layers) from the frame currently rolling
;  in; the order is bottom-3-first then TOP-LAST (hw-frames 0,1,2 -> bands 1,2,3;
;  hw-frame 3 -> band 0). After the top band lands the frame is complete and the
;  roll target flips A<->B. Watch the top edge: it always changes LAST.
;
;  (Solid tiles => tiny bands, so this validates the write ORDER/mechanism;
;  the DMA-window bandwidth is covered separately by dma_rate_test.)
;
;  Build: ca65 --cpu 65816 -o build/roll_test.o roll_test.s
;         ld65 -C cube3d.cfg -o build/roll_test.sfc build/roll_test.o ; perl fixsum.pl ... 0x7FC0
;  Public domain (CC0).
; ============================================================
.p816
.smart +
.include "snes.inc"

BG1_CHR  = $0000
BG3_CHR  = $2000       ; char base unit 2
BG1_MAP  = $1000
BG3_MAP  = $2800

; rolling state (DP)
FC    = $00            ; hw-frame counter 0..3 within a displayed frame
TGT   = $01            ; frame currently rolling in (0=A, 1=B)
SLOW  = $02            ; hw-frame divider so the wipe is watchable
TMPW  = $04            ; 16-bit scratch

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

    ; mode 1, dual layer + colour math (same as the cube)
    lda #$01
    sta BGMODE
    stz BG12NBA
    lda #$02
    sta BG34NBA
    lda #$10
    sta BG1SC
    lda #$28
    sta BG3SC
    stz BG1HOFS
    stz BG1HOFS
    stz BG3HOFS
    stz BG3HOFS
    ; scroll image down 4px (BGVOFS = -4 = $3FC) so it centres in the 12/12 letterbox
    lda #$FC
    sta BG1VOFS
    lda #$03
    sta BG1VOFS
    lda #$FC
    sta BG3VOFS
    lda #$03
    sta BG3VOFS
    lda #$01
    sta TM
    lda #$04
    sta TS
    lda #$02
    sta CGWSEL
    lda #$41
    sta CGADSUB

    ; palette
    stz CGADD
    stz DMAP0
    lda #<CGDATA
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(palette_data)
    sta A1T0L
    lda #(20*2)
    sta DAS0L
    sep #$20
    .a8
    lda #^palette_data
    sta A1B0
    lda #$01
    sta MDMAEN

    ; tilemaps (static): image rows 1..25, cols 1..30 -> tile (row-1)
    rep #$20
    .a16
    lda #BG1_MAP
    sta VMADDL
    lda #.loword(bg1_map)
    sta A1T0L
    lda #2048
    sta DAS0L
    sep #$20
    .a8
    lda #^bg1_map
    sta A1B0
    lda #$80
    sta VMAIN
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #$01
    sta MDMAEN
    rep #$20
    .a16
    lda #BG3_MAP
    sta VMADDL
    lda #.loword(bg3_map)
    sta A1T0L
    lda #2048
    sta DAS0L
    sep #$20
    .a8
    lda #^bg3_map
    sta A1B0
    lda #$01
    sta MDMAEN

    ; --- force-blank letterbox via HDMA on INIDISP (ch 7): top 8 + bottom 16
    ;     force-blanked -> ~62-line DMA window (vblank 38 + 16 + 8). ---
    stz DMAP7               ; A->B, 1 reg (INIDISP), absolute
    stz BBAD7               ; B-bus $2100
    rep #$20
    .a16
    lda #.loword(letterbox_hdma)
    sta A1T7L
    sep #$20
    .a8
    lda #^letterbox_hdma
    sta $4374               ; A1B7 (HDMA table bank)
    lda #$80
    sta HDMAEN              ; enable channel 7 HDMA

    ; --- start displaying frame A: write all 4 bands of A ---
    stz TGT                 ; rolling in A
    ldx #0
@initband:
    txa
    jsr write_band          ; band X of frame TGT (=A)
    inx
    cpx #4
    bne @initband

    stz FC
    lda #1
    sta TGT                 ; now roll in B next
    stz SLOW

    lda #$0F
    sta INIDISP
    stz NMITIMEN

main_loop:
    jsr wait_vblank
    ; slow the wipe down to ~15 displayed-fps feel: advance one band every ~8 hw frames
    lda SLOW
    inc a
    and #$07
    sta SLOW
    bne main_loop
    ; one rolling step: write the band for this FC of frame TGT
    ; band order: FC 0,1,2 -> bands 1,2,3 (bottom 3); FC 3 -> band 0 (top, last)
    lda FC
    cmp #3
    bne @notop
    lda #0                  ; top band last
    bra @doband
@notop:
    clc
    adc #1                  ; FC 0/1/2 -> band 1/2/3
@doband:
    jsr write_band
    ; advance FC; on wrap, flip roll target
    lda FC
    inc a
    cmp #4
    bne @noflip
    lda #0
    sta FC
    lda TGT
    eor #$01
    sta TGT
    bra main_loop
@noflip:
    sta FC
    bra main_loop
.endproc

; ---- write_band: DMA band (A reg, 0..3) of frame TGT into both layers.
;      band tile ranges: 0:0-5, 1:6-11, 2:12-17, 3:18-24 ----
.proc write_band
    php
    rep #$30
    .a16
    .i16
    phx                     ; preserve caller's X/Y (we use them as indices)
    phy
    and #$00FF
    asl a
    tax                     ; X = band*2 (word index into band tables)
    lda TGT
    and #$00FF
    asl a
    tay                     ; Y = TGT*2 (word index into frame-base tables)

    ; --- BG1 4bpp band ---
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda band_start,x        ; tile_start
    asl a
    asl a
    asl a
    asl a                   ; *16 words
    clc
    adc #BG1_CHR
    sta VMADDL
    lda band_start,x
    asl a
    asl a
    asl a
    asl a
    asl a                   ; tile_start*32 bytes
    clc
    adc fa1tab,y            ; + frameTGT bg1 base
    sta A1T0L
    lda band_bytes1,x
    sta DAS0L
    sep #$20
    .a8
    stz A1B0                ; ROM bank 0
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; --- BG3 2bpp band ---
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda band_start,x
    asl a
    asl a
    asl a                   ; *8 words
    clc
    adc #BG3_CHR
    sta VMADDL
    lda band_start,x
    asl a
    asl a
    asl a
    asl a                   ; tile_start*16 bytes
    clc
    adc fa3tab,y            ; + frameTGT bg3 base
    sta A1T0L
    lda band_bytes3,x
    sta DAS0L
    sep #$20
    .a8
    stz A1B0
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #$01
    sta MDMAEN
    rep #$30
    .a16
    .i16
    ply
    plx
    plp
    rts
.endproc

; ------------------------------------------------------------------
palette_data:
    .word $3000, $001F, $021F, $03FF, $0360, $7D40, $7C14, $0011
    .word $0111, $0231, $01E0, $44A0, $440B, $7FFF, $4A52, $2108
    .word $0000, $6739, $35AD, $14A5

band_start:  .word 0, 6, 12, 18
band_bytes1: .word 6*32, 6*32, 6*32, 7*32
band_bytes3: .word 6*16, 6*16, 6*16, 7*16
fa1tab:  .word .loword(frameA_bg1), .loword(frameB_bg1)
fa3tab:  .word .loword(frameA_bg3), .loword(frameB_bg3)

; frame A/B solid-tile CHR (25 tiles each), one per image row.
frameA_bg1:
.repeat 25, R
  .repeat 8
    .byte ((R & 15) & 1)*$FF, (((R & 15) >> 1) & 1)*$FF
  .endrepeat
  .repeat 8
    .byte (((R & 15) >> 2) & 1)*$FF, (((R & 15) >> 3) & 1)*$FF
  .endrepeat
.endrepeat
frameB_bg1:
.repeat 25, R
  .repeat 8
    .byte (((R+8) & 15) & 1)*$FF, ((((R+8) & 15) >> 1) & 1)*$FF
  .endrepeat
  .repeat 8
    .byte ((((R+8) & 15) >> 2) & 1)*$FF, ((((R+8) & 15) >> 3) & 1)*$FF
  .endrepeat
.endrepeat
frameA_bg3:
.repeat 25, R
  .repeat 8
    .byte ((R & 3) & 1)*$FF, (((R & 3) >> 1) & 1)*$FF
  .endrepeat
.endrepeat
frameB_bg3:
.repeat 25, R
  .repeat 8
    .byte (((R+1) & 3) & 1)*$FF, ((((R+1) & 3) >> 1) & 1)*$FF
  .endrepeat
.endrepeat

bg1_map:
.repeat 32, ROW
  .repeat 32, COL
    .if (ROW >= 1) && (ROW < 26) && (COL >= 1) && (COL < 31)
      .word (ROW-1)
    .else
      .word 24
    .endif
  .endrepeat
.endrepeat
bg3_map:
.repeat 32, ROW
  .repeat 32, COL
    .if (ROW >= 1) && (ROW < 26) && (COL >= 1) && (COL < 31)
      .word (ROW-1) + $1000
    .else
      .word 24 + $1000
    .endif
  .endrepeat
.endrepeat

; INIDISP per-scanline HDMA: 8 force-blank, 200 visible, 16 force-blank (224).
; Per-scanline mode ($80|N + N bytes) so it works on bsnes-plus too, not just
; ares/hardware (bsnes-plus advances HDMA source per line even in repeat mode).
letterbox_hdma:
    .byte $FF                    ; next 127 scanlines (0..126)
    .repeat 12
      .byte $80                  ; top letterbox (force-blank)
    .endrepeat
    .repeat 115
      .byte $0F                  ; visible
    .endrepeat
    .byte $E1                    ; next 97 scanlines (127..223)
    .repeat 85
      .byte $0F                  ; visible (115+85 = 200)
    .endrepeat
    .repeat 12
      .byte $80                  ; bottom letterbox (12 top / 12 bottom = centered)
    .endrepeat
    .byte $00                    ; terminator

.proc wait_vblank
    php
    sep #$20
    .a8
@a:
    lda HVBJOY
    bmi @a
@v:
    lda HVBJOY
    bpl @v
    plp
    rts
.endproc

.proc stub
    rti
.endproc

.segment "HEADER"
    .byte "ROLL TEST            "
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
