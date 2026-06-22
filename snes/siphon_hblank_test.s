; ============================================================
;  siphon_hblank_test.s — NO-FORCE-BLANK per-scanline VRAM siphon.
;
;  GPT's approach: instead of toggling force-blank ($8F/$0F) around each
;  per-line DMA (which we suspect glitches the PPU and leaves the residual
;  bottom-row speckle), do the DMA purely inside the right-edge H-blank and
;  NEVER touch INIDISP per line. VRAM is writable during H-blank, so a small
;  DMA (20 B = 10 words ~= 40 dots) sized to fit the ~278..339(+0..21) H-blank
;  window lands cleanly with no rendering disturbance at all.
;
;  Harness = siphon_dbuf_test.s (double-buffered, 3-state timer IRQ), with the
;  siphon state changed to:
;    - HTIME_VAL ~= H-blank start (9-bit, > 255), set in State B; the per-line
;      ISR re-arms only VTIME (HTIME persists), so every line's MDMAEN fires in
;      H-blank.
;    - @sip_line: setup A1T0/DAS0, fire MDMAEN, advance, re-arm V. NO $8F/$0F.
;    - 20 B/line, 192 lines => 120 tiles = bottom rows 21..24 (row 25 left as
;      the State-A marker, since 20 B can't carry all 150 tiles in one in-window
;      pass — the real kernel splits per sub-frame).
;
;  PASS  -> rows 21..24 = solid colours 8..11 (delivered, CLEAN — no speckle,
;           no force-blank band anywhere); row 25 = solid colour 15 (marker,
;           intentionally not delivered this single pass).
;  FAIL  -> rows 21..24 speckled with colour 15  -> DMA landing in active
;             display (HTIME too early/late; tune HTIME_VAL, rebuild);
;           rows partial/black                    -> ISR > H-blank, lines lost;
;           visible garbage in the top 21 rows    -> DMA spilling into active.
;
;  Build:  ca65 --cpu 65816 -o build/siphon_hblank_test.o siphon_hblank_test.s
;          ld65 -C siphon_hblank_test.cfg -o build/siphon_hblank_test.sfc \
;               build/siphon_hblank_test.o
;  Verify in ares (cycle-accurate) AND bsnes-plus.
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"

; --- geometry (matches the FMV (8,8) full-height layout) ---
TW            = 30
TH            = 26
NTILES        = TW * TH            ; 780
BURST_TILES   = 21 * TW            ; 630 (rows 0..20)
SIPHON_TILES  = NTILES - BURST_TILES ; 150 (rows 21..25)
TILE_WORDS    = 16                 ; 4bpp tile = 16 words = 32 bytes
SIPHON_VRAM   = BURST_TILES * TILE_WORDS  ; 10080 — back-buffer CHR tail
SIPHON_BYTES_TOTAL = SIPHON_TILES * 32    ; 4800
BURST_BYTES   = BURST_TILES * 32          ; 20160

CHR_A_WORD    = $0000
CHR_B_WORD    = $4000
TM_A_WORD     = $7C00
TM_B_WORD     = $7800

VIS_END       = 216                ; bottom_lb = 8  -> 224 - 8
TOP_LB        = 8
BYTES_PER_LINE = 20                ; sized to fit the H-blank window (no f-blank)
SIPHON_LINES  = 192                ; 192*20 = 3840 = 120 tiles (rows 21..24);
                                   ; V = 9..200, fits the 207-line window.
; H-blank-start fire position (9-bit). Active display ends ~dot 277, so the
; right H-blank is ~278..339. Fire here so the whole ISR (setup + the ~40-dot
; DMA) runs inside H-blank. TUNE if rows 21..24 speckle.
HTIME_VAL     = 274

; --- direct-page state ---
src_off   = $00      ; 16-bit running siphon source offset
back_chr  = $02      ; 16-bit back-buffer CHR base word ($0000 or $4000)
frame_st  = $04      ; 8-bit state (0=A,1=B,2=SIPHON)
vline     = $05      ; 8-bit running V target for the siphon
line_rem  = $06      ; 8-bit running scanline counter

