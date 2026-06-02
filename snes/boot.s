; ============================================================
;  boot.s — cart-ROM boot stub (the part the copro serves at reset).
;
;  Brings up the 65816, waits for the copro to stage the RAM
;  kernel in the window, copies it into WRAM, then hands off. The
;  hardware NMI/IRQ vectors point at tiny trampolines that bounce
;  through a RAM pointer, so the RAM kernel owns the interrupts.
;
;  This whole file lives in the window's $8000-$FFFF at reset; the
;  copro may reuse $0000-$FEFF afterwards but MUST keep $FF00-$FFFF
;  (trampolines + vectors) static — see copro.inc.
;
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"
.include "copro.inc"

; symbols the linker exports for the KERNEL segment (LOAD in ROM,
; RUN in WRAM): where it sits in the window, where it runs, its size.
.import __KERNEL_LOAD__, __KERNEL_RUN__, __KERNEL_SIZE__

.segment "BOOT"
.proc reset
    .a8
    .i8
    sei
    clc
    xce                     ; emulation -> native
    rep #$38                ; A/X/Y 16-bit, decimal off
    .a16
    .i16
    ldx #$01FF
    txs                     ; stack at $01FF
    lda #$0000
    tcd                     ; direct page = $0000
    phk
    plb                     ; data bank = $00 (for $21xx/$42xx + low RAM)
    sep #$20
    .a8

    lda #$8F
    sta INIDISP             ; forced blank, brightness 0
    stz NMITIMEN            ; NMI off, auto-joypad off (for now)

    ; --- wait until the copro has staged the kernel blob ---
    ; (skipped for the standalone smoke build: the blob is already in the ROM)
.ifndef SMOKE_TEST
@wait_kernel:
    lda f:COPRO_STATUS_L
    and #ST_KERNEL_RDY
    beq @wait_kernel
.endif

    ; --- copy kernel: window(LOAD, bank $00) -> WRAM(RUN, bank $00) ---
    ; byte copy for clarity; MVN is the fast path once the size is fixed.
    ldx #$0000
@copy:
    lda f:__KERNEL_LOAD__,x     ; long,x : read from the ROM window
    sta a:__KERNEL_RUN__,x      ; abs,x  : write to low WRAM (DBR=$00)
    inx
    cpx #__KERNEL_SIZE__
    bne @copy

.ifndef SMOKE_TEST
    ; --- tell the copro we are running from RAM (read-strobe; go runtime) ---
    lda f:STROBE_BOOTED_L       ; the access itself is the signal; value ignored
.endif

    ; --- hand off to the RAM kernel (entry = first byte of KERNEL) ---
    jmp a:__KERNEL_RUN__        ; bank $00, native, A8/I16
.endproc

; ------------------------------------------------------------------
; Interrupt trampolines (top page, copro-preserved). The RAM kernel
; writes its handler addresses into RAMVEC_* before enabling NMI.
; ------------------------------------------------------------------
.segment "TRAMP"
nmi_trampoline:
    jmp (RAMVEC_NMI)
irq_trampoline:
    jmp (RAMVEC_IRQ)
; Safety net: a stray BRK (= opcode $00) shouldn't trap the CPU in
; an infinite "BRK -> BRK vector points to $0000 -> read $00 -> BRK"
; loop the way it used to. Pointing the BRK vector here gets us a
; clean RTI back to whatever was executing, so the kernel survives a
; transient and we can see what's on the PPU.
brk_trampoline:
    rti

; ------------------------------------------------------------------
; Minimal SNES header ($FFC0-$FFDF) — lets stock bsnes detect/load this
; as a HiROM for smoke testing; the custom mapper can ignore it.
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "MICROGARBAGE SMOKE"  ; $FFC0 title, 21 bytes...
    .byte $20, $20, $20         ; ...padded with spaces
    .byte $21                   ; $FFD5 map mode: HiROM, slow
    .byte $00                   ; $FFD6 cart type: ROM only
    .byte $06                   ; $FFD7 ROM size (~64 KB)
    .byte $00                   ; $FFD8 RAM size: none
    .byte $01                   ; $FFD9 country: NTSC
    .byte $00                   ; $FFDA developer id
    .byte $00                   ; $FFDB version
    .word $0000                 ; $FFDC-DD checksum complement (placeholder)
    .word $FFFF                 ; $FFDE-DF checksum (placeholder)

; ------------------------------------------------------------------
; 65816 native + emulation vectors ($FFE4-$FFFF).
; ------------------------------------------------------------------
.segment "VECTORS"
    .word $0000             ; FFE4 COP   (native)
    .word brk_trampoline    ; FFE6 BRK -- "just RTI", see TRAMP segment
    .word $0000             ; FFE8 ABORT
    .word nmi_trampoline    ; FFEA NMI
    .word $0000             ; FFEC (reserved)
    .word irq_trampoline    ; FFEE IRQ
    .word $0000             ; FFF0 (reserved)
    .word $0000             ; FFF2 (reserved)
    .word $0000             ; FFF4 COP   (emulation, unused in native)
    .word $0000             ; FFF6 (reserved)
    .word $0000             ; FFF8 ABORT
    .word $0000             ; FFFA NMI
    .word reset             ; FFFC RESET
    .word $0000             ; FFFE IRQ/BRK
