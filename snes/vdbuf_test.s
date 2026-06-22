; ============================================================
;  vdbuf_test.s — isolate the depth-2 FMV "shimmer" to the
;  DOUBLE-BUFFER FLIP under continuous mid-frame bursting.
;
;  vnmi_test proved SINGLE-buffer continuous bursting (8 KB/frame into
;  VRAM word 0, virtual-NMI State A/B, FB(8,8) letterbox) is CLEAN in
;  both bsnes-plus and ares. The real FMV at depth-2 adds exactly one
;  thing on top of that: a per-frame DOUBLE-BUFFER flip — BG12NBA
;  toggling between CHR_A ($0000) and CHR_B ($4000), bursting the BACK
;  buffer while the FRONT is displayed. That depth-2 path shimmers; the
;  whole mgapi host pipeline was instrumented clean, so the suspect is
;  the flip+continuous-burst combo as rendered by the PPU.
;
;  This ROM reproduces JUST that, standalone, with STATIC content:
;    * Both CHR_A and CHR_B preloaded with the SAME 256-tile striped
;      image at boot.
;    * Every frame: force-blank, re-burst the SAME image into the BACK
;      buffer, flip BG12NBA to display the FRONT (= last frame's back),
;      swap, unblank. Continuous bursting + a flip every frame.
;
;  Because both buffers hold identical content, a CORRECT flip is
;  invisible — the screen is rock-steady stripes. Any shimmer/flash
;  means the continuous double-buffer flip itself corrupts the display.
;
;    PASS (steady stripes) -> double-buffer flip is clean; the FMV
;            shimmer is elsewhere (e.g. real changing content / cadence).
;    FAIL (shimmer/flash)  -> reproduced. Then load the SAME .sfc in
;            ares: clean in ares => bsnes rendering artifact (20fps OK
;            on real hw); shimmers in ares => real SNES behavior.
;
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"

TOP_LB        = 8               ; top letterbox lines
BOT_LB        = 8               ; bottom letterbox lines
VIS_END       = 224 - BOT_LB    ; 216 — first bottom-letterbox line
BURST_TILES   = 256
BURST_BYTES   = BURST_TILES * 32    ; 8192
CHR_A_WORD    = $0000
CHR_B_WORD    = $4000
TILEMAP_WORD  = $7C00
TILEMAP_BYTES = 32 * 32 * 2     ; 2048
PAL_BYTES     = 16 * 2

; Direct-page state.
state    = $00                  ; 0 = A (blank+burst), 1 = B (unblank)
back_is_b = $02                 ; 0 = burst into A this frame, 1 = into B

.segment "CODE"

; ------------------------------------------------------------------
;  reset_handler
; ------------------------------------------------------------------
.proc reset_handler
    .a8
    .i8
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
    plb                         ; DBR = $00

    lda #$80
    sta INIDISP                 ; force blank during init

    lda #$01
    sta BGMODE
    lda #$7C                    ; BG1SC: tilemap @ word $7C00, 32x32
    sta BG1SC
    stz BG12NBA                 ; BG1 CHR base = word $0000 (A) to start
    lda #$01
    sta TM                      ; main screen = BG1
    stz A1B0                    ; DMA ch0 source bank = $00

    ; --- palette ---
    stz CGADD
    stz DMAP0
    lda #<CGDATA
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(palette_data)
    sta A1T0L
    lda #PAL_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    lda #$80
    sta VMAIN                   ; word step, inc after high byte

    ; --- clear all VRAM: 32768 B of $00 (fixed-source DMA) ---
    rep #$20
    .a16
    stz VMADDL                  ; VRAM word 0
    lda #.loword(zero_word)
    sta A1T0L
    lda #$8000                  ; 32768 bytes
    sta DAS0L
    sep #$20
    .a8
    lda #$09                    ; DMAP: pattern 1 (2 regs) + fixed src (bit3)
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; --- preload BOTH CHR buffers with the SAME striped image ---
    jsr load_chr_a
    jsr load_chr_b

    ; --- tilemap upload (cell N -> tile N mod 256) ---
    rep #$20
    .a16
    lda #TILEMAP_WORD
    sta VMADDL
    sep #$20
    .a8
    lda #$01                    ; 2 regs ascending
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(tilemap_data)
    sta A1T0L
    lda #TILEMAP_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    ; --- arm the self-chaining V-IRQ in state A @ V=VIS_END ---
    stz state                   ; state A
    stz back_is_b               ; first burst goes into A
    lda #<VIS_END
    sta VTIMEL
    lda #>VIS_END
    sta VTIMEH
    lda #22                     ; H target = 22 dots (H+V alignment)
    sta HTIMEL
    stz HTIMEH

    lda #$0F
    sta INIDISP                 ; unblank

    lda #$30                    ; NMITIMEN: NMI OFF, H+V IRQ ON (bits 4+5)
    sta NMITIMEN

    cli

@idle:
    wai
    bra @idle
.endproc

; ------------------------------------------------------------------
;  load_chr_a / load_chr_b — upload the striped image to a CHR buffer.
;  Entry A8. Clobbers A. DBR = $00.
; ------------------------------------------------------------------
.proc load_chr_a
    .a8
    rep #$20
    .a16
    lda #CHR_A_WORD
    sta VMADDL
    lda #.loword(chr_data)
    sta A1T0L
    lda #BURST_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    lda #$01
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN
    rts
.endproc

.proc load_chr_b
    .a8
    rep #$20
    .a16
    lda #CHR_B_WORD
    sta VMADDL
    lda #.loword(chr_data)
    sta A1T0L
    lda #BURST_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    lda #$01
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN
    rts
.endproc

; ------------------------------------------------------------------
;  cgram_burst — re-DMA the same 16-colour palette into CGRAM every
;  frame, exactly like the FMV's slot-0 CGRAM upload. Entry A8.
;  Clobbers A. DBR = $00. Source bank A1B0 = $00 (set at boot).
; ------------------------------------------------------------------
.proc cgram_burst
    .a8
    stz CGADD                   ; CGRAM address = 0
    stz DMAP0                   ; 1 reg (CGDATA), increment
    lda #<CGDATA
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(palette_data)
    sta A1T0L
    lda #PAL_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN
    rts
.endproc

; ------------------------------------------------------------------
;  irq_handler — self-chaining two-state machine with a per-frame
;  double-buffer flip. State A: flip display to FRONT, burst SAME image
;  into BACK, swap. This is the depth-2 FMV pattern minus changing
;  content.
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

    lda state
    bne @state_b

    ; --- State A: blank, flip to front, burst into back, swap. ---
    lda #$80
    sta INIDISP                 ; force blank
    jsr cgram_burst             ; v2: per-frame CGRAM DMA (mimic FMV slot 0)

    ; Display the FRONT buffer (= last frame's back = NOT the one we are
    ; about to burst). back_is_b==1 -> front is A ($00); ==0 -> front is B ($04).
    lda back_is_b
    bne @front_a
    lda #$04                    ; back is A -> front is B -> BG12NBA page 4
    sta BG12NBA
    bra @burst
@front_a:
    stz BG12NBA                 ; back is B -> front is A -> BG12NBA page 0

@burst:
    ; Burst the SAME image into the BACK buffer.
    lda back_is_b
    bne @burst_b
    jsr load_chr_a              ; back = A
    bra @swap
@burst_b:
    jsr load_chr_b              ; back = B

@swap:
    lda back_is_b
    eor #$01
    sta back_is_b               ; toggle back for next frame

    lda #<TOP_LB                ; next event = unblank at top_lb
    sta VTIMEL
    lda #>TOP_LB
    sta VTIMEH
    lda #$01
    sta state                   ; -> state B
    bra @ack

@state_b:
    ; --- State B: unblank for the visible region. ---
    lda #$0F
    sta INIDISP
    lda #<VIS_END               ; next event = blank+burst at 224-bot_lb
    sta VTIMEL
    lda #>VIS_END
    sta VTIMEH
    stz state                   ; -> state A

@ack:
    lda $4211                   ; ack IRQ (TIMEUP)
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

; ------------------------------------------------------------------
;  Data (identical to vnmi_test so a side-by-side comparison is clean)
; ------------------------------------------------------------------
zero_word:
    .word $0000

.macro solid_tile p0, p1, p2, p3
    .repeat 8
        .byte p0, p1
    .endrepeat
    .repeat 8
        .byte p2, p3
    .endrepeat
.endmacro

.macro band p0, p1, p2, p3
    .repeat 32
        solid_tile p0, p1, p2, p3
    .endrepeat
.endmacro

chr_data:
    band $FF, $00, $00, $00     ; band 0: idx 1
    band $00, $FF, $00, $00     ; band 1: idx 2
    band $FF, $FF, $00, $00     ; band 2: idx 3
    band $00, $00, $FF, $00     ; band 3: idx 4
    band $FF, $00, $FF, $00     ; band 4: idx 5
    band $00, $FF, $FF, $00     ; band 5: idx 6
    band $FF, $FF, $FF, $00     ; band 6: idx 7
    band $00, $00, $00, $FF     ; band 7: idx 8  (WHITE — burst TAIL marker)

palette_data:
    .word $0000                 ; 0 black backdrop
    .word $001F                 ; 1 red
    .word $03E0                 ; 2 green
    .word $03FF                 ; 3 yellow
    .word $7C00                 ; 4 blue
    .word $7FE0                 ; 5 cyan
    .word $7C1F                 ; 6 magenta
    .word $4210                 ; 7 grey
    .word $7FFF                 ; 8 white
    .repeat 7
        .word $0000
    .endrepeat

tilemap_data:
    .repeat 1024, i
        .word (i .mod 256)
    .endrepeat

; ------------------------------------------------------------------
;  HiROM header + vectors
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "VDBUF FLIP TEST      "
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
    .word $0000              ; FFE4 native COP
    .word $0000              ; FFE6 native BRK
    .word $0000              ; FFE8 native ABORT
    .word irq_handler        ; FFEA native NMI (unused; NMI disabled)
    .word $0000              ; FFEC reserved
    .word irq_handler        ; FFEE native IRQ
    .word $0000              ; FFF0 reserved
    .word $0000              ; FFF2 reserved
    .word $0000              ; FFF4 emul COP
    .word $0000              ; FFF6 reserved
    .word $0000              ; FFF8 emul ABORT
    .word irq_handler        ; FFFA emul NMI
    .word reset_handler      ; FFFC emul RESET
    .word irq_handler        ; FFFE emul IRQ/BRK
