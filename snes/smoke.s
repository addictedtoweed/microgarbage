; ============================================================
;  smoke.s — standalone smoke-test kernel (NOT the real kernel).
;
;  Proves the boot path end to end with no coprocessor: boot.s
;  copies THIS into WRAM and jumps to it, it installs its NMI
;  handler through the RAM trampoline, and every vblank it reads
;  the joypad and — while any button is held — advances the
;  backdrop colour. No BG layers are enabled, so the whole screen
;  is CGRAM[0]: hold a button and the screen cycles colours, let
;  go and it freezes.
;
;  Exercises: WRAM execution, the ROM->RAM NMI vector indirection,
;  vblank timing, auto-joypad read, CGRAM writes. Runs as a plain
;  HiROM in stock bsnes; also valid once the custom mapper is in.
;
;  Build:  .\snes\build.ps1 -Smoke   (defines SMOKE_TEST for boot.s)
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"
.include "copro.inc"

COLOR = $0214                   ; smoke: current backdrop colour (BGR555, 16-bit)

.segment "KERNEL"               ; LOAD in ROM window, RUN at $0400 (boot copies it)

; entry MUST be first — boot.s does `jmp __KERNEL_RUN__`.
.proc kmain
    .a8
    .i16
    ; install our NMI handler into the RAM vector the trampoline reads
    rep #$20
    .a16
    lda #.loword(nmi)
    sta RAMVEC_NMI
    lda #$001F              ; seed colour (red)
    sta COLOR
    sep #$20
    .a8

    lda #$0F
    sta INIDISP             ; screen on, full brightness
    stz TM                  ; no BG layers -> whole screen = backdrop CGRAM[0]
    lda #$81
    sta NMITIMEN            ; enable NMI (b7) + auto-joypad read (b0)
    ; leave IRQ masked (boot did SEI); NMI is non-maskable, so WAI still wakes.

@idle:
    wai                     ; sleep until the vblank NMI; it does the work
    bra @idle
.endproc

; ------------------------------------------------------------------
; vblank: read the pad; while any button is held, step the backdrop.
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

    rep #$20
    .a16
    lda JOY1L               ; 16-bit pad (auto-read latched this vblank)
    and #$FFF0              ; meaningful button bits (low nibble is always 0)
    beq @paint              ; nothing held -> leave the colour as-is
    lda COLOR
    clc
    adc #$0843              ; arbitrary visible step
    sta COLOR

@paint:
    sep #$20
    .a8
    stz CGADD               ; CGRAM word address 0 (the backdrop)
    lda COLOR
    sta CGDATA              ; low byte
    lda COLOR+1
    sta CGDATA              ; high byte (CGRAM byte pointer auto-advances)

    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc
