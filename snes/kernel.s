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

    ; v2.34 virtual-NMI: NMI is DISABLED. A single self-chaining H+V
    ; timer IRQ (proc `irq`) does all per-frame work — state A blanks +
    ; runs the DMA burst at the bottom-letterbox line, state B unblanks
    ; at the top-letterbox line. Point the IRQ vector at it; park the
    ; unused NMI vector at a bare RTI so a stray NMI is harmless.
    rep #$20
    .a16
    lda #.loword(irq)
    sta RAMVEC_IRQ
    lda #.loword(nmi_stub)
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

    ; v2.29 Phase 3a: HDMA channel 7 (INIDISP letterbox via cart_window
    ; table) is no longer enabled — the unified default_hirq_handler
    ; ISR drives INIDISP transitions directly. The emit_inidisp_table
    ; machinery still runs (for back-compat with mg_force_blank) but
    ; its table is now dormant since channel 7 isn't armed. Phase 3b
    ; will rip out emit_inidisp_table and the table region's 256 bytes
    ; for reuse.
    stz HDMAEN              ; no HDMA channels armed at boot

    ; Force NON-INTERLACE. The virtual-NMI design pins fixed H+V IRQ
    ; targets each frame; interlace (262/263-line field alternation)
    ; would drift them every other frame. Cold-boot default is already
    ; 0, but assert it defensively since the kernel never wrote $2133.
    stz SETINI

    ; --- per-frame state (cached by @loop, consumed by the IRQ) ---
    stz K_FRAME_STATE       ; 0 = state A (blank + burst)
    stz K_FRAME_READY       ; no staged frame yet
    stz K_SLOT_CURSOR       ; chainer starts at slot 0
    stz K_LAYOUT_TOP_LB
    stz K_LAYOUT_BOT_LB
    lda #224
    sta K_LAYOUT_VIS_END    ; 224 - bot_lb (bot_lb = 0 at boot)
    stz K_SIPHON_BYTES

    ; --- arm the self-chaining H+V IRQ: state A at V=VIS_END, H=22 ---
    ; H+V mode (NMITIMEN bits 4+5) fires once per frame at an exact
    ; (V,H). NMI (b7) OFF; auto-joypad (b0) OFF — the kernel bit-bangs
    ; pads in active display (see read_joypads).
    lda #22
    sta HTIMEL
    stz HTIMEH
    lda K_LAYOUT_VIS_END
    sta VTIMEL
    stz VTIMEH
    lda #$0F
    sta INIDISP             ; visible until the first burst
    lda #$30
    sta NMITIMEN

    cli

@loop:
    ; @spin just exited, which means the NMI has finished the vblank DMA burst
    ; and we are at the very start of active display. The CPU is idle for the
    ; whole active period; we use a tiny piece of it (~150 cycles) to bit-bang
    ; the joypads and forward all four to the copro before settling in to
    ; wait for the next vblank.
    sep #$10
    .i8

    ; v2.34 virtual-NMI: there are TWO IRQ events per frame (state A =
    ; blank+burst, state B = unblank); the loop wakes on BOTH. Run the
    ; per-frame handshake (joypad post + frame-ready + layout reads)
    ; ONLY before the state-A burst. K_FRAME_STATE holds the NEXT IRQ's
    ; state (0 = A). Doing it on both events would double-count the
    ; host's $7800/port-7 sub-frame handshake — advancing a sub-frame
    ; right before a state-B IRQ that does no walk, so that sub-frame's
    ; DMA is skipped (visible as checker/partial CHR).
    sep #$20
    .a8
    lda K_FRAME_STATE
    bne @wait_only                  ; state B coming (unblank only) -> wait

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

    ; v2.34 virtual-NMI: cache the per-frame cart-window state HERE, in
    ; active display (CPU-idle time — slow cart reads are free), so the
    ; time-critical IRQ touches only cheap cached WRAM values. This is
    ; also where a future per-scanline siphon source would be cached.
    ;
    ; ORDER MATTERS: the $7800 frame-ready read must stay AFTER the
    ; port-7 ($7700) joypad post above. The host gates sub-frame advance
    ; on the port-7 read seeing a $7800 read since the last commit, so
    ; the sequence (port-7, then $7800, then wai->burst) preserves the
    ; existing handshake with zero host changes.
    .a8
    lda f:COPRO_FRAME_RDY_L
    sta K_FRAME_READY

    ; Cache letterbox layout; recompute VIS_END = 224 - bot_lb.
    lda f:COPRO_LAYOUT_TOP_LB_L
    sta K_LAYOUT_TOP_LB
    lda f:COPRO_LAYOUT_BOT_LB_L
    sta K_LAYOUT_BOT_LB
    lda #224
    sec
    sbc K_LAYOUT_BOT_LB
    sta K_LAYOUT_VIS_END

    ; Sleep until the next H+V IRQ event (state A blank+burst at VIS_END,
    ; or state B unblank at top_lb). The IRQ does all per-frame PPU work;
    ; we resume here in active display afterward.
