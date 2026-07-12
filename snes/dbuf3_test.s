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

; tilemap entries = tile index | palette (bit10 = $0400 = palette 1), chosen to
; hit indices 0/256/512 so the shared tilemap drives BOTH BG1 and BG3.
PAL1        = $0400
IDX_RED     = 0   | PAL1   ; mid_A  base_A idx 0
IDX_GREEN   = 256 | PAL1   ; bot_A  base_A idx 256
IDX_WHITE_A = 512 | PAL1   ; top    base_A idx 512
IDX_WHITE_B = 0   | PAL1   ; top    base_B idx 0
IDX_BLUE    = 256 | PAL1   ; mid_B  base_B idx 256
IDX_YELLOW  = 512 | PAL1   ; bot_B  base_B idx 512

; BG12NBA/BG34NBA (CHR base, 0x1000w units); BG1SC/BG3SC (tilemap base, 0x400w).
NBA_A       = $00          ; BG1 base_A  = 0x0000
NBA_B       = $02          ; BG1 base_B  = 0x2000
NBA3_A      = $05          ; BG3 base3_A = 0x5000
NBA3_B      = $06          ; BG3 base3_B = 0x6000
SC_A        = $78          ; tilemap_A @ 0x7800 (shared by BG1+BG3)
SC_B        = $7C          ; tilemap_B @ 0x7C00

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

    jsr clean_slate         ; zero the whole PPU control bank (power-on garbage
                            ; here intermittently blanks the screen once colour
                            ; math is on — memory: ppu-baseline-registers)

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

    ; --- CGRAM (palette 1, shared-tilemap 60-colour trick) ---
    ; BG3 2bpp palette 1 -> CGRAM 4..7  = brightness ramp (4=none/black for
    ;   half-add, 5..7 = increasing grey added to the hue).
    ; BG1 4bpp palette 1 -> CGRAM 16..31; we use 17..21 = red,green,white,blue,
    ;   yellow hues.  4..7 and 16..31 do NOT overlap -> both layers coexist.
    ; clear ALL 256 CGRAM entries to black first (backdrop @0 + any unset entry
    ; the half-add subscreen could read = uninitialised garbage otherwise).
    stz CGADD
    ldx #256
@cgclr:
    stz CGDATA
    stz CGDATA
    dex
    bne @cgclr

    lda #4
    sta CGADD               ; start at CGRAM 4 (BG3 palette-1 entry 0)
    stz CGDATA              ; 4 = brightness 0 = black $0000
    stz CGDATA
    lda #$4A                ; 5 = dark  grey $294A
    sta CGDATA
    lda #$29
    sta CGDATA
    lda #$94                ; 6 = mid   grey $5294
    sta CGDATA
    lda #$52
    sta CGDATA
    lda #$DE                ; 7 = light grey $6BDE
    sta CGDATA
    lda #$6B
    sta CGDATA
    ; --- BG1 hues at CGRAM 16..21 (palette 1 entries 0..5) ---
    lda #16
    sta CGADD
    stz CGDATA              ; 16 = hue 0 (unused, backdrop) $0000
    stz CGDATA
    lda #$1F                ; 17 red    $001F
    sta CGDATA
    stz CGDATA
    lda #$E0                ; 18 green   $03E0
    sta CGDATA
    lda #$03
    sta CGDATA
    lda #$FF                ; 19 white   $7FFF
    sta CGDATA
    lda #$7F
    sta CGDATA
    lda #$00                ; 20 blue    $7C00
    sta CGDATA
    lda #$7C
    sta CGDATA
    lda #$FF                ; 21 yellow  $03FF
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
    ; --- BG3 2bpp brightness tiles (8 words each) at the BG3 bases; SAME indices
    ; as BG1 (0/256/512) so the shared tilemap drives both. top=3, mid=2, bot=1. ---
    lda #$FFFF             ; top3  @0x6000 bright3 (SHARED)  bp0+bp1
    sta $04
    ldx #$6000
    jsr write_solid_tile2
    lda #$FF00             ; mid3_A @0x5000 bright2  bp1
    sta $04
    ldx #$5000
    jsr write_solid_tile2
    lda #$FF00             ; mid3_B @0x6800 bright2
    sta $04
    ldx #$6800
    jsr write_solid_tile2
    lda #$00FF             ; bot3_A @0x5800 bright1  bp0
    sta $04
    ldx #$5800
    jsr write_solid_tile2
    lda #$00FF             ; bot3_B @0x7000 bright1
    sta $04
    ldx #$7000
    jsr write_solid_tile2
    sep #$20
    .a8

    ; --- build tilemap_A @ 0x7800 and tilemap_B @ 0x7C00 ---
    jsr build_tilemaps

    ; --- show frame A + BG3 sub layer + half-add colour math (60-colour) ---
    lda #NBA_A
    sta BG12NBA
    lda #NBA3_A
    sta BG34NBA
    lda #SC_A
    sta BG1SC
    sta BG3SC              ; BG3 SHARES the same tilemap (palette 1)
    lda #$04
    sta TS                 ; BG3 on subscreen
    lda #$02
    sta CGWSEL             ; add subscreen
    lda #$41
    sta CGADSUB            ; half + add, BG1 affected (60-colour composite)
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

