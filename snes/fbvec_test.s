; ============================================================
;  fbvec_test.s — VECTOR-SWAP framebuffer kernel proof (240x200 dual-layer).
;
;  Proves the emitter/dead-simple-kernel IRQ core in isolation (no coprocessor):
;  a V-counter IRQ whose handler POINTER (IRQVEC) is swapped between two tiny
;  self-chaining routines — finish_irq (bottom-bar line: force-blank + DMA one
;  band) and start_irq (top-bar line: unblank + advance frame). One band per
;  displayed frame, bottom-3-first / TOP-LAST, A/B frame flip on wrap. This is
;  the roll_test image + write_band, but driven by the vector-swap IRQ instead
;  of HDMA-INIDISP bars + a main-loop — the mechanism the copro kernel will use.
;
;  Letterbox: top 8 + bottom 16 lines force-blanked (visible = lines 8..207 =
;  the 200px frame). finish_irq at line 208, start_irq at line 8.
;
;  Build: ca65 --cpu 65816 -o build/fbvec_test.o fbvec_test.s
;         ld65 -C cube3d.cfg -o build/fbvec_test.sfc build/fbvec_test.o
;         perl fixsum.pl build/fbvec_test.sfc 0x7FC0
;  Public domain (CC0).
; ============================================================
.p816
.smart +
.include "snes.inc"

BG1_CHR  = $0000
BG3_CHR  = $2000       ; char base unit 2
BG1_MAP  = $1000
BG3_MAP  = $2800

FIN_LINE   = 208       ; bottom-bar line: force-blank + band DMA
START_LINE = 8         ; top-of-visible: unblank

; direct-page state
FC     = $00           ; band-within-frame counter 0..3
TGT    = $01           ; frame currently rolling in (0=A, 1=B)
IRQVEC = $10           ; 2-byte pointer the IRQ trampoline jmp ()s through
TMPW   = $12

.segment "CODE"

.proc reset_handler
    sei
    clc
    xce                     ; native mode
    rep #$38
    .a16
    .i16
    ldx #$1FFF
    txs
    lda #$0000
    tcd                     ; DP = 0

    sep #$20
    .a8
    lda #$8F
    sta INIDISP             ; force blank during setup

    ; --- Mode 1: BG1 4bpp main, BG3 2bpp sub, half-add colour math ---
    lda #$01
    sta BGMODE
    stz BG1SC               ; BG1 map @ BG1_MAP/$400 = 4 -> 4<<2 ... set below
    lda #(BG1_MAP / $400) << 2
    sta BG1SC
    lda #(BG3_MAP / $400) << 2
    sta BG3SC
    lda #(BG1_CHR / $1000)  ; BG1 char base (BG12NBA low nibble)
    sta BG12NBA
    lda #(BG3_CHR / $1000)  ; BG3 char base (BG34NBA low nibble)
    sta BG34NBA
    stz BG1HOFS
    stz BG1HOFS
    stz BG1VOFS
    stz BG1VOFS
    stz BG3HOFS
    stz BG3HOFS
    stz BG3VOFS
    stz BG3VOFS
    lda #$01
    sta TM                  ; BG1 on main
    lda #$04
    sta TS                  ; BG3 on sub
    lda #$02
    sta CGWSEL              ; colour math always, use sub screen
    lda #$41
    sta CGADSUB             ; BG1 participates; half; add
    stz HDMAEN
    stz SETINI

    ; --- palette (20 entries) ---
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

    ; --- BG1 tilemap ---
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
    ; --- BG3 tilemap ---
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

    ; --- prime: write all 4 bands of frame A so the first frame is complete ---
    stz TGT
    ldx #0
@initband:
    txa
    jsr write_band
    inx
    cpx #4
    bne @initband
    stz FC
    lda #1
    sta TGT                 ; roll in B next

    ; --- arm the vector-swap V-IRQ: first event = finish (bottom bar) ---
    rep #$20
    .a16
    lda #.loword(finish_irq)
    sta IRQVEC
    sep #$20
    .a8
    lda #FIN_LINE
    sta VTIMEL
    stz VTIMEH
    lda #$20                ; NMITIMEN: V-count IRQ (b5), NMI off, auto-joy off
    sta NMITIMEN
    lda #$0F
    sta INIDISP            ; unblank; the IRQ bars take over
    cli
