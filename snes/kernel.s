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

    ; install our NMI handler into the RAM vector the trampoline uses,
    ; and stash a copy at K_NMI_DEFAULT so @loop can restore it on
    ; "uninstall" (cart version rolls back to 0 when the host unloads
    ; the guest VM).
    rep #$20
    .a16
    lda #.loword(nmi)
    sta RAMVEC_NMI
    sta K_NMI_DEFAULT
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

    ; v2.29 Phase 3a: HDMA channel 7 (INIDISP letterbox via cart_window
    ; table) is no longer enabled — the unified default_hirq_handler
    ; ISR drives INIDISP transitions directly. The emit_inidisp_table
    ; machinery still runs (for back-compat with mg_force_blank) but
    ; its table is now dormant since channel 7 isn't armed. Phase 3b
    ; will rip out emit_inidisp_table and the table region's 256 bytes
    ; for reuse.
    stz HDMAEN              ; no HDMA channels armed at boot

    lda #$0F
    sta INIDISP             ; screen on, full brightness
    lda #$80
    sta NMITIMEN            ; NMI on (b7); auto-joypad read OFF (b0=0). The
                            ; kernel bit-bangs $4016/$4017 in active display
                            ; instead (see read_joypads below), reclaiming the
                            ; ~3 lines the auto-read would have eaten from the
                            ; vblank DMA window.

    ; v2.19: explicitly zero the cached NMI-builder version. boot.s does
    ; NOT scrub all of WRAM (only KRAM at $0400+ via the kernel copy), so
    ; $022A holds whatever the platform left at cold boot. If that happens
    ; to be non-zero, the @loop version-poll below would compare against
    ; cart-window version 0, see "different," and install the all-zero
    ; staged region as a 1 KB BRK loop -- screen goes black until the
    ; next reset. STZ here makes the cache deterministic.
    stz K_NMI_VERSION
    ; v2.26: same defensive zero for HIRQ.
    stz K_HIRQ_VERSION

    ; v2.29 Phase 3a: install the default HIRQ handler at RAMVEC_IRQ
    ; and cache its address at K_HIRQ_DEFAULT so the @loop's HIRQ
    ; uninstall path can restore it. Also zero the layout / siphon
    ; WRAM cache so the first frame's NMI handler doesn't read stale
    ; cold-boot WRAM and end up with garbage line counts.
    rep #$20
    .a16
    lda #.loword(default_hirq_handler)
    sta RAMVEC_IRQ
    sta K_HIRQ_DEFAULT
    sep #$20
    .a8
    stz K_LAYOUT_TOP_LB
    stz K_LAYOUT_BOT_LB
    lda #224
    sta K_LAYOUT_VIS_END
    stz K_SIPHON_BYTES
    stz K_FRAME_STATE

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

    ; v2.21: NMI-builder version poll with uninstall support.
    ;
    ; Cart version semantics:
    ;   0    = "no custom NMI active" (boot default; written by host
    ;          on VM unload to clear the previous demo's handler)
    ;   1..N = "use the handler staged at COPRO_NMI_CODE_L"
    ;
    ; Cache cmp finds three cases:
    ;   match                 -> nothing to do
    ;   diff, new = 0         -> UNINSTALL: restore RAMVEC_NMI to default
    ;   diff, new != 0        -> INSTALL: copy + RAMVEC_NMI = $0E00
    ;
    ; Byte-copy (not MVN) deliberately: MVN's operand-order pitfall
    ; was the suspected v2.18 regression. The copy runs once per
    ; install (rare), so the ~9700-cycle (~57-scanline) cost is
    ; absorbed in active-display CPU idle time and never affects
    ; the next NMI.
    .a8
    lda f:COPRO_NMI_VERSION_L
    cmp K_NMI_VERSION
    beq @nmi_unchanged              ; cache matches -> already at this version
    sta K_NMI_VERSION
    cmp #0                          ; set Z based on A (sta itself doesn't!)
    bne @nmi_install                ; non-zero -> install new handler

    ; UNINSTALL — restore RAMVEC_NMI to the default kernel proc.
    rep #$20
    .a16
    lda K_NMI_DEFAULT
    sta RAMVEC_NMI
    sep #$20
    .a8
    bra @nmi_unchanged

@nmi_install:
    rep #$30
    .a16
    .i16
    ldx #$0000
@nmi_copy:
    lda f:COPRO_NMI_CODE_L,x        ; long,X — 16-bit read from cart
    sta a:K_NMI_CODE_BASE,x         ; abs,X  — 16-bit write to WRAM
    inx
    inx
    cpx #COPRO_NMI_CODE_BYTES
    bne @nmi_copy

    lda #K_NMI_CODE_BASE
    sta RAMVEC_NMI                  ; trampoline now lands in WRAM at $0E00

    sep #$20
    .a8
@nmi_unchanged:

    ; v2.26 Phase 2.5: HIRQ-builder version poll. Same shape as the NMI
    ; poll above. Cart version 0 = no HIRQ active; the install path
    ; copies the staged code into WRAM at K_HIRQ_CODE_BASE and points
    ; RAMVEC_IRQ at it. Uninstall path clears RAMVEC_IRQ to 0 — the
    ; trampoline at $FFEE does `jmp (RAMVEC_IRQ)`, so if a guest later
    ; arms HIRQ with no handler the SNES will spin in a tight loop at
    ; bank $00:$0000 (BRK), which is preferable to silently running a
    ; stale handler.
    .a8
    lda f:COPRO_HIRQ_VERSION_L
    cmp K_HIRQ_VERSION
    beq @hirq_unchanged
    sta K_HIRQ_VERSION
    cmp #0
    bne @hirq_install

    ; v2.29 Phase 3a: UNINSTALL restores RAMVEC_IRQ to the kernel's
    ; default_hirq_handler (cached at K_HIRQ_DEFAULT) instead of
    ; clearing to 0, so the unified layout/siphon ISR keeps working
    ; even after a custom-installed HIRQ uninstalls.
    rep #$20
    .a16
    lda K_HIRQ_DEFAULT
    sta RAMVEC_IRQ
    sep #$20
    .a8
    bra @hirq_unchanged

@hirq_install:
    rep #$30
    .a16
    .i16
    ldx #$0000
@hirq_copy:
    lda f:COPRO_HIRQ_CODE_L,x       ; long,X — 16-bit read from cart
    sta a:K_HIRQ_CODE_BASE,x        ; abs,X  — 16-bit write to WRAM
    inx
    inx
    cpx #COPRO_HIRQ_CODE_BYTES
    bne @hirq_copy

    lda #K_HIRQ_CODE_BASE
    sta RAMVEC_IRQ                  ; trampoline now lands in WRAM at $1200

    sep #$20
    .a8
@hirq_unchanged:

    ; v2.29 Phase 3a: the cart_window-driven schedule programming
    ; (Phase 2.5b) was removed. The NMI handler now drives the unified
    ; HIRQ ISR by reading CW_OFF_KERNEL_LAYOUT/SIPHON_CONFIG at vblank
    ; and programming $4207-$420A + NMITIMEN there. Guest demos use
    ; mg_kernel_layout / mg_siphon_configure instead of the raw
    ; mg_hirq_configure path.

    ; 3) sleep until the next vblank. NMI fires, reads COPRO_DMACTRL, and
    ;    dispatches whichever DMAs the copro requested (or none if it wrote
    ;    $00). After RTI we land back here in active display and loop.
    ;
    ; v2.26: bra @loop became out-of-range (~176 bytes) once both the NMI
    ; and HIRQ version polls landed in the loop body. Use a jmp; the
    ; extra byte costs ~1 master cycle per iteration, negligible.
    wai
    jmp @loop
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
    bne @do_frame
    jmp @out               ; long jump — short branch range was exceeded
                           ; when the PPU register batch was added below