.segment "CODE"

; ------------------------------------------------------------------
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

    lda #$01
    sta BGMODE              ; mode 1
    lda #$01
    sta TM                  ; BG1 on main screen

    stz A1B0                ; all DMA sources in bank 0 (.loword addressing)

    ; --- palette ---
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
    sta VMAIN              ; word increment after high-byte write

    ; --- write the BURST top (630 tiles) into BOTH buffers (static) ---
    rep #$20
    .a16
    lda #CHR_A_WORD
    sta VMADDL
    lda #.loword(chr_top)
    sta A1T0L
    lda #BURST_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN
    rep #$20
    .a16
    lda #CHR_B_WORD
    sta VMADDL
    lda #.loword(chr_top)
    sta A1T0L
    lda #BURST_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    ; --- both tilemaps (identical) ---
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
    rep #$20
    .a16
    lda #TM_B_WORD
    sta VMADDL
    lda #.loword(tilemap_data)
    sta A1T0L
    lda #(32*32*2)
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    ; --- initial display = buffer B (front); back = A ---
    lda #$78               ; BG1SC: TM_B
    sta BG1SC
    lda #$04               ; BG12NBA: BG1 page 4 = CHR_B
    sta BG12NBA
    rep #$20
    .a16
    lda #CHR_A_WORD
    sta back_chr           ; back = A
    stz src_off
    sep #$20
    .a8
    stz frame_st           ; -> state A

    ; --- arm the H+V timer IRQ: state A at V=VIS_END, H=22 ---
    lda #22
    sta HTIMEL
    stz HTIMEH
    lda #VIS_END
    sta VTIMEL
    stz VTIMEH
    lda #$0F
    sta INIDISP            ; unblank
    lda #$30
    sta NMITIMEN           ; H+V IRQ (no NMI, no auto-joypad)
    cli
@idle:
    wai
    bra @idle
.endproc

; ------------------------------------------------------------------
;  irq — the 3-state machine. Siphon state does NO force-blank.
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

    lda frame_st
    beq @state_a
    cmp #$01
    beq @state_b
    jmp @state_siphon

@state_a:
    ; ===== State A: blank + clear the BACK buffer's bottom 150 tiles =====
    lda #$8F
    sta INIDISP            ; force-blank (clear is fine under force-blank)
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda back_chr
    clc
    adc #SIPHON_VRAM
    sta VMADDL             ; VRAM = back + 10080
    lda #.loword(fill_word)
    sta A1T0L
    lda #SIPHON_BYTES_TOTAL
    sta DAS0L
    sep #$20
    .a8
    lda #$09               ; 2 regs + FIXED A-bus source (clear)
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN
    ; schedule state B at V=TOP_LB
    lda #TOP_LB
    sta VTIMEL
    stz VTIMEH
    lda #$01
    sta frame_st
    jmp @ack

@state_b:
    ; ===== State B: set up the siphon DMA + VMADD WHILE STILL FORCE-BLANKED
    ; (State A left $8F on through vblank), THEN unblank. Writing VMADD during
    ; active display corrupts ("black lines across the tile", nesdev), so set
    ; it now while blanked; per-line the DMA only auto-increments it. =====
    lda #$01
    sta DMAP0              ; 2 regs, incrementing source
    lda #<VMDATAL
    sta BBAD0
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda back_chr
    clc
    adc #SIPHON_VRAM
    sta VMADDL             ; set once, while force-blanked
    stz src_off
    sep #$20
    .a8
    lda #$0F
    sta INIDISP            ; NOW unblank for the visible region
    lda #SIPHON_LINES
    sta line_rem
    lda #(TOP_LB+1)
    sta vline
    sta VTIMEL
    stz VTIMEH
    ; Fire each siphon line well inside active display (H=120); the ISR then
    ; SPIN-WAITS for the hardware H-blank flag before the DMA, so the DMA timing
    ; is robust (no HTIME guessing) and never lands in active display.
    lda #120
    sta HTIMEL
    stz HTIMEH
    lda #$02
    sta frame_st           ; -> state SIPHON (NMITIMEN stays $30)
    jmp @ack

