; ============================================================
;  dbuf3_test.s — prove the OVERLAPPING-BASE thirds double-buffer + atomic
;  4-register reveal for the tear-free 240x200 renderer (docs/emitter-kernel.md).
;
;  BG1-only proof (BG3 + palette-4 sharing added once this is confirmed). Two
;  frames share the TOP third; each frame's 3 thirds are reachable from its own
;  BG12NBA base at indices 0/256/512, with the shared top living in the overlap.
;
;  VRAM (words): red 0x0000 | green 0x1000 | WHITE(shared) 0x2000 | blue 0x3000
;                yellow 0x4000 ; tilemap_A 0x7800 ; tilemap_B 0x7C00
;      base_A = 0x0000 (BG12NBA=0)   base_B = 0x2000 (BG12NBA=2)
;      tilemap_A idx: top->512(white) mid->0(red)  bot->256(green)
;      tilemap_B idx: top->0(white)   mid->256(blue) bot->512(yellow)
;
;  EXPECT on ares: screen alternates ~every 40 frames. mid band red<->blue and
;  bot band green<->yellow FLIP, but the TOP band stays WHITE across both frames
;  (it is the one shared physical tile) -> proves the overlap + the atomic flip.
;  Any tear (top changes, or a partial mid/bot) = the reveal isn't atomic.
;
;  Build: ca65 --cpu 65816 -o build/dbuf3_test.o dbuf3_test.s
;         ld65 -C cube3d.cfg -o build/dbuf3_test.sfc build/dbuf3_test.o
;         perl fixsum.pl build/dbuf3_test.sfc 0x7FC0
;
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"

; --- geometry: 240x200 = 30x25 tiles, thirds of ~8/8/9 rows ---
TW          = 30
TOP_ROWS    = 8            ; rows 0..7   -> top third (shared)
MID_ROWS    = 8            ; rows 8..15  -> mid third
BOT_ROWS    = 9            ; rows 16..24 -> bot third

; solid-colour tile indices within each base (chosen to hit 0/256/512)
IDX_RED     = 0            ; mid_A  @ 0x0000, base_A idx 0
IDX_GREEN   = 256          ; bot_A  @ 0x1000, base_A idx 256
IDX_WHITE_A = 512          ; top    @ 0x2000, base_A idx 512
IDX_WHITE_B = 0            ; top    @ 0x2000, base_B idx 0
IDX_BLUE    = 256          ; mid_B  @ 0x3000, base_B idx 256
IDX_YELLOW  = 512          ; bot_B  @ 0x4000, base_B idx 512

; BG12NBA (CHR base, 0x1000-word units) and BG1SC (tilemap base, 0x400-word units)
NBA_A       = $00          ; base_A = 0x0000
NBA_B       = $02          ; base_B = 0x2000
SC_A        = $78          ; tilemap_A @ 0x7800 -> (0x7800/0x400)<<2 = 0x78
SC_B        = $7C          ; tilemap_B @ 0x7C00 -> 0x7C

; --- direct page ---
frame_par   = $00          ; 0 = showing A, 1 = showing B
flip_ctr    = $01          ; frame countdown to next flip

.segment "CODE"

.proc reset_handler
    sei
    clc
    xce                     ; native mode
    rep #$30
    .a16
    .i16
    ldx #$1FFF
    txs
    sep #$20
    .a8
    lda #$8F
    sta INIDISP             ; force-blank during setup
    stz NMITIMEN

    ; --- clear all VRAM to 0 ---
    stz VMAIN
    rep #$20
    .a16
    stz VMADDL
    sep #$20
    .a8
    lda #$80
    sta VMAIN               ; word increment after high byte
    rep #$20
    .a16
    ldx #$0000
@clr:
    stz VMDATAL
    inx
    cpx #$8000              ; 32768 words
    bne @clr
    sep #$20
    .a8

    ; --- BG mode 1, BG1 on main screen only (OBJ off) ---
    lda #$01
    sta BGMODE
    lda #$01
    sta TM                  ; BG1 main only
    ; scroll 0
    stz BG1HOFS
    stz BG1HOFS
    stz BG1VOFS
    stz BG1VOFS

    ; --- CGRAM: palette 0 colours 1..5 = red,green,white,blue,yellow (BGR555) ---
    stz CGADD               ; start at colour 0
    lda #$00                ; colour 0 = black (lo)
    sta CGDATA
    stz CGDATA              ; (hi)
    ; colour 1 red   = $001F
    lda #$1F
    sta CGDATA
    stz CGDATA
    ; colour 2 green = $03E0
    lda #$E0
    sta CGDATA
    lda #$03
    sta CGDATA
    ; colour 3 white = $7FFF
    lda #$FF
    sta CGDATA
    lda #$7F
    sta CGDATA
    ; colour 4 blue  = $7C00
    lda #$00
    sta CGDATA
    lda #$7C
    sta CGDATA
    ; colour 5 yellow= $03FF
    lda #$FF
    sta CGDATA
    lda #$03
    sta CGDATA

    ; --- write the 5 solid tiles (4bpp, 16 words each) at their base offsets ---
    ; plane01 word = (bp1<<8)|bp0 ; plane23 word = (bp3<<8)|bp2 ; solid -> $FF/$00.
    ; red(idx1)=bp0 ; green(idx2)=bp1 ; white(idx3)=bp0+bp1 ; blue(idx4)=bp2 ;
    ; yellow(idx5)=bp0+bp2.  ($04=plane01, $06=plane23, X=VRAM word)
    rep #$20
    .a16
    lda #$00FF              ; red   @0x0000  (bp0)
    sta $04
    stz $06
    ldx #$0000
    jsr write_solid_tile
    lda #$FF00             ; green @0x1000  (bp1)
    sta $04
    stz $06
    ldx #$1000
    jsr write_solid_tile
    lda #$FFFF             ; white @0x2000  (bp0+bp1)  SHARED
    sta $04
    stz $06
    ldx #$2000
    jsr write_solid_tile
    stz $04                ; blue  @0x3000  (bp2)
    lda #$00FF
    sta $06
    ldx #$3000
    jsr write_solid_tile
    lda #$00FF             ; yellow@0x4000  (bp0+bp2)
    sta $04
    lda #$00FF
    sta $06
    ldx #$4000
    jsr write_solid_tile
    sep #$20
    .a8

    ; --- build tilemap_A @ 0x7800 and tilemap_B @ 0x7C00 ---
    jsr build_tilemaps

    ; --- show frame A ---
    lda #NBA_A
    sta BG12NBA
    lda #SC_A
    sta BG1SC
    stz frame_par
    lda #40
    sta flip_ctr

    ; --- arm NMI (vblank) for the flip cadence ---
    lda #$0F
    sta INIDISP             ; unblank
    lda #$80
    sta NMITIMEN            ; NMI on, auto-joypad off
    cli
