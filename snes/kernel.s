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

    ; --- BG setup (Mode 1, BG1 4bpp; tilemap at $0000, CHR at $1000) ---
    lda #$01
    sta BGMODE
    stz BG1SC               ; BG1 tilemap word addr 0, 32x32 size
    lda #$01
    sta BG12NBA             ; BG1 char base = 1  (= word $1000)
    sta TM                  ; main-screen enable = BG1 only

    ; --- DMA channel 0 source-bank constant (the only field shared across
    ;     every list slot; BBAD/DMAP/A1T/DAS come from the slot itself). ---
    lda #COPRO_BANK
    sta A1B0

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

    ; 3) sleep until the next vblank. NMI fires, reads COPRO_DMACTRL, and
    ;    dispatches whichever DMAs the copro requested (or none if it wrote
    ;    $00). After RTI we land back here in active display and loop.
    wai
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

    ; --- frame_ready gate ----------------------------------------------
    ; The copro writes COPRO_FRAME_RDY to a non-zero value once it has
    ; finished staging the per-frame payload AND the DMA list. 0 here
    ; means "nothing to do this vblank; just RTI and the previous frame
    ; stays on screen."
    lda f:COPRO_FRAME_RDY_L
    beq @out

    ; (Real hardware: assert/extend forced blank for the letterbox lines so the
    ;  whole 54-line window is DMA-able -- see the DMA-budget notes. TODO.)

    ; --- walk the 8-slot DMA list -------------------------------------
    ; For each slot whose bbus byte is non-zero: program channel 0 from
    ; the slot, write the prep value to the corresponding PPU dest
    ; register (CGADD/VMADD/OAMADDR), and fire MDMAEN bit 0. Channel 0
    ; is reused across slots -- SNES DMA channels never run in parallel
    ; anyway, so this is functionally identical to using 8 channels.
    rep #$10
    .i16
    ldx #0
@slot:
    cpx #(8 * 8)            ; processed all 8 slots? -> done
    beq @done

    lda f:COPRO_DMA_LIST_L+0,x    ; bbus (0 => empty slot, skip)
    beq @next
    sta BBAD0
    lda f:COPRO_DMA_LIST_L+1,x    ; dmap
    sta DMAP0
    rep #$20
    .a16
    lda f:COPRO_DMA_LIST_L+2,x    ; src
    sta A1T0L
    lda f:COPRO_DMA_LIST_L+4,x    ; size
    sta DAS0L
    sep #$20
    .a8

    ; prep dispatch: write the slot's +6..+7 value to whichever PPU dest
    ; register the bbus byte names. Unknown bbus values fall through with
    ; no prep written -- the copro is trusted to put a valid byte here.
    lda BBAD0
    cmp #<CGDATA            ; $22 -> CGADD (low byte only; CGRAM is word-addressed but the reg is 8-bit)
    bne @check_v
    lda f:COPRO_DMA_LIST_L+6,x
    sta CGADD
    bra @fire
@check_v:
    cmp #<VMDATAL           ; $18 -> set VMAIN word-step, write VMADDL/H
    bne @check_o
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda f:COPRO_DMA_LIST_L+6,x
    sta VMADDL
    sep #$20
    .a8
    bra @fire
@check_o:
    cmp #<OAMDATA           ; $04 -> write OAMADDL/H
    bne @fire
    rep #$20
    .a16
    lda f:COPRO_DMA_LIST_L+6,x
    sta OAMADDL
    sep #$20
    .a8

@fire:
    lda #$01
    sta MDMAEN              ; fire channel 0; CPU pauses until this slot completes

@next:
    ; advance to the next slot (entry size = 8 bytes)
    .repeat 8
    inx
    .endrepeat
    bra @slot

@done:
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