@loop:
    wai
    bra @loop
.endproc

; ------------------------------------------------------------------
; finish_irq — bottom-bar line (208): force-blank, DMA this frame's next band,
; advance the band counter, point IRQVEC at start_irq, arm V=START_LINE.
; ------------------------------------------------------------------
.proc finish_irq
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8
    lda TIMEUP              ; ack IRQ
    lda #$80
    sta INIDISP            ; force-blank (opens the DMA window)

    ; band = band_order[FC]  (bottom-3-first, TOP-LAST)
    rep #$20
    .a16
    lda FC
    and #$00FF
    tax
    sep #$20
    .a8
    lda band_order,x
    jsr write_band         ; A = band index

    inc FC
    lda FC
    cmp #4
    bne :+
    stz FC                 ; wrap handled at start_irq (frame flip)
:
    rep #$20
    .a16
    lda #.loword(start_irq)
    sta IRQVEC
    sep #$20
    .a8
    lda #START_LINE
    sta VTIMEL
    stz VTIMEH
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

; ------------------------------------------------------------------
; start_irq — top-of-visible line (8): unblank; on band-cycle wrap (FC just
; became 0) flip the roll target A<->B; point IRQVEC at finish_irq, arm
; V=FIN_LINE.
; ------------------------------------------------------------------
.proc start_irq
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8
    lda TIMEUP
    lda #$0F
    sta INIDISP            ; unblank visible region

    lda FC
    bne @noflip            ; FC==0 => a full 4-band cycle just completed
    lda TGT
    eor #$01
    sta TGT
@noflip:
    rep #$20
    .a16
    lda #.loword(finish_irq)
    sta IRQVEC
    sep #$20
    .a8
    lda #FIN_LINE
    sta VTIMEL
    stz VTIMEH
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

; ------------------------------------------------------------------
; irq_trampoline — the ROM IRQ vector points here; jmp () through IRQVEC.
; This 3-byte indirect is the whole "vector swap": handlers just rewrite IRQVEC.
; ------------------------------------------------------------------
.proc irq_trampoline
    jmp (IRQVEC)
.endproc

; ------------------------------------------------------------------
; write_band — DMA band (A reg, 0..3) of frame TGT into both layers.
;   band tile ranges: 0:0-5, 1:6-11, 2:12-17, 3:18-24  (ported from roll_test)
; ------------------------------------------------------------------
.proc write_band
    php
    rep #$30
    .a16
    .i16
    phx
    phy
    and #$00FF
    asl a
    tax                     ; X = band*2
    lda TGT
    and #$00FF
    asl a
    tay                     ; Y = TGT*2

    ; --- BG1 4bpp band ---
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda band_start,x
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
    adc fa1tab,y
    sta A1T0L
    lda band_bytes1,x
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
    adc fa3tab,y
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

band_order:  .byte 1, 2, 3, 0           ; bottom-3-first, TOP-LAST
band_start:  .word 0, 6, 12, 18
band_bytes1: .word 6*32, 6*32, 6*32, 7*32
band_bytes3: .word 6*16, 6*16, 6*16, 7*16
fa1tab:  .word .loword(frameA_bg1), .loword(frameB_bg1)
fa3tab:  .word .loword(frameA_bg3), .loword(frameB_bg3)

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

; ------------------------------------------------------------------
.proc stub
    rti
.endproc

.segment "HEADER"
    .byte "FBVEC TEST           "
    .byte $20
    .byte $00, $05, $00, $01, $00, $00
    .word $AAAA, $5555

.segment "VECTORS"
    .word $0000,$0000,$0000  ; COP, BRK, ABORT
    .word stub               ; NMI
    .word $0000              ; reserved
    .word irq_trampoline     ; IRQ (native) — the vector-swap entry point
    .word $0000,$0000
    .word $0000,$0000,$0000
    .word stub               ; NMI (emu)
    .word reset_handler      ; RESET
    .word stub               ; IRQ (emu)