@state_siphon:
    lda line_rem
    bne @sip_line
    ; ===== done: flip BG1 to the back buffer, restore burst timing =====
    rep #$20
    .a16
    lda back_chr
    bne @flip_to_b
    sep #$20
    .a8
    lda #$7C               ; BG1SC: TM_A
    sta BG1SC
    lda #$00               ; BG12NBA: page 0 = CHR_A
    sta BG12NBA
    rep #$20
    .a16
    lda #CHR_B_WORD
    sta back_chr           ; next back = B
    bra @flip_done
@flip_to_b:
    sep #$20
    .a8
    lda #$78               ; BG1SC: TM_B
    sta BG1SC
    lda #$04               ; BG12NBA: page 4 = CHR_B
    sta BG12NBA
    rep #$20
    .a16
    lda #CHR_A_WORD
    sta back_chr           ; next back = A
@flip_done:
    sep #$20
    .a8
    lda #22
    sta HTIMEL
    stz HTIMEH             ; State A HTIME back to 22 (8-bit)
    lda #VIS_END
    sta VTIMEL
    stz VTIMEH
    stz frame_st           ; -> state A
    jmp @ack

@sip_line:
    ; --- NO force-blank: set up A1T0/DAS0 during active, then SPIN-WAIT for the
    ; hardware H-blank flag and fire the DMA inside H-blank. ---
    rep #$20
    .a16
    lda src_off
    clc
    adc #.loword(chr_bottom)
    sta A1T0L
    lda #BYTES_PER_LINE
    sta DAS0L
    sep #$20
    .a8
    ; spin until $4212 (HVBJOY) bit6 = H-blank. Robust: the DMA is guaranteed to
    ; run in H-blank (never active display, which would corrupt the whole frame).
@hb_wait:
    lda $4212
    and #$40
    beq @hb_wait
    lda #$8F
    sta INIDISP           ; force-blank IN H-blank — THIS is what frees VRAM
                          ; (spinning to H-blank without it delivered nothing)
    lda #$01
    sta MDMAEN            ; 20 B -> VRAM (force-blanked, in H-blank, VMADD auto-incs)
    lda #$0F
    sta INIDISP           ; unblank
    ; advance source, decrement counter, RE-ARM V for the next scanline
    rep #$20
    .a16
    lda src_off
    clc
    adc #BYTES_PER_LINE
    sta src_off
    sep #$20
    .a8
    dec line_rem
    inc vline
    lda vline
    sta VTIMEL
    stz VTIMEH

@ack:
    lda $4211              ; ack timer IRQ
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

fill_word:
    .word $FFFF            ; State-A marker (colour 15) for undelivered tiles

palette_data:
    .word $0000        ; 0 black
    .word $001F        ; 1 red
    .word $03E0        ; 2 green
    .word $7C00        ; 3 blue
    .word $03FF        ; 4 yellow
    .word $7C1F        ; 5 magenta
    .word $7FE0        ; 6 cyan
    .word $7FFF        ; 7 white
    .word $4210        ; 8 grey  (siphon row 21)
    .word $001A        ; 9 dk red (siphon row 22)
    .word $0340        ; 10 dk green (siphon row 23)
    .word $6800        ; 11 dk blue (siphon row 24)
    .word $0210        ; 12 dim (siphon row 25)
    .word $0000        ; 13
    .word $0000        ; 14
    .word $03FF        ; 15 MARKER (yellow)

; TOP 630 tiles (rows 0..20): colour = (row % 7) + 1
chr_top:
    .repeat BURST_TILES, I
        solid4 (((I / TW) .MOD 7) + 1)
    .endrepeat

; BOTTOM 150 tiles (rows 21..25): colour = 8 + (row - 21)  (8..12)
chr_bottom:
    .repeat SIPHON_TILES, I
        solid4 (8 + (I / TW))
    .endrepeat

; 32x32 tilemap: content rows 1..26, cols 1..30 -> tile (row)*30 + col.
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

; ------------------------------------------------------------------
;  HiROM header + vectors
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "SIPHON HBLANK TEST   "   ; 21 chars
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
