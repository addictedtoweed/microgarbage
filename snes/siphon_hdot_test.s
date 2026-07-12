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
.ifndef BYTES_PER_LINE             ; override at build time: ca65 -D BYTES_PER_LINE=16
BYTES_PER_LINE = 20                ; spin-to-H-blank version, clean in ares
.endif                             ; (Gemini: ~14-16 B is the H-blank ceiling; 8
                                   ; gives wide margin). 8*192 = 1536 = 48 tiles.
                                   ; more bytes -> more rows delivered AND bigger
                                   ; per-line DMA, so the ceiling shows as the
                                   ; delivered diagonals breaking up.
; Deliver the WHOLE 150-tile (5-row) siphon region when it fits the active field
; (<=200 lines), else cap at 200 (the rest stays yellow). At >=24 B/line the full
; region fits one pass, so there's NO yellow tail to confuse the read — any
; yellow/scramble in the delivered rows is then a real failure.
.if (SIPHON_BYTES_TOTAL / BYTES_PER_LINE) > 200
SIPHON_LINES  = 200
.else
SIPHON_LINES  = (SIPHON_BYTES_TOTAL / BYTES_PER_LINE)
.endif
; H-counter (9-bit, 0..339) at which the per-line IRQ fires. NO-SPIN variant:
; the ISR force-blanks IMMEDIATELY at this dot (no $4212 spin), so we can place
; the blank in the RIGHT MARGIN (~dot 248-270, blank tile) instead of waiting for
; the H-blank flag (~274). That way the whole blank->DMA->unblank completes before
; the NEXT line's active display starts (~dot 22), so the unblank isn't late and
; the displayed line isn't force-blanked. Spinning to the flag was too late under
; accurate emulation. Build-override + sweep to find the dot that lands in time.
; NO-SPIN default: fire in the RIGHT MARGIN. Visible dots are ~22-277 (256 px);
; the rightmost 8 px is ~dot 269. Firing at ~260 blanks the last ~2-3 tiles + the
; whole H-blank as the DMA window. Sweep on ares: LOWER = bigger DMA window but a
; wider black right-margin; HIGHER = tighter 8px margin but less window. Find the
; highest value where the DMA still completes + unblanks before the next active line.
.ifndef HTIME_TUNE                 ; ca65 -D HTIME_TUNE=NNN to sweep
HTIME_TUNE    = 260
.endif

; --- direct-page state ---
src_off   = $00      ; 16-bit running siphon source offset
back_chr  = $02      ; 16-bit back-buffer CHR base word ($0000 or $4000)
frame_st  = $04      ; 8-bit state (0=A,1=B,2=SIPHON)
vline     = $05      ; 8-bit running V target for the siphon
line_rem  = $06      ; 8-bit running scanline counter
ramvec    = $10      ; 16-bit IRQ vector — the real kernel dispatches every IRQ
                     ; through `jmp (RAMVEC_IRQ)`; mirror that indirection here so
                     ; the per-line latency (and thus H-blank margin) matches.

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

    ; install the IRQ handler in the indirect vector (the vectors point at a
    ; `jmp (ramvec)` trampoline) so every IRQ pays the same dispatch latency the
    ; real kernel does — that latency is what shrinks the usable H-blank window.
    lda #<irq_handler
    sta ramvec
    lda #>irq_handler
    sta ramvec+1

    lda #$80
    sta INIDISP             ; force-blank during setup

    lda #$01
    sta BGMODE              ; mode 1
    lda #$01
    sta TM                  ; BG1 ONLY — OBJ OFF (dedicated framebuffer mode: no
                            ; sprite eval/fetch contending for H-blank cycles).
                            ; The OAM/OBJ setup below is now inert (harmless).
    ; OBSEL: name base 0 (OBJ tile 0 = VRAM $0000 = burst tile 0, a solid
    ; pixel-index-1 tile), sizes 8x8/16x16, namegap 0. Sprites reuse burst CHR
    ; as a known solid tile so any fetch glitch is obvious.
    stz OBSEL

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

    ; --- OBJ palette 0: color 0 transparent, color 1 magenta ---
    lda #128
    sta CGADD
    stz DMAP0
    lda #<CGDATA
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(obj_palette)
    sta A1T0L
    lda #(2*2)
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

    ; --- OAM: 96 sprites (12 rows x 8) over the WHOLE active field, incl. the
    ; siphoned rows 21..24 (Y 168..200), so sprite eval+fetch coincides with the
    ; H-blank siphon DMA. Hypothesis: if enabling OBJ contends with the siphon,
    ; either the magenta sprites OR the siphon rows mangle. ---
    stz OAMADDL
    stz OAMADDH
    stz DMAP0              ; 1 reg, 1 byte/xfer
    lda #<OAMDATA
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(oam_data)
    sta A1T0L
    lda #(512+32)         ; low table + high table
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
    bne @state_b           ; frame_st=1 -> State B. Siphon scanlines do NOT pass
                           ; through here — State B installs a lean siphon_isr in
                           ; ramvec, so per-line IRQs bypass this save/dispatch
                           ; (that heaviness is what made the old handler fall
                           ; behind the beam and stall after ~40 lines).
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
    ; Fire each siphon line at HTIME_TUNE (right margin). The no-spin ISR
    ; force-blanks immediately there, so the DMA spans margin+H-blank and the
    ; unblank lands before the next active display.
    lda #<HTIME_TUNE
    sta HTIMEL
    lda #>HTIME_TUNE
    sta HTIMEH
    ; Install the LEAN siphon handler in the indirect vector. Per-line siphon
    ; IRQs now dispatch straight to it (no reg save, no state check), exactly
    ; like the real kernel — short enough to fire EVERY scanline.
    lda #<siphon_isr
    sta ramvec
    lda #>siphon_isr
    sta ramvec+1
    jmp @ack

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