@wait_only:
    wai
    jmp @loop
.endproc

; ------------------------------------------------------------------
; frame_dma — apply the copro's staged per-frame PPU work: register
; batch, Mode-7 batch, HDMA ch1-6 arm, then the DMA-list walk.
;
; Called from the `irq` state-A handler, which has ALREADY force-
; blanked the screen and confirmed a staged frame (K_FRAME_READY != 0),
; and saved A/X/Y. Touches no INIDISP — the irq state machine owns the
; blank/unblank transitions. Entry/exit: A8, I16. DBR=$00.
; ------------------------------------------------------------------
.proc frame_dma
    .a8
    .i16

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

    ; --- Mode 7 batch (skipped unless BGMODE selects mode 7) ----------
    ; v2.36: gated on bgmode==7 so non-Mode-7 demos (the FMV is Mode 1)
    ; hand ~13 register writes of the force-blank DMA window back to the
    ; chainer every burst — those M7 matrix writes are dead anyway when
    ; bgmode!=7 (the PPU ignores M7A-D/M7X/Y outside Mode 7). Mode-7
    ; demos (bgmode==7) take the same path as before. M7A-D + M7X/Y are
    ; write-twice 8-bit (PPU latches low byte, then high byte).
    lda f:PB_BGMODE
    and #$07
    cmp #$07
    bne @skip_m7
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
@skip_m7:

    ; --- HDMA channels 1..6 setup -------------------------------------
    ; Channel 0 is reserved for the DMA-list dispatch below. Channel 7
    ; (INIDISP letterbox) is NO LONGER armed — the v2.34 irq state
    ; machine drives INIDISP blank/unblank directly, so the HDMA INIDISP
    ; table is retired (it would otherwise fight the irq for INIDISP).
    ; For each channel C in 1..6: if the copro-staged enabled byte at
    ; COPRO_HDMA_CONFIG + C*8 is non-zero, program DMAP_C / BBAD_C /
    ; A1T_C / A1B_C and OR (1 << C) into the HDMAEN accumulator.
    .a8
    sep #$10
    .i8

    ldy #$00                              ; HDMAEN accumulator (no ch7)

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

    ; --- cycle-budgeted DMA-list chainer ------------------------------
    ; Walk staged slots from the persistent cursor K_SLOT_CURSOR. Before
    ; each slot, read the LIVE beam position (calc_bytes_rem) and fire
    ; only if the remaining blank window can still sink the slot; else
    ; DEFER (leave the cursor) so the next burst resumes here. A bbus of
    ; 0 marks the end of the staged (contiguous) list. When the last
    ; slot is walked, reset the cursor and strobe COPRO_FRAME_DONE so the
    ; host bumps frame_consumed + clears frame_ready.
    ;
    ; Y counts slots fired THIS burst: an oversized FIRST slot is fired
    ; anyway (the host must chunk uploads <= one window; this just keeps
    ; an over-budget slot from hanging the chainer forever).
    rep #$30
    .a16
    .i16
    lda K_SLOT_CURSOR
    and #$00FF
    asl a
    asl a
    asl a                        ; X = cursor * 8 (entry size)
    tax
    ldy #$0000                   ; slots fired this burst
    sep #$20
    .a8
@slot:
    cpx #(8 * 8)                 ; walked all 8 slots? -> frame complete
    bne :+
    jmp @frame_complete
:
    lda f:COPRO_DMA_LIST_L+0,x    ; bbus (0 => empty = end of list)
    bne :+
    jmp @frame_complete
:
    ; --- budget check ---
    ; v2.36: read the live beam ONLY on the first slot fired this burst;
    ; subsequent slots use a running K_BYTES_REM that @fire decrements by
    ; each slot's bytes. Re-reading the beam (SLHV/OPVCT + the *170
    ; multiply) before EVERY slot was itself the per-slot DMA-setup
    ; overhead that pushed 240x208/20fps sub-frames a hair over the thin
    ; ~92 B margin -> defer whole slot -> waste the rest of the burst.
    pha                          ; save bbus (A8)
    cpy #$0000                   ; first slot fired this burst?
    bne @have_budget             ; no -> K_BYTES_REM is the running value
    jsr calc_bytes_rem           ; yes -> read live beam (accurate start); A8 out
@have_budget:
    rep #$20
    .a16
    lda f:COPRO_DMA_LIST_L+4,x    ; slot byte count
    cmp K_BYTES_REM
    sep #$20
    .a8
    bcc @fits                    ; size <  window remaining -> fits
    beq @fits                    ; size == window remaining -> fits
    cpy #$0000                   ; size > remaining: defer UNLESS nothing
    beq @fits                    ;   fired yet (avoid hang on a huge slot)
    pla                          ; discard saved bbus
    jmp @defer
@fits:
    pla                          ; restore bbus (A8)
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
    iny                     ; count a fired slot
    ; v2.36: decrement the running window budget by this slot's bytes +
    ; a small fudge for inter-slot setup time (so the next slot needn't
    ; re-read the beam). Clamp to 0 on underflow so an exhausted budget
    ; correctly defers the next slot instead of wrapping to ~64 KB.
    rep #$20
    .a16
    lda K_BYTES_REM
    sec
    sbc f:COPRO_DMA_LIST_L+4,x    ; - slot byte count
    bcc @budget_zero
    sec
    sbc #24                       ; - inter-slot setup fudge
    bcc @budget_zero
    sta K_BYTES_REM
    bra @budget_done
@budget_zero:
    stz K_BYTES_REM
@budget_done:
    sep #$20
    .a8

@next:
    inc K_SLOT_CURSOR            ; advance cursor + X to the next slot
    .repeat 8
    inx
    .endrepeat
    jmp @slot

@defer:
    ; Window full — leave K_SLOT_CURSOR at the un-fired slot; the next
    ; burst resumes the walk from here. frame_ready stays set (no strobe).
    rts

@frame_complete:
    stz K_SLOT_CURSOR            ; ready for the next frame's staged list
    lda f:COPRO_FRAME_DONE_L     ; strobe: host bumps frame_consumed
    rts
.endproc

; ------------------------------------------------------------------
; calc_bytes_rem — read the live 9-bit V counter and set K_BYTES_REM
; to a CONSERVATIVE estimate of how many DMA bytes the blank window can
; still sink before the unblank deadline (V=top_lb of the next frame).
;
;   lines_rem = V in [vis_end,261]: (262 + top_lb) - V
;               V in [0, top_lb]   : top_lb - V
;               V in (top_lb,vis_end): 0  (visible region — never fire)
;   K_BYTES_REM = lines_rem * 160        (160 < the ~170 B/line real
;                                         rate, so we under-estimate =
;                                         defer slightly early = safe)
; Uses cached K_LAYOUT_VIS_END / K_LAYOUT_TOP_LB. Entry/exit A8; touches
; A only (X/Y preserved). DBR=$00.
; ------------------------------------------------------------------
.proc calc_bytes_rem
    .a8
    lda SLHV                     ; latch H/V counters
    lda STAT78                   ; reset OPHCT/OPVCT read toggle
    lda OPVCT                    ; V low byte
    sta K_VLO
    lda OPVCT                    ; V high byte
    and #$01                     ; bit0 = V bit8
    sta K_VHI

    rep #$20
    .a16
    lda K_LAYOUT_VIS_END
    and #$00FF
    sta K_BYTES_REM              ; scratch = vis_end
    lda K_VLO                    ; V (16-bit: K_VLO | K_VHI<<8)
    cmp K_BYTES_REM
    bcc @below                   ; V < vis_end

    ; V >= vis_end: lines = (262 + top_lb) - V
    sta K_BYTES_REM              ; scratch = V
    lda K_LAYOUT_TOP_LB
    and #$00FF
    clc
    adc #262
    sec
    sbc K_BYTES_REM              ; (262 + top_lb) - V
    bra @lines_done

