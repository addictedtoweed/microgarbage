; ============================================================
;  vnmi_test.s — validate the "virtual NMI" kernel architecture.
;
;  fblank_test proved a 32 B mid-frame forced-blank write lands.
;  This proves the full ARCHITECTURE the real kernel will use:
;
;    * NMI DISABLED entirely (NMITIMEN bit7 = 0).
;    * A single self-chaining V-counter IRQ runs a two-state machine,
;      reprogramming its own next V target each fire (no NMI exists to
;      reprogram it):
;        State A @ V=224-bot_lb (bottom-letterbox blank transition,
;                 the "virtual NMI"): force-blank, run a realistic
;                 ~8 KB CPU-DMA CHR burst, set next target = top_lb,
;                 flip to B.
;        State B @ V=top_lb: unblank, set next target = 224-bot_lb,
;                 flip to A.
;    * Real FB(8,8) letterbox produced purely by those two INIDISP
;      transitions — exactly the target kernel's mechanism.
;
;  The burst re-uploads 256 tiles (8192 B) every frame to VRAM word 0.
;  The 256 tiles are 8 colour bands of 32 tiles; band 7 (the burst
;  TAIL) is WHITE. The tilemap tiles cell N -> tile (N mod 256), so the
;  8-band pattern repeats down the screen. VRAM CHR is cleared to black
;  at boot, so any tile the burst fails to reach stays BLACK.
;
;    PASS -> full screen of repeating 8-colour stripes INCLUDING the
;            white stripes, with clean black letterbox top + bottom,
;            rock steady. The ~8 KB burst fully lands inside the
;            contiguous 54-line blank window every frame.
;    FAIL (truncation) -> the white (tail) stripes are BLACK at the
;            repeat interval -> the window can't sink the whole burst.
;    FAIL (timing) -> flicker, wrong letterbox height, or whole-screen
;            black -> the self-chaining IRQ schedule is wrong.
;
;  Load in bsnes-plus (and ares for a hardware cross-check).
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
TILEMAP_WORD  = $7C00
TILEMAP_BYTES = 32 * 32 * 2     ; 2048
PAL_BYTES     = 16 * 2

; Direct-page state.
state    = $00                  ; 0 = A (blank+burst), 1 = B (unblank)

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
    stz BG12NBA                 ; BG1 CHR base = word $0000
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

    ; --- clear CHR: 1024 tiles × 32 B = 32768 B of $00 to VRAM word 0.
    ;     Fixed-source DMA reads the same zero word repeatedly. ---
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
    lda #<VIS_END
    sta VTIMEL
    lda #>VIS_END
    sta VTIMEH
    lda #22                     ; H target = 22 dots (just past HBLANK), matches
    sta HTIMEL                  ; the real kernel's H+V alignment to kill the
    stz HTIMEH                  ; V-only H-jitter on the blank-start trigger

    lda #$0F
    sta INIDISP                 ; unblank; first visible frame is black
                                ; (CHR still cleared) until the first burst

    lda #$30                    ; NMITIMEN: NMI OFF, H+V IRQ ON (bits 4+5)
    sta NMITIMEN

    cli

@idle:
    wai
    bra @idle
.endproc

; ------------------------------------------------------------------
;  chr_burst — the realistic ~8 KB DMA burst. 256 tiles (8192 B) from
;  chr_data to VRAM word 0. Caller has asserted forced blank.
;  Clobbers A; leaves A 8-bit. DBR = $00.
; ------------------------------------------------------------------
.proc chr_burst
    .a8
    rep #$20
    .a16
    stz VMADDL                  ; VRAM word 0 (tile 0)
    lda #.loword(chr_data)
    sta A1T0L
    lda #BURST_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    lda #$01                    ; 2 regs ascending (NOT fixed src)
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN                  ; CPU paused here for the whole burst
    rts
.endproc

; ------------------------------------------------------------------
;  irq_handler — self-chaining two-state machine.
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

    ; --- State A: bottom-blank transition. Blank + burst. ---
    lda #$80
    sta INIDISP                 ; force blank (covers bot LB + vblank + top LB)
    jsr chr_burst               ; ~8 KB into VRAM during the blank window
    lda #<TOP_LB                ; next event = unblank at top_lb
    sta VTIMEL
    lda #>TOP_LB
    sta VTIMEH
    lda #$01
    sta state                   ; -> state B
    bra @ack

@state_b:
    ; --- State B: top-letterbox end. Unblank for the visible region. ---
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
;  Data
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

; 8 bands × 32 tiles. Band B uses palette index (B+1); band 7 = idx 8 = WHITE.
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
    .byte "VNMI ARCH TEST       "
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
