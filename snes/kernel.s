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
    lda #$80
    sta NMITIMEN            ; NMI on (b7); auto-joypad read OFF (b0=0). The
                            ; kernel bit-bangs $4016/$4017 in active display
                            ; instead (see read_joypads below), reclaiming the
                            ; ~3 lines the auto-read would have eaten from the
                            ; vblank DMA window.
    cli

@loop:
    ; @spin just exited, which means the NMI has finished the vblank DMA burst
    ; and we are at the very start of active display. The CPU is idle for the
    ; whole active period; we use a tiny piece of it (~150 cycles) to bit-bang
    ; the joypads and forward all four to the copro before settling in to
    ; wait for the next vblank.
    sep #$10
    .i8

    ; 1) manual joypad read (auto-joypad is disabled in NMITIMEN). Fills PADS,
    ;    16 bits per pad, order matching the auto-read register layout.
    jsr read_joypads

    ; 2) forward all 4 pads to the copro: 8 page-aligned read-strobes. The
    ;    access *is* the message (the copro decodes pad index + lo/hi from
    ;    the high address bits); each is a single clean bus read because the
    ;    base is page-aligned and the 8-bit index can't cross the page. This
    ;    also acks the previous frame and asks for the next.
    ldx PADS+0
    lda f:JOYPORT_P0_LO_L,x
    ldx PADS+1
    lda f:JOYPORT_P0_HI_L,x
    ldx PADS+2
    lda f:JOYPORT_P1_LO_L,x
    ldx PADS+3
    lda f:JOYPORT_P1_HI_L,x
    ldx PADS+4
    lda f:JOYPORT_P2_LO_L,x
    ldx PADS+5
    lda f:JOYPORT_P2_HI_L,x
    ldx PADS+6
    lda f:JOYPORT_P3_LO_L,x
    ldx PADS+7
    lda f:JOYPORT_P3_HI_L,x

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

; ------------------------------------------------------------------
; read_joypads -- bit-bang $4016/$4017 to read up to four pads.
;
; Auto-joypad read is disabled (NMITIMEN bit 0 = 0); the kernel calls
; this once per frame in active display (the moment NMI returns).
; ~150 cycles total -- a tiny fraction of one scanline.
;
; Each port carries TWO controllers' bits in parallel: bit 0 is the
; "primary" pad on that port, bit 1 is the multitap second pad on the
; same port. Reads with no controller present return 1's -- the copro
; (or game logic) can detect a non-connected pad by its signature.
;
; WRAM layout (16 bits per pad, MSB-first matching auto-read format):
;     PADS+0/+1 = pad0 = port-1 main         ($4016 bit 0)
;     PADS+2/+3 = pad1 = port-2 main         ($4017 bit 0)
;     PADS+4/+5 = pad2 = port-1 multitap #2  ($4016 bit 1)
;     PADS+6/+7 = pad3 = port-2 multitap #2  ($4017 bit 1)
;
; In/Out: A8/I8 (caller's responsibility to bracket with sep/rep).
; ------------------------------------------------------------------
.proc read_joypads
    ; latch: $01 then $00 to $4016 -- pads load their shift registers
    lda #$01
    sta JOYSER0
    stz JOYSER0

    ; clear the four 16-bit pad accumulators (8 bytes)
    rep #$20
    .a16
    stz PADS+0
    stz PADS+2
    stz PADS+4
    stz PADS+6
    sep #$20
    .a8

    ldy #16
@bit:
    lda JOYSER0          ; bit 0 = port-1 main, bit 1 = port-1 multitap
    lsr a                ; port-1 main bit -> carry
    rol PADS+0           ; accumulate pad0 lo
    rol PADS+1           ; ...pad0 hi
    lsr a                ; port-1 multitap bit -> carry
    rol PADS+4           ; accumulate pad2 lo
    rol PADS+5
    lda JOYSER1          ; bit 0 = port-2 main, bit 1 = port-2 multitap
    lsr a
    rol PADS+2           ; pad1
    rol PADS+3
    lsr a
    rol PADS+6           ; pad3
    rol PADS+7
    dey
    bne @bit
    rts
.endproc