@below:
    ; V < vis_end. Is V <= top_lb (next-frame top region) or visible?
    sta K_BYTES_REM              ; scratch = V
    lda K_LAYOUT_TOP_LB
    and #$00FF
    cmp K_BYTES_REM              ; top_lb vs V
    bcc @visible                 ; top_lb < V -> visible region
    sec
    sbc K_BYTES_REM              ; top_lb - V
    bra @lines_done

@visible:
    lda #0

@lines_done:
    ; A = lines_rem. bytes = lines * 170 (the real ~170.5 B/line for the
    ; force-blank window: 54 lines * 1364 master-cyc / 8 = 9207 B). Raised
    ; from the old conservative 160 so three ~9 KB sub-frames each fit ONE
    ; burst at FB(8,8) -> 20 fps. Overrun safety is the per-slot live-V
    ; re-check above + the host's ~120 B/sub-frame margin (sub-frames are
    ; sized < window). 170 = 128 + 32 + 8 + 2.
    pha                          ; preserve lines across the *160 build
    asl a
    asl a
    asl a
    asl a
    asl a                        ; A = lines * 32
    sta K_BYTES_REM
    asl a
    asl a                        ; A = lines * 128
    clc
    adc K_BYTES_REM              ; + lines*32 = lines*160
    sta K_BYTES_REM
    pla                          ; lines
    asl a                        ; lines * 2
    sta K_VLO                    ; stash (K_VLO/K_VHI free — V already used)
    asl a
    asl a                        ; lines * 8
    clc
    adc K_VLO                    ; + lines*2 = lines*10
    clc
    adc K_BYTES_REM              ; + lines*160 = lines*170
    sta K_BYTES_REM
    sep #$20
    .a8
    rts
.endproc

; ------------------------------------------------------------------
; irq — the virtual-NMI handler. Self-chaining H+V timer IRQ with two
; states (cached in K_FRAME_STATE):
;
;   State A (0) — fires at V=VIS_END (bottom-letterbox blank line):
;       force-blank; if a frame is staged (K_FRAME_READY != 0) run
;       frame_dma (PPU batch + HDMA ch1-6 + DMA-list walk); then
;       schedule the unblank at V=top_lb and advance to state B.
;
;   State B (1) — fires at V=top_lb (visible-region start):
;       unblank; schedule the next blank+burst at V=VIS_END; → state A.
;
; H+V mode (NMITIMEN=$30) is armed once at init and never changed here;
; only the V target (VTIMEL/H) is reprogrammed per event. HTIME stays
; 22. The screen stays force-blanked from VIS_END through vblank and
; the top letterbox — the full DMA-able window — and unblanks at top_lb.
; ------------------------------------------------------------------
.proc irq
    rep #$30
    .a16
    .i16
    pha
    phx
    phy
    sep #$20
    .a8

    lda K_FRAME_STATE
    bne @state_b

    ; ===== State A: blank, then (if staged) burst =====
    lda #$80
    sta INIDISP
    lda K_FRAME_READY
    beq @a_schedule         ; no staged frame -> keep letterbox, skip DMA
    jsr frame_dma           ; PPU batch + HDMA ch1-6 + DMA-list walk
    sep #$20                ; frame_dma is A8 on return, but be explicit
    .a8
@a_schedule:
    ; schedule the unblank at V = top_lb (state B)
    lda K_LAYOUT_TOP_LB
    sta VTIMEL
    stz VTIMEH
    lda #$01
    sta K_FRAME_STATE       ; -> state B
    bra @ack

@state_b:
    ; ===== State B: unblank for the visible region =====
    lda #$0F
    sta INIDISP
    lda K_LAYOUT_VIS_END
    sta VTIMEL
    stz VTIMEH
    stz K_FRAME_STATE       ; -> state A

@ack:
    lda TIMEUP              ; ack the timer IRQ (read $4211)
    rep #$30
    .a16
    .i16
    ply
    plx
    pla
    rti
.endproc

; ------------------------------------------------------------------
; nmi_stub — NMI is disabled (NMITIMEN bit7 = 0); park RAMVEC_NMI at a
; bare RTI so a stray/edge NMI is harmless rather than a wild jump.
; ------------------------------------------------------------------
.proc nmi_stub
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

; default_hirq_handler removed in v2.34 — the unified `irq` proc above
; now owns the letterbox INIDISP transitions (state A/B) directly.
