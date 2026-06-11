; ============================================================
;  smoke.s — standalone "Select Demo" menu kernel (NOT the real kernel).
;
;  After boot copies us into low WRAM we draw a centered selector
;  screen with three options:
;
;        SELECT DEMO
;
;        > FMV DEMO
;          3D DEMO
;          MIXER DEMO
;
;  D-pad Up/Down moves the arrow (edge-triggered so a held button
;  only steps once). A / B / Start / Select are deliberately
;  UNUSABLE -- the demos aren't wired up yet; this is the
;  boot+display+input test ahead of that.
;
;  Exercises: WRAM execution, the ROM->RAM NMI vector indirection,
;  VRAM/CGRAM upload via DMA channel 0, auto-joypad read with edge
;  detection, in-vblank tilemap writes via VMADDL/VMDATAL. Runs as
;  a plain HiROM in stock bsnes; the custom mapper drops in later.
;
;  Build:  .\snes\build.ps1 -Smoke   (defines SMOKE_TEST for boot.s)
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"
.include "copro.inc"

; --- WRAM state (low RAM; separate from copro.inc's PADS at $0220) ---
ARROW_POS   = $0214             ; selected menu index (0..2)
NEED_REDRAW = $0215             ; 1 = arrow moved, NMI redraws on next vblank
JOY_LAST    = $0216             ; previous joypad word (16-bit at $0216..$0217)
JOY_EDGES   = $0218             ; freshly-pressed bits this frame (16-bit)
TEMP_JOY    = $021A             ; scratch for current joypad word (16-bit)

; --- tile indices in the bundled font ---
T_SPC = 0
T_A   = 1
T_C   = 2
T_D   = 3
T_E   = 4
T_F   = 5
T_I   = 6
T_L   = 7
T_M   = 8
T_O   = 9
T_R   = 10
T_S   = 11
T_T   = 12
T_V   = 13
T_X   = 14
T_3   = 15
T_ARR = 16

; --- joypad bits (JOY1 16-bit format, matches auto-read $4218/$4219) ---
JOY_UP = $0800
JOY_DN = $0400

; --- screen layout: 32-tile-wide field; menu centred ---
ROW_TITLE = 8
COL_TITLE = 10                  ; "SELECT DEMO" (11 chars) starts here
ROW_OPT0  = 12                  ; "FMV DEMO"
ROW_OPT1  = 14                  ; "3D DEMO"
ROW_OPT2  = 16                  ; "MIXER DEMO"
COL_ARROW = 9                   ; ">" column
COL_OPT   = 11                  ; option text column

.segment "KERNEL"

; entry MUST be first -- boot.s does `jmp __KERNEL_RUN__`.
.proc kmain
    .a8
    .i16

    ; install NMI handler so the ROM trampoline at $FFB0 bounces here
    rep #$20
    .a16
    lda #.loword(nmi)
    sta RAMVEC_NMI
    stz JOY_LAST
    sep #$20
    .a8

    stz ARROW_POS
    lda #1
    sta NEED_REDRAW

    ; --- PPU setup ----------------------------------------------------
    lda #$80
    sta INIDISP                 ; force blank during VRAM/CGRAM upload
    lda #$01
    sta BGMODE                  ; Mode 1 (BG1 4bpp + BG2 4bpp + BG3 2bpp)
    stz BG1SC                   ; BG1 tilemap @ VRAM word $0000, 32x32
    lda #$01
    sta BG12NBA                 ; BG1 CHR base = 1 (= word $1000)
    sta TM                      ; BG1 on main screen

    ; --- channel 0 source bank ($7E since our data lives in WRAM) -----
    lda #$7E
    sta A1B0

    ; --- 1) clear the BG1 tilemap region (1024 word entries) ---------
    lda #$80
    sta VMAIN                   ; word-step, inc after high byte
    rep #$20
    .a16
    stz VMADDL                  ; VRAM word addr = 0
    ldx #1024
@clr:
    stz VMDATAL                 ; 16-bit STZ -> $2118+$2119 = one word
    dex
    bne @clr
    sep #$20
    .a8

    ; --- 2) DMA font_data to VRAM CHR base ($1000 word = $2000 byte) -
    rep #$20
    .a16
    lda #$1000
    sta VMADDL
    sep #$20
    .a8
    lda #$01                    ; DMAP pattern 1 (lo/hi to VMDATAL/H)
    sta DMAP0
    lda #<VMDATAL               ; $18
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(font_data)
    sta A1T0L
    lda #FONT_SIZE
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN                  ; fire channel 0

    ; --- 3) DMA palette_data to CGRAM (color 0..1) -------------------
    stz CGADD
    stz DMAP0                   ; pattern 0 (1 byte -> 1 reg)
    lda #<CGDATA                ; $22
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(palette_data)
    sta A1T0L
    lda #PAL_SIZE
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    ; --- 4) Render the static menu + first arrow into BG1 tilemap ----
    jsr draw_static_menu
    jsr draw_arrow
    stz NEED_REDRAW

    ; --- 5) Unblank + enable NMI + auto-joypad -----------------------
    lda #$0F
    sta INIDISP                 ; brightness full, blank off
    lda #$81
    sta NMITIMEN                ; NMI on (b7) + auto-joypad (b0)

; --- main loop --------------------------------------------------------
@loop:
    wai                         ; sleep until NMI

    ; auto-joypad read finishes ~3 lines into vblank; busy-wait it out
@joybusy:
    lda HVBJOY
    and #$01
    bne @joybusy

    ; edges = (current XOR last) AND current  (= bits newly pressed)
    rep #$20
    .a16
    lda JOY1L                   ; 16-bit auto-read result
    sta TEMP_JOY
    eor JOY_LAST
    and TEMP_JOY
    sta JOY_EDGES
    lda TEMP_JOY
    sta JOY_LAST
    sep #$20
    .a8

    ; D-pad Up -> ARROW_POS--
    lda JOY_EDGES+1             ; high byte holds bits 8..15 incl. Up/Dn
    and #>JOY_UP                ; >JOY_UP = $08
    beq @no_up
    lda ARROW_POS
    beq @no_up                  ; already at top
    dec ARROW_POS
    lda #1
    sta NEED_REDRAW
@no_up:
    lda JOY_EDGES+1
    and #>JOY_DN                ; >JOY_DN = $04
    beq @no_dn
    lda ARROW_POS
    cmp #2
    beq @no_dn                  ; already at bottom
    inc ARROW_POS
    lda #1
    sta NEED_REDRAW
@no_dn:
    bra @loop
.endproc

; ------------------------------------------------------------------
; NMI: ack, redraw the arrow if it moved (the only per-vblank work).
; ------------------------------------------------------------------
.proc nmi
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8
    lda RDNMI                   ; ack

    lda NEED_REDRAW
    beq @out
    jsr draw_arrow
    stz NEED_REDRAW
@out:
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

; ------------------------------------------------------------------
; Draw the title + the three option labels into BG1's tilemap.
; Called once during init under force-blank; no need for vblank.
; ------------------------------------------------------------------
.proc draw_static_menu
    .a8
    .i16
    lda #$80
    sta VMAIN                   ; word-step (idempotent if init set it)

    ; --- title at row 8 col 10 ---
    rep #$20
    .a16
    lda #(ROW_TITLE * 32 + COL_TITLE)
    sta VMADDL
    sep #$20
    .a8
    ldx #0
@t:
    lda title_str,x
    sta VMDATAL                 ; tile-id low byte
    stz VMDATAH                 ; palette/flip high byte (palette 0, no flip)
    inx
    cpx #title_len
    bne @t

    ; --- opt 0 (FMV DEMO) at row 12 col 11 ---
    rep #$20
    .a16
    lda #(ROW_OPT0 * 32 + COL_OPT)
    sta VMADDL
    sep #$20
    .a8
    ldx #0
@o0:
    lda opt0_str,x
    sta VMDATAL
    stz VMDATAH
    inx
    cpx #opt0_len
    bne @o0

    ; --- opt 1 (3D DEMO) at row 14 col 11 ---
    rep #$20
    .a16
    lda #(ROW_OPT1 * 32 + COL_OPT)
    sta VMADDL
    sep #$20
    .a8
    ldx #0
@o1:
    lda opt1_str,x
    sta VMDATAL
    stz VMDATAH
    inx
    cpx #opt1_len
    bne @o1

    ; --- opt 2 (MIXER DEMO) at row 16 col 11 ---
    rep #$20
    .a16
    lda #(ROW_OPT2 * 32 + COL_OPT)
    sta VMADDL
    sep #$20
    .a8
    ldx #0
@o2:
    lda opt2_str,x
    sta VMDATAL
    stz VMDATAH
    inx
    cpx #opt2_len
    bne @o2

    rts
.endproc