@do_frame:

    ; Reaffirm INIDISP visible at every NMI start so the screen survives
    ; even if HDMA channel 7's source table is malformed or never
    ; touched this frame. Belt-and-suspenders against the bsnes-plus
    ; HDMA repeat-mode discrepancy (see emit_inidisp_table in
    ; copro_mg_state.c). Real-hardware no-op when HDMA writes $0F
    ; anyway; cheap diagnostic safety on emulators.
    lda #$0F
    sta INIDISP

    ; (Real hardware: assert/extend forced blank for the letterbox lines so the
    ;  whole 54-line window is DMA-able -- see the DMA-budget notes. TODO.)

    ; --- apply PPU register batch -------------------------------------
    ; 32 bytes at COPRO_PPU_BATCH the copro filled this frame: BGMODE,
    ; OBSEL, BG1-4SC, BG12NBA, BG34NBA, TM, TS, MOSAIC (single bytes),
    ; then 8 16-bit scrolls. Single-byte regs first, in order.
    .a8
    lda f:PB_BGMODE
    sta BGMODE
    lda f:PB_OBSEL
    sta OBSEL
    lda f:PB_BG1SC
    sta BG1SC
    lda f:PB_BG2SC
    sta BG2SC
    lda f:PB_BG3SC
    sta BG3SC
    lda f:PB_BG4SC
    sta BG4SC
    lda f:PB_BG12NBA
    sta BG12NBA
    lda f:PB_BG34NBA
    sta BG34NBA
    lda f:PB_TM
    sta TM
    lda f:PB_TS
    sta TS
    lda f:PB_MOSAIC
    sta MOSAIC

    ; Scrolls — write-twice 16-bit. The PPU latches low byte first,
    ; then high byte (9-bit value). We read each 16-bit batch entry
    ; and write its low/high to the matching PPU reg.
    lda f:PB_SCROLLS + $00
    sta BG1HOFS
    lda f:PB_SCROLLS + $01
    sta BG1HOFS
    lda f:PB_SCROLLS + $02
    sta BG1VOFS
    lda f:PB_SCROLLS + $03
    sta BG1VOFS
    lda f:PB_SCROLLS + $04
    sta BG2HOFS
    lda f:PB_SCROLLS + $05
    sta BG2HOFS
    lda f:PB_SCROLLS + $06
    sta BG2VOFS
    lda f:PB_SCROLLS + $07
    sta BG2VOFS
    lda f:PB_SCROLLS + $08
    sta BG3HOFS
    lda f:PB_SCROLLS + $09
    sta BG3HOFS
    lda f:PB_SCROLLS + $0A
    sta BG3VOFS
    lda f:PB_SCROLLS + $0B
    sta BG3VOFS
    lda f:PB_SCROLLS + $0C
    sta BG4HOFS
    lda f:PB_SCROLLS + $0D
    sta BG4HOFS
    lda f:PB_SCROLLS + $0E
    sta BG4VOFS
    lda f:PB_SCROLLS + $0F
    sta BG4VOFS

    ; --- Mode 7 batch ------------------------------------------------
    ; Written every frame regardless of BGMODE; harmless when bgmode != 7
    ; and avoids a conditional branch. M7A-D + M7X/Y are write-twice
    ; 8-bit registers — the PPU latches low byte, then high byte gives
    ; the 16-bit value.
    lda f:M7B_SEL
    sta M7SEL

    lda f:M7B_A_LO + 0
    sta M7A
    lda f:M7B_A_LO + 1
    sta M7A

    lda f:M7B_B_LO + 0
    sta M7B
    lda f:M7B_B_LO + 1
    sta M7B

    lda f:M7B_C_LO + 0
    sta M7C
    lda f:M7B_C_LO + 1
    sta M7C

    lda f:M7B_D_LO + 0
    sta M7D
    lda f:M7B_D_LO + 1
    sta M7D

    lda f:M7B_X_LO + 0
    sta M7X
    lda f:M7B_X_LO + 1
    sta M7X

    lda f:M7B_Y_LO + 0
    sta M7Y
    lda f:M7B_Y_LO + 1
    sta M7Y

    ; --- HDMA channels 1..6 setup -------------------------------------
    ; Channel 0 is reserved for the kernel's DMA-list dispatch below.
    ; Channel 7 stays as INIDISP letterbox (set up at boot).
    ; For each channel C in 1..6: if the copro-staged enabled byte at
    ; COPRO_HDMA_CONFIG + C*8 is non-zero, program DMAP_C / BBAD_C /
    ; A1T_C / A1B_C and OR (1 << C) into the HDMAEN accumulator.
    ; Then write HDMAEN once with channel-7 bit pre-set.
    .a8
    sep #$10
    .i8
    ldy #$00                              ; v2.29 Phase 3a: ch7 no longer
                                          ; armed; guest HDMA channels 1-6
                                          ; still OR their bits below

    ; Channel 1
    lda f:COPRO_HDMA_CONFIG + 1*COPRO_HDMA_CONFIG_STRIDE + 0
    beq @hskip_1
    lda f:COPRO_HDMA_CONFIG + 1*COPRO_HDMA_CONFIG_STRIDE + 2
    sta HDMA_CH_BASE + 1*$10 + 0
    lda f:COPRO_HDMA_CONFIG + 1*COPRO_HDMA_CONFIG_STRIDE + 1
    sta HDMA_CH_BASE + 1*$10 + 1
    rep #$20
    .a16
    lda f:COPRO_HDMA_CONFIG + 1*COPRO_HDMA_CONFIG_STRIDE + 4
    sta HDMA_CH_BASE + 1*$10 + 2
    sep #$20
    .a8
    lda #COPRO_BANK
    sta HDMA_CH_BASE + 1*$10 + 4
    tya
    ora #$02
    tay