; Zero the entire PPU control-register bank to a known baseline (mirrors the
; kernel's clean_slate). Called under force-blank at reset; eliminates power-on
; garbage that intermittently blanks the screen once colour math is on. A8/I16.
.proc clean_slate
    .a8
    .i16
    lda #$80
    sta $2100               ; INIDISP force-blank
    stz $2101               ; OBSEL
    stz $2102               ; OAMADDL
    stz $2103               ; OAMADDH
    stz $2105               ; BGMODE
    stz $2106               ; MOSAIC
    stz $2107               ; BG1SC
    stz $2108               ; BG2SC
    stz $2109               ; BG3SC
    stz $210A               ; BG4SC
    stz $210B               ; BG12NBA
    stz $210C               ; BG34NBA
    stz $210D               ; scrolls $210D..$2114 (each write-twice)
    stz $210D
    stz $210E
    stz $210E
    stz $210F
    stz $210F
    stz $2110
    stz $2110
    stz $2111
    stz $2111
    stz $2112
    stz $2112
    stz $2113
    stz $2113
    stz $2114
    stz $2114
    stz $2115               ; VMAIN
    stz $211A               ; M7SEL
    stz $211B               ; M7A..M7Y $211B..$2120 (each write-twice)
    stz $211B
    stz $211C
    stz $211C
    stz $211D
    stz $211D
    stz $211E
    stz $211E
    stz $211F
    stz $211F
    stz $2120
    stz $2120
    stz $2121               ; CGADD
    stz $2123               ; W12SEL
    stz $2124               ; W34SEL
    stz $2125               ; WOBJSEL
    stz $2126               ; WH0
    stz $2127               ; WH1
    stz $2128               ; WH2
    stz $2129               ; WH3
    stz $212A               ; WBGLOG
    stz $212B               ; WOBJLOG
    stz $212C               ; TM
    stz $212D               ; TS
    stz $212E               ; TMW
    stz $212F               ; TSW
    stz $2130               ; CGWSEL
    stz $2131               ; CGADSUB
    lda #$E0
    sta $2132               ; COLDATA = black fixed colour
    stz $2133               ; SETINI
    rts
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

; Write a solid 2bpp tile at VRAM word X: 8 words of plane01 ($04). A16/I16.
.proc write_solid_tile2
    .a16
    .i16
    stx VMADDL
    ldy #8
@p:
    lda $04
    sta VMDATAL
    dey
    bne @p
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
    ; -> show B : the 4-register reveal (BG12NBA+BG34NBA+BG1SC+BG3SC), in vblank
    lda #NBA_B
    sta BG12NBA
    lda #NBA3_B
    sta BG34NBA
    lda #SC_B
    sta BG1SC
    sta BG3SC
    lda #$01
    sta frame_par
    bra @done
@to_a:
    lda #NBA_A
    sta BG12NBA
    lda #NBA3_A
    sta BG34NBA
    lda #SC_A
    sta BG1SC
    sta BG3SC
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