; ------------------------------------------------------------------
; Clear the three arrow positions, place ">" at the selected one.
; Two-row spacing between options means the per-step word delta is 64.
; ------------------------------------------------------------------
.proc draw_arrow
    .a8
    .i16
    lda #$80
    sta VMAIN

    ; clear row 12 col 9
    rep #$20
    .a16
    lda #(ROW_OPT0 * 32 + COL_ARROW)
    sta VMADDL
    sep #$20
    .a8
    lda #T_SPC
    sta VMDATAL
    stz VMDATAH

    ; clear row 14 col 9
    rep #$20
    .a16
    lda #(ROW_OPT1 * 32 + COL_ARROW)
    sta VMADDL
    sep #$20
    .a8
    lda #T_SPC
    sta VMDATAL
    stz VMDATAH

    ; clear row 16 col 9
    rep #$20
    .a16
    lda #(ROW_OPT2 * 32 + COL_ARROW)
    sta VMADDL
    sep #$20
    .a8
    lda #T_SPC
    sta VMDATAL
    stz VMDATAH

    ; place ">" at (ROW_OPT0 + ARROW_POS*2) * 32 + COL_ARROW
    ;   = ROW_OPT0*32 + COL_ARROW + ARROW_POS*64
    rep #$20
    .a16
    lda ARROW_POS
    and #$00FF                  ; keep only the byte (NEED_REDRAW lives at +1)
    asl
    asl
    asl
    asl
    asl
    asl                         ; * 64
    clc
    adc #(ROW_OPT0 * 32 + COL_ARROW)
    sta VMADDL
    sep #$20
    .a8
    lda #T_ARR
    sta VMDATAL
    stz VMDATAH

    rts
.endproc

; ==================================================================
;  Data: 4bpp font, palette, menu strings
; ==================================================================

; 4bpp tile macro: 8 row bytes in plane 0; planes 1-3 are zero (2-color
; tiles: backdrop + text). Each tile is 32 bytes.
.macro TILE r0,r1,r2,r3,r4,r5,r6,r7
    .byte r0, 0, r1, 0, r2, 0, r3, 0
    .byte r4, 0, r5, 0, r6, 0, r7, 0
    .res 16, 0
.endmacro

font_data:
    TILE $00, $00, $00, $00, $00, $00, $00, $00   ;  0: SPACE
    TILE $3C, $42, $42, $7E, $42, $42, $42, $00   ;  1: A
    TILE $3C, $42, $40, $40, $40, $42, $3C, $00   ;  2: C
    TILE $7C, $42, $42, $42, $42, $42, $7C, $00   ;  3: D
    TILE $7E, $40, $40, $78, $40, $40, $7E, $00   ;  4: E
    TILE $7E, $40, $40, $78, $40, $40, $40, $00   ;  5: F
    TILE $38, $10, $10, $10, $10, $10, $38, $00   ;  6: I
    TILE $40, $40, $40, $40, $40, $40, $7E, $00   ;  7: L
    TILE $42, $66, $5A, $5A, $42, $42, $42, $00   ;  8: M
    TILE $3C, $42, $42, $42, $42, $42, $3C, $00   ;  9: O
    TILE $7C, $42, $42, $7C, $48, $44, $42, $00   ; 10: R
    TILE $3C, $42, $40, $3C, $02, $42, $3C, $00   ; 11: S
    TILE $7E, $10, $10, $10, $10, $10, $10, $00   ; 12: T
    TILE $42, $42, $42, $42, $42, $24, $18, $00   ; 13: V
    TILE $42, $42, $24, $18, $24, $42, $42, $00   ; 14: X
    TILE $3C, $42, $02, $1C, $02, $42, $3C, $00   ; 15: 3
    TILE $40, $20, $10, $08, $10, $20, $40, $00   ; 16: >
font_data_end:
FONT_SIZE = font_data_end - font_data

; CGRAM: two colours -- black backdrop, white text. (The bring-up bisection used
; a red backdrop so we could tell "kernel reached unblank" from "kernel hung".)
palette_data:
    .word $0000                 ; color 0: black (backdrop)
    .word $7FFF                 ; color 1: white (letter foreground)
palette_data_end:
PAL_SIZE = palette_data_end - palette_data

; Menu strings (each byte is a tile index, not ASCII).
title_str:  .byte T_S, T_E, T_L, T_E, T_C, T_T, T_SPC, T_D, T_E, T_M, T_O
title_len = * - title_str

opt0_str:   .byte T_F, T_M, T_V, T_SPC, T_D, T_E, T_M, T_O
opt0_len  = * - opt0_str

opt1_str:   .byte T_3, T_D, T_SPC, T_D, T_E, T_M, T_O
opt1_len  = * - opt1_str

opt2_str:   .byte T_M, T_I, T_X, T_E, T_R, T_SPC, T_D, T_E, T_M, T_O
opt2_len  = * - opt2_str