@hskip_1:

    ; Channel 2
    lda f:COPRO_HDMA_CONFIG + 2*COPRO_HDMA_CONFIG_STRIDE + 0
    beq @hskip_2
    lda f:COPRO_HDMA_CONFIG + 2*COPRO_HDMA_CONFIG_STRIDE + 2
    sta HDMA_CH_BASE + 2*$10 + 0
    lda f:COPRO_HDMA_CONFIG + 2*COPRO_HDMA_CONFIG_STRIDE + 1
    sta HDMA_CH_BASE + 2*$10 + 1
    rep #$20
    .a16
    lda f:COPRO_HDMA_CONFIG + 2*COPRO_HDMA_CONFIG_STRIDE + 4
    sta HDMA_CH_BASE + 2*$10 + 2
    sep #$20
    .a8
    lda #COPRO_BANK
    sta HDMA_CH_BASE + 2*$10 + 4
    tya
    ora #$04
    tay
@hskip_2:

    ; Channel 3
    lda f:COPRO_HDMA_CONFIG + 3*COPRO_HDMA_CONFIG_STRIDE + 0
    beq @hskip_3
    lda f:COPRO_HDMA_CONFIG + 3*COPRO_HDMA_CONFIG_STRIDE + 2
    sta HDMA_CH_BASE + 3*$10 + 0
    lda f:COPRO_HDMA_CONFIG + 3*COPRO_HDMA_CONFIG_STRIDE + 1
    sta HDMA_CH_BASE + 3*$10 + 1
    rep #$20
    .a16
    lda f:COPRO_HDMA_CONFIG + 3*COPRO_HDMA_CONFIG_STRIDE + 4
    sta HDMA_CH_BASE + 3*$10 + 2
    sep #$20
    .a8
    lda #COPRO_BANK
    sta HDMA_CH_BASE + 3*$10 + 4
    tya
    ora #$08
    tay
