; ============================================================
;  kernel.s — the RAM-resident kernel (supplied by the copro,
;  copied into WRAM by boot.s, runs from $0400 in bank $00).
;
;  Per-frame lockstep with the coprocessor:
;     post joypad -> mailbox  ->  signal copro  ->  wait copro done
;     ->  DMA the staged payload into the PPU (in vblank)  ->  repeat
;
;  STUB: the control flow + handshake + one concrete DMA (CGRAM) are
;  real; the rest of the transfer list and double-buffering are TODO.
;
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"
.include "copro.inc"

.segment "KERNEL"               ; LOAD in ROM window, RUN at $0400 (see snes.cfg)

; entry MUST be the first byte — boot.s does `jmp __KERNEL_RUN__`.
.proc kmain
    .a8
    .i16
    ; arrives native, A8/I16, DBR=$00 (set by boot.s)

    ; install our NMI handler into the RAM vector the trampoline uses
    rep #$20
    .a16
    lda #.loword(nmi)
    sta RAMVEC_NMI
    sep #$20
    .a8

    stz KFRAME_FLAG

    lda #$0F
    sta INIDISP             ; screen on, full brightness
    lda #$81
    sta NMITIMEN            ; NMI on (b7) + auto-joypad read (b0)
                            ; NOTE: auto-joypad costs ~3 vblank lines of DMA
                            ; budget; for the full window, clear b0 and read the
                            ; pad manually outside the burst.
    cli

@loop:
    ; 1) forward the joypad to the copro by READING two page-aligned ports.
    ;    The access *is* the message (it also acks the previous frame and asks
    ;    for the next). 8-bit index over a page-aligned base never crosses the
    ;    page, so each read is a single clean bus access the copro latches.
    sep #$10
    .i8
    ldx JOY1L
    lda f:JOYPORT_LO_L,x        ; strobe low byte
    ldx JOY1H
    lda f:JOYPORT_HI_L,x        ; strobe high byte
    rep #$10
    .i16

    ; 2) wait for the copro to report the frame payload is staged
@wait:
    lda f:COPRO_STATUS_L
    and #ST_FRAME_RDY
    beq @wait

    ; 4) arm the DMA burst; the next vblank NMI runs it and clears the flag
    lda #$01
    sta KFRAME_FLAG
@spin:
    lda KFRAME_FLAG
    bne @spin
    bra @loop
.endproc

; ------------------------------------------------------------------
; vblank: push the copro-staged transfer list into the PPU.
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
    lda RDNMI               ; acknowledge NMI

    lda KFRAME_FLAG
    beq @out                ; nothing staged this vblank

    ; (Real hardware: assert/extend forced blank for the letterbox lines so the
    ;  whole 54-line window is DMA-able — see the DMA-budget notes.)

    ; ---- CGRAM: 256 B  COPRO_BANK:PL_CGRAM -> $2122  (concrete example) ----
    stz CGADD               ; CGRAM word address 0
    lda #$00
    sta DMAP0               ; pattern 0 (1 reg), A-bus increments, A->B
    lda #<CGDATA
    sta BBAD0               ; B-bus = $2122
    rep #$20
    .a16
    lda #PL_CGRAM
    sta A1T0L               ; A-bus address
    lda #PL_CGRAM_LEN
    sta DAS0L               ; byte count
    sep #$20
    .a8
    lda #COPRO_BANK
    sta A1B0                ; A-bus bank
    lda #$01
    sta MDMAEN              ; fire channel 0

    ; ---- TODO: tilemap + CHR -> VRAM ($2118 via VMAIN/VMADDL/H, BBAD=$18),
    ;            OAM later, then flip BG1SC/BG12NBA bases for double-buffering.

    stz KFRAME_FLAG         ; the loop's next joypad post acks the frame to the copro

@out:
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc
