; ============================================================
;  siphon_dbuf_test.s — DOUBLE-BUFFERED per-scanline VRAM siphon, driven
;  by the kernel's exact 3-state machine, in isolation (ROM data, no DLL).
;
;  Replicates the FMV siphon integration that blacks the screen live:
;    - Two CHR buffers A($0000)/B($4000) + two tilemaps TM_A/TM_B.
;    - The TOP 630 tiles (rows 1..21) are written to BOTH buffers ONCE at
;      init (the "burst" content — static here; we're testing the siphon,
;      not the burst budget).
;    - Each frame the 3-state timer IRQ:
;        State A (V=VIS_END): force-blank, CLEAR the BACK buffer's bottom
;          150 tiles (so a non-delivering siphon shows black, not stale).
;        State B (V=TOP_LB): unblank, arm the siphon (VMADD=back+10080,
;          source reset, VLINE=TOP_LB+1, HTIME), -> State SIPHON.
;        State SIPHON (per visible scanline): setup A1T0/DAS0 OUTSIDE the
;          force-blank, then force-blank / fire 24B DMA / unblank in the
;          right pillar; advance source, RE-ARM VTIME=++VLINE (H+V mode,
;          never H-only); at the line counter hitting 0, FLIP BG1 to the
;          back buffer and -> State A.
;
;  PASS  -> bottom 5 rows show solid colours 8..12 (the siphoned tiles),
;           stable, top 21 rows show the static rainbow, no force-blank
;           band crawling through the visible region.
;  FAIL modes (what we're hunting):
;     bottom black / partial         -> siphon not delivering all 200 lines
;        (ISR > 1 scanline -> H/V re-arm skips). Count coloured rows: each
;        bottom row = 40 siphon lines, so "2 rows + black" = ~80 lines.
;     vertical band in visible region -> force-blank lands mid-visible
;        (tune HTIME_VAL below; rebuild).
;     whole screen black / flicker   -> state machine / flip broken.
;
;  Build:  ca65 --cpu 65816 -o build/siphon_dbuf_test.o siphon_dbuf_test.s
;          ld65 -C siphon_dbuf_test.cfg -o build/siphon_dbuf_test.sfc \
;               build/siphon_dbuf_test.o
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
BYTES_PER_LINE = 24
SIPHON_LINES  = SIPHON_BYTES_TOTAL / BYTES_PER_LINE   ; 200
HTIME_VAL     = 155                ; per-line siphon fire pos (TUNE THIS) — was 130;
                                   ; bumped so the force-blank asserts in H-blank
                                   ; (~H>=256) instead of ~16px into visible content.

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
    ; buffer A top
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
    ; buffer B top
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
    lda #$78               ; BG1SC: TM_B ($7800>>10=30, <<2)
    sta BG1SC
    lda #$04               ; BG12NBA: BG1 page 4 = CHR_B ($4000>>12)
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
;  irq — the 3-state machine (mirrors snes/kernel.s).
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
    sta INIDISP            ; force-blank
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda back_chr
    clc
    adc #SIPHON_VRAM
    sta VMADDL             ; VRAM = back + 10080
    lda #.loword(fill_word)   ; DIAG: marker clear (colour 15) — see fill_word
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
    ; ===== State B: unblank, arm the siphon =====
    lda #$0F
    sta INIDISP
    ; set up channel-0 VRAM DMA ONCE (VMADD auto-incs across the per-line
    ; fires; A1B0 stays 0 from init; only A1T0/DAS0 change per line).
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
    sta VMADDL             ; set once
    stz src_off            ; reset running source offset
    sep #$20
    .a8
    lda #SIPHON_LINES
    sta line_rem
    lda #(TOP_LB+1)
    sta vline
    sta VTIMEL
    stz VTIMEH
    lda #HTIME_VAL
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
    ; back = A -> show A
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
    stz HTIMEH
    lda #VIS_END
    sta VTIMEL
    stz VTIMEH
    stz frame_st           ; -> state A
    jmp @ack

@sip_line:
    ; setup OUTSIDE force-blank (slow part)
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
    ; --- critical section: lands in the right pillar / H-blank ---
    lda #$8F
    sta INIDISP            ; force-blank
    lda #$01
    sta MDMAEN             ; 24 B -> VRAM (VMADD auto-incs)
    lda #$0F
    sta INIDISP            ; unblank
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

zero_word:
    .word $0000
; DIAG: the State-A clear fills the back-buffer bottom with this each frame.
; $FFFF -> solid colour 15 (marker). Any bottom row still showing the marker
; was NOT delivered by the siphon this frame (exposes a partial per-frame
; line rate). Set back to zero_word once per-frame delivery is confirmed full.
fill_word:
    .word $FFFF

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
    .word $03FF        ; 15 MARKER (yellow) — State-A clear fills the back
                       ;    bottom with this; siphon overwrites delivered rows

; TOP 630 tiles (rows 0..20): colour = (row % 7) + 1  (rainbow bands)
chr_top:
    .repeat BURST_TILES, I
        solid4 (((I / TW) .MOD 7) + 1)
    .endrepeat

; BOTTOM 150 tiles (rows 21..25): colour = 8 + (row - 21)  (8..12)
chr_bottom:
    .repeat SIPHON_TILES, I
        solid4 (8 + (I / TW))
    .endrepeat

; 32x32 tilemap: content rows 1..26 (screen lines 8..215 under TOP_LB=8),
; cols 0..29 -> tile (row)*30 + col. Everything else = blank tile (NTILES,
; whose CHR is left zero => backdrop).
tilemap_data:
    .repeat 32, R          ; tilemap row
        .repeat 32, C      ; tilemap col
            .if (R >= 1) && (R <= TH) && (C >= 1) && (C <= TW)
                .word ((R - 1) * TW + (C - 1))   ; centered: cols 1..30 (8px pads)
            .else
                .word NTILES
            .endif
        .endrepeat
    .endrepeat

; ------------------------------------------------------------------
;  HiROM header + vectors
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "SIPHON DBUF TEST     "   ; 21 chars
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