; ------------------------------------------------------------------
;  siphon_isr — LEAN per-scanline handler (installed in ramvec by State B).
;  NO register save (A/X/Y are scratch; the main loop only WAIs), A8/I8. This
;  mirrors the real kernel's siphon_isr so the per-line latency — and thus the
;  H-blank budget — is faithful. On the last line it flips BG1 to the freshly
;  siphoned buffer, restores irq_handler in ramvec, and schedules State A.
; ------------------------------------------------------------------
.proc siphon_isr
    .a8
    .i8
    lda $4211              ; ACK timer IRQ
    lda line_rem
    beq @done
    ; per line: source addr + count (active display — just CPU-reg writes)
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
    ; NO-SPIN (new notes): the IRQ fires at HTIME_TUNE, positioned in the RIGHT
    ; MARGIN (~dot 248-270, a blanked region). Force-blank IMMEDIATELY here so the
    ; DMA window spans the right 8px margin + the full H-blank, then unblank before
    ; the next line's active display begins (~dot 22). No $4212 spin (too late under
    ; accurate emulation). With OBJ off, H-blank has no sprite-fetch contention.
    ; Tune HTIME_TUNE up/down until the unblank consistently beats active display.
    lda #$8F
    sta INIDISP
    lda #$01
    sta MDMAEN
    lda #$0F
    sta INIDISP
    ; advance source, dec counter, re-arm V for the next scanline (H persists)
    rep #$20
    .a16
    lda src_off
    clc
    adc #BYTES_PER_LINE
    sta src_off
    sep #$20
    .a8
    dec line_rem
    beq @done
    inc vline
    lda vline
    sta VTIMEL
    stz VTIMEH
    rti

@done:
    lda #$0F
    sta INIDISP            ; restore display
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
    sta back_chr
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
    sta back_chr
@flip_done:
    sep #$20
    .a8
    ; restore the state-machine handler in ramvec; schedule State A
    lda #<irq_handler
    sta ramvec
    lda #>irq_handler
    sta ramvec+1
    lda #22
    sta HTIMEL
    stz HTIMEH
    lda #VIS_END
    sta VTIMEL
    stz VTIMEH
    stz frame_st           ; -> state A
    rti
.endproc

.proc nmi_stub
    rti
.endproc

; Indirect IRQ trampoline — mirrors the kernel's `jmp (RAMVEC_IRQ)` so the
; siphon ISR's dispatch latency (and therefore its H-blank timing) is faithful.
.proc irq_tramp
    jmp (ramvec)
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

; diag4 PH — a per-tile diagnostic tile: a 1px diagonal (colour index 1 = red)
; whose phase = PH. Correctly delivered, consecutive tiles form a CONTINUOUS
; diagonal staircase across the row; any scramble / byte-shift / dropped line
; visibly breaks it (which solid colours hid). Row R lit at column (R+PH)&7.
.macro diag4 PH
    .repeat 8, R
        .byte ($80 >> ((R + PH) & 7)), $00   ; plane 0 = 1 bit, plane 1 = 0
    .endrepeat
    .res 16, $00                              ; planes 2-3 = 0
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

; TOP 630 tiles (rows 0..20): continuous diagonal staircase, phase = tile index
; (burst-delivered; the positive control — should be clean).
chr_top:
    .repeat BURST_TILES, I
        diag4 I
    .endrepeat

; BOTTOM 150 tiles (rows 21..25): diagonal staircase CONTINUING from the top
; (phase = BURST_TILES + I) — siphon-delivered. If the in-H-blank siphon mangles
; tiles, the bottom diagonals break where the top ones stayed clean.
chr_bottom:
    .repeat SIPHON_TILES, I
        diag4 (BURST_TILES + I)
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

; OBJ palette 0: transparent + magenta (so sprites stand out over any BG colour)
obj_palette:
    .word $0000        ; OBJ color 0 = transparent
    .word $7C1F        ; OBJ color 1 = magenta

; OAM: 12 rows of 8 8x8 sprites, tile 0 (solid pixel-index-1 => magenta),
; priority 3, palette 0; spanning Y=16..192 (covers the siphon rows). Remaining
; 32 sprites hidden at Y=240. High table = 32 B all zero (8x8, X<256).
.macro sprow SY
    .byte  24, SY, 0, $30
    .byte  52, SY, 0, $30
    .byte  80, SY, 0, $30
    .byte 108, SY, 0, $30
    .byte 136, SY, 0, $30
    .byte 164, SY, 0, $30
    .byte 192, SY, 0, $30
    .byte 220, SY, 0, $30
.endmacro
oam_data:
    sprow 16
    sprow 32
    sprow 48
    sprow 64
    sprow 80
    sprow 96
    sprow 112
    sprow 128
    sprow 144
    sprow 160
    sprow 176
    sprow 192
    .repeat 32          ; hide sprites 96..127
        .byte 0, 240, 0, 0
    .endrepeat
    .repeat 32          ; high table: 8x8, X-high 0
        .byte $00
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
    .word irq_tramp          ; native IRQ -> indirect trampoline (kernel-faithful)
    .word $0000
    .word $0000
    .word $0000
    .word $0000
    .word $0000
    .word nmi_stub
    .word reset_handler
    .word irq_tramp          ; emulation IRQ