@hskip_3:

    ; Channel 4
    lda f:COPRO_HDMA_CONFIG + 4*COPRO_HDMA_CONFIG_STRIDE + 0
    beq @hskip_4
    lda f:COPRO_HDMA_CONFIG + 4*COPRO_HDMA_CONFIG_STRIDE + 2
    sta HDMA_CH_BASE + 4*$10 + 0
    lda f:COPRO_HDMA_CONFIG + 4*COPRO_HDMA_CONFIG_STRIDE + 1
    sta HDMA_CH_BASE + 4*$10 + 1
    rep #$20
    .a16
    lda f:COPRO_HDMA_CONFIG + 4*COPRO_HDMA_CONFIG_STRIDE + 4
    sta HDMA_CH_BASE + 4*$10 + 2
    sep #$20
    .a8
    lda #COPRO_BANK
    sta HDMA_CH_BASE + 4*$10 + 4
    tya
    ora #$10
    tay
@hskip_4:

    ; Channel 5
    lda f:COPRO_HDMA_CONFIG + 5*COPRO_HDMA_CONFIG_STRIDE + 0
    beq @hskip_5
    lda f:COPRO_HDMA_CONFIG + 5*COPRO_HDMA_CONFIG_STRIDE + 2
    sta HDMA_CH_BASE + 5*$10 + 0
    lda f:COPRO_HDMA_CONFIG + 5*COPRO_HDMA_CONFIG_STRIDE + 1
    sta HDMA_CH_BASE + 5*$10 + 1
    rep #$20
    .a16
    lda f:COPRO_HDMA_CONFIG + 5*COPRO_HDMA_CONFIG_STRIDE + 4
    sta HDMA_CH_BASE + 5*$10 + 2
    sep #$20
    .a8
    lda #COPRO_BANK
    sta HDMA_CH_BASE + 5*$10 + 4
    tya
    ora #$20
    tay