@idle:
    wai
    bra @idle
.endproc

; Write a solid 4bpp tile at VRAM word X: 8 words of plane01 ($04) then 8 words of
; plane23 ($06). Called in A16/I16 (rep #$20). VMAIN already $80. Clobbers A,Y.
.proc write_solid_tile
    .a16
    .i16
    stx VMADDL
    ldy #8
@p1:
    lda $04
    sta VMDATAL             ; 16-bit -> VMDATAL/H, word increment
    dey
    bne @p1
    ldy #8
@p2:
    lda $06
    sta VMDATAL
    dey
    bne @p2
    rts
.endproc

; Build tilemap_A (0x7800) and tilemap_B (0x7C00). 32x32 words each; only the
; 30x25 visible cells matter. Top 8 rows -> top third, next 8 -> mid, last 9 -> bot.
.proc build_tilemaps
    ; ---- tilemap_A @ 0x7800 ----
    rep #$20
    .a16
    lda #$7800
    sta VMADDL
    sep #$20
    .a8
    ; rows 0..24, 32 cols each (only 30 visible; fill 32 to keep VMADD aligned)
    ldy #0                  ; row
@a_row:
    ; pick tile index for this row's third
    rep #$20
    .a16
    cpy #TOP_ROWS
    bcc @a_top
    cpy #(TOP_ROWS+MID_ROWS)
    bcc @a_mid
    lda #IDX_GREEN          ; bot third
    bra @a_have
@a_top:
    lda #IDX_WHITE_A
    bra @a_have
@a_mid:
    lda #IDX_RED
@a_have:
    ; A = tile index (palette 0, no flip/priority)
    ldx #32
@a_col:
    sta VMDATAL
    dex
    bne @a_col
    sep #$20
    .a8
    iny
    cpy #25
    bne @a_row
    ; pad remaining rows 25..31 with 0 (7 rows x 32 = 224 words)
    rep #$20
    .a16
    ldx #(7*32)
    lda #$0000
@a_pad:
    sta VMDATAL
    dex
    bne @a_pad
    sep #$20
    .a8

    ; ---- tilemap_B @ 0x7C00 ----
    rep #$20
    .a16
    lda #$7C00
    sta VMADDL
    sep #$20
    .a8
    ldy #0
@b_row:
    rep #$20
    .a16
    cpy #TOP_ROWS
    bcc @b_top
    cpy #(TOP_ROWS+MID_ROWS)
    bcc @b_mid
    lda #IDX_YELLOW         ; bot third
    bra @b_have
@b_top:
    lda #IDX_WHITE_B
    bra @b_have
@b_mid:
    lda #IDX_BLUE
@b_have:
    ldx #32
@b_col:
    sta VMDATAL
    dex
    bne @b_col
    sep #$20
    .a8
    iny
    cpy #25
    bne @b_row
    rep #$20
    .a16
    ldx #(7*32)
    lda #$0000
@b_pad:
    sta VMDATAL
    dex
    bne @b_pad
    sep #$20
    .a8
    rts
.endproc

; NMI: count down; on 0, flip to the other frame (BG12NBA + BG1SC = the 2-reg
; reveal for BG1; full scheme adds BG34NBA + BG3SC). All during vblank -> atomic.
.proc nmi_handler
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8
    lda RDNMI               ; ack
    dec flip_ctr
    bne @done
    lda #40
    sta flip_ctr
    lda frame_par
    bne @to_a
    ; -> show B
    lda #NBA_B
    sta BG12NBA
    lda #SC_B
    sta BG1SC
    lda #$01
    sta frame_par
    bra @done
@to_a:
    lda #NBA_A
    sta BG12NBA
    lda #SC_A
    sta BG1SC
    stz frame_par
@done:
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

.proc stub
    rti
.endproc

; ------------------------------------------------------------------
;  Header + vectors (LoROM, matches cube3d.cfg)
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "DBUF3 THIRDS TEST    "   ; 21 chars title

.segment "VECTORS"
    ; native
    .word stub            ; COP
    .word stub            ; BRK
    .word stub            ; ABORT
    .word nmi_handler     ; NMI
    .word stub            ; reserved
    .word stub            ; IRQ
    ; emulation
    .word stub            ; COP
    .word stub            ; reserved
    .word stub            ; ABORT
    .word stub            ; NMI
    .word reset_handler   ; RESET
    .word stub            ; IRQ/BRK