@hskip_5:

    ; Channel 6
    lda f:COPRO_HDMA_CONFIG + 6*COPRO_HDMA_CONFIG_STRIDE + 0
    beq @hskip_6
    lda f:COPRO_HDMA_CONFIG + 6*COPRO_HDMA_CONFIG_STRIDE + 2
    sta HDMA_CH_BASE + 6*$10 + 0
    lda f:COPRO_HDMA_CONFIG + 6*COPRO_HDMA_CONFIG_STRIDE + 1
    sta HDMA_CH_BASE + 6*$10 + 1
    rep #$20
    .a16
    lda f:COPRO_HDMA_CONFIG + 6*COPRO_HDMA_CONFIG_STRIDE + 4
    sta HDMA_CH_BASE + 6*$10 + 2
    sep #$20
    .a8
    lda #COPRO_BANK
    sta HDMA_CH_BASE + 6*$10 + 4
    tya
    ora #$40
    tay
@hskip_6:

    tya
    sta HDMAEN                            ; channels enabled this frame

    rep #$10                              ; restore 16-bit X for DMA walk
    .i16

    ; Force-blank the PPU around the DMA walk so VRAM writes always
    ; land in VRAM regardless of whether the slot list overruns vblank.
    ; Without this, any DMA executed during active display has its
    ; VMDATAL/VMDATAH writes silently dropped by the PPU -- the failure
    ; mode behind demo_audio_mixer's "screen stays backdrop red, BG1
    ; tilemap never updates" symptom (see v1.75 instrumentation that
    ; confirmed bsnes-plus's DMA hardware reads the source bytes but
    ; the VRAM writes never stick). One-shot black flash on demos
    ; whose first-frame DMA list overruns (clean_slate's 24ms VRAM
    ; clear is the only practical case); steady-state DMAs are short
    ; enough that the force-blank window closes inside vblank and the
    ; user never sees it.
    lda #$80
    sta INIDISP

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
    bne :+
    jmp @done
:

    lda f:COPRO_DMA_LIST_L+0,x    ; bbus (0 => empty slot, skip)
    bne :+
    jmp @next
:
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
    beq :+
    jmp @check_v
:
    lda f:COPRO_DMA_LIST_L+6,x
    sta CGADD
    jmp @fire
@check_v:
    cmp #<VMDATAL           ; $18 -> set VMAIN word-step, write VMADDL/H
    beq :+
    jmp @check_o
:
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda f:COPRO_DMA_LIST_L+6,x
    sta VMADDL
    sep #$20
    .a8
    jmp @fire
@check_o:
    cmp #<OAMDATA           ; $04 -> write OAMADDL/H
    beq :+
    jmp @fire
:
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
    jmp @slot

@done:
    ; Un-blank the PPU now that all VRAM/CGRAM/OAM writes have landed.
    ; The HDMA letterbox channel (if armed) will re-write INIDISP per
    ; scanline starting next frame; this $0F is what the screen shows
    ; for the rest of the visible frame.
    lda #$0F
    sta INIDISP

    ; v2.29 Phase 3a: program unified HIRQ for the next frame's
    ; letterbox / siphon. Reads cart_window layout config, caches
    ; into WRAM (default_hirq_handler reads from there to avoid
    ; per-fire cart accesses), then sets V counter target and
    ; NMITIMEN bits. Cases:
    ;
    ;   top_lb > 0          : V target = top_lb, INIDISP forced to $80
    ;                          (so the top_lb lines stay force-blanked
    ;                          until the ISR fires the visible
    ;                          transition). NMITIMEN = $A0.
    ;   top_lb = 0, has_lower: V target = visible_end (= 224 - bot_lb).
    ;                          INIDISP stays $0F (visible from line 0).
    ;                          NMITIMEN = $A0.
    ;   all zero            : no transitions needed. NMITIMEN = $80
    ;                          (just NMI, IRQ disabled).
    .i8
    sep #$10

    ; v2.30.9: read top_lb + bot_lb in a SINGLE 16-bit load. Pairs
    ; with the host's cart_window_store_u16_le write so we never see
    ; a torn (new-top, old-bottom) pair — the source of the
    ; occasional dynamic_letterbox flicker. K_LAYOUT_TOP_LB and
    ; K_LAYOUT_BOT_LB are adjacent in WRAM ($0230, $0231), so the
    ; matching 16-bit sta stores both bytes in one instruction.
    rep #$20
    .a16
    lda f:COPRO_LAYOUT_TOP_LB_L
    sta K_LAYOUT_TOP_LB
    sep #$20
    .a8
    lda f:COPRO_SIPHON_BYTES_L
    sta K_SIPHON_BYTES

    ; Compute visible_end = 224 - bot_lb
    lda #224
    sec
    sbc K_LAYOUT_BOT_LB
    sta K_LAYOUT_VIS_END

    lda K_LAYOUT_TOP_LB
    beq @nmi_no_top_lb

    ; Has top letterbox: state 0 (VISIBLE_START), V target = top_lb,
    ; INIDISP = $80. ISR at line top_lb will unblank.
    stz K_FRAME_STATE       ; state 0 = VISIBLE_START
    sta VTIMEL
    stz VTIMEH
    stz HTIMEL
    stz HTIMEH
    lda #$80
    sta INIDISP
    lda #$A0                ; NMI + VIRQ
    sta NMITIMEN
    bra @nmi_hirq_done

@nmi_no_top_lb:
    ; No top letterbox. Skip directly to state 1 (VISIBLE_END) so the
    ; ISR's first fire at visible_end re-blanks for the bottom region.
    ; If bot_lb=0 too AND no siphon, no IRQ needed at all.
    lda K_LAYOUT_BOT_LB
    ora K_SIPHON_BYTES
    beq @nmi_no_hirq
    lda #1                  ; state 1 = VISIBLE_END
    sta K_FRAME_STATE
    lda K_LAYOUT_VIS_END
    sta VTIMEL
    stz VTIMEH
    stz HTIMEL
    stz HTIMEH
    lda #$A0
    sta NMITIMEN
    bra @nmi_hirq_done

@nmi_no_hirq:
    lda #$80
    sta NMITIMEN

@nmi_hirq_done:
    rep #$10                ; restore 16-bit X for the epilogue

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

; ------------------------------------------------------------------
; v2.29 Phase 3a: default HIRQ handler.
;
; State-machine dispatcher driven by K_FRAME_STATE (cached in WRAM
; at vblank by the NMI handler). Handles letterbox transitions
; (INIDISP toggling at top_lb and visible_end) using the cached
; K_LAYOUT_TOP_LB / K_LAYOUT_VIS_END values. Siphon support stubbed
; for Phase 3a — a future iteration extends the SIPHON state with
; per-fire CPU DMA.
;
; State 0 (VISIBLE_START): fired at line top_lb. Set INIDISP=$0F.
;                          If bot_lb>0, advance to state 1 with V
;                          target = visible_end. Else state 2 (done).
; State 1 (VISIBLE_END):   fired at line visible_end (= 224 - bot_lb).
;                          Set INIDISP=$80. Move to state 2.
; State 2 (DORMANT):       NMI hasn't reset us yet — should not fire,
;                          but if it does, just ack and exit.
;
; Cycle cost per fire (worst — state 0 with bot_lb>0):
;   IRQ entry 7 + PHP/SEP/PHA 30 + state dispatch 25 + INIDISP write
;   25 + V target update 35 + state update 20 + $4211 ack 12 + PLA/PLP/
;   RTI 50 = ~204 master cycles, well within the 1364-master scanline.
; ------------------------------------------------------------------
.proc default_hirq_handler
    .a8
    .i8
    php                         ; save P (carries M/X flags)
    sep #$20                    ; force M=8 for 1-byte ops
    pha

    lda K_FRAME_STATE
    cmp #1
    beq @state_visible_end
    cmp #2
    beq @state_dormant

    ; --- State 0: VISIBLE_START — unblank, schedule next event ---
    lda #$0F
    sta INIDISP
    ; If bot_lb > 0, next event = visible_end (transition back to blank)
    lda K_LAYOUT_BOT_LB
    beq @vs_no_bot_lb
    lda K_LAYOUT_VIS_END
    sta VTIMEL
    stz VTIMEH
    lda #1                      ; state 1 = waiting on VISIBLE_END
    sta K_FRAME_STATE
    bra @hirq_ack

@vs_no_bot_lb:
    ; No bottom letterbox — nothing more to do this frame.
    lda #2
    sta K_FRAME_STATE
    bra @hirq_ack

@state_visible_end:
    ; --- State 1: VISIBLE_END — force-blank the bottom region ---
    lda #$80
    sta INIDISP
    lda #2
    sta K_FRAME_STATE
    ; (Could also set V target to vblank to stop firing this frame;
    ; for now leave VTIMEL at visible_end — next IRQ won't fire until
    ; V wraps and matches again, which won't happen this frame.)
    bra @hirq_ack

@state_dormant:
    ; --- State 2: DORMANT — shouldn't fire, but ack defensively ---

@hirq_ack:
    lda $4211                   ; ack IRQ (read TIMEUP)
    pla
    plp
    rti
.endproc
