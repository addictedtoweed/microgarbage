; ============================================================
;  vchain_test.s — validate the runtime cycle-budgeted DMA chainer.
;
;  vnmi_test proved a single monolithic burst lands in the blank
;  window. This proves the REAL chainer: a multi-slot DMA list, too
;  big for one window, fired GREEDILY with a live per-slot bounds
;  check against the actual V-counter, deferring the tail to the next
;  burst and resuming where it left off — so the whole payload lands
;  across several bursts with no truncation and no lost slots.
;
;  This is the "sanity check while maximalizing PPU DMA" design: the
;  chainer reads the real beam position before each MDMAEN and only
;  fires if enough blank scanlines remain for that slot (+ jitter pad).
;  A static byte budget can be wrong; the beam never lies.
;
;  Architecture (same as vnmi_test: FB 16,16, NMI off, H+V IRQ):
;    State A @ V=208 (224-bot_lb): force-blank, then run the chainer:
;        loop:
;          if slot_index == NSLOTS: slot_index = 0; stop (cycle done)
;          read V -> lines_remaining to the unblank deadline (V=16)
;          if lines_remaining < slot_cost[slot_index] + PAD: stop (defer)
;          else: fire slot DMA; slot_index++
;        set next IRQ = unblank @ V=16, flip to B.
;    State B @ V=16 (top_lb): unblank, set next IRQ = V=208, flip to A.
;
;  Payload: 24960 B CHR (780 tiles, 6 colour bands of 130) split into
;  12 uniform slots of 2080 B (= 13 scanlines each). 12*13 = 156 lines
;  >> one ~70-line window, so the chainer MUST defer across ~3 bursts.
;  slot_index persists across bursts; resets after the 12th. CHR is
;  black-cleared at boot so any slot that never fires shows BLACK.
;
;    PASS -> full 6-band rainbow (red/green/yellow/blue/magenta/cyan),
;            clean 2-row letterbox top+bottom, rock steady. Every slot
;            lands across the multi-burst cycle; the live bounds check
;            never overruns and never drops a slot.
;    FAIL (over-run / lenient check) -> a band shows tearing/garbage.
;    FAIL (over-defer / strict check) -> bottom bands stay BLACK.
;
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"

; Registers not in snes.inc:
SETINI  = $2133        ; screen mode / interlace (bit0 = interlace)
SLHV    = $2137        ; software latch of H/V counters (read to latch)
OPVCT   = $213D        ; V counter (read low then high; bit8 in high)
STAT78  = $213F        ; PPU2 status (read resets OPHCT/OPVCT toggle)
TIMEUP  = $4211        ; read to acknowledge H/V-timer IRQ

TOP_LB        = 16
BOT_LB        = 16
VIS_END       = 224 - BOT_LB        ; 208
DEADLINE_V    = TOP_LB              ; 16 — unblank line (next frame)
PAD_LINES     = 3                   ; jitter / IRQ-latency / inter-slot slack

NSLOTS        = 12
SLOT_BYTES    = 2080                ; 65 tiles
SLOT_LINES    = 13                  ; ceil(2080*8 / 1364)

TILEMAP_WORD  = $7C00
TILEMAP_BYTES = 32 * 32 * 2
PAL_BYTES     = 16 * 2

; Direct-page state
state      = $00                    ; byte: 0 = A (blank+chain), 1 = B
slot_index = $02                    ; 16-bit: next slot to fire (0..NSLOTS)
v_lo       = $04                    ; V counter assembly (v_lo/v_hi adjacent
v_hi       = $05                    ;   so a 16-bit load reads both)
lines_rem  = $06                    ; 16-bit scanlines until deadline

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
    plb                             ; DBR = $00

    lda #$80
    sta INIDISP
    stz SETINI                      ; force NON-INTERLACE

    lda #$01
    sta BGMODE
    lda #$7C
    sta BG1SC
    stz BG12NBA
    lda #$01
    sta TM
    stz A1B0

    ; palette
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
    sta VMAIN

    ; clear CHR (1024 tiles * 32 B) via fixed-source zero DMA
    rep #$20
    .a16
    stz VMADDL
    lda #.loword(zero_word)
    sta A1T0L
    lda #$8000
    sta DAS0L
    sep #$20
    .a8
    lda #$09                        ; pattern 1 + fixed source (bit3)
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; tilemap (cell N -> tile N, scan order)
    rep #$20
    .a16
    lda #TILEMAP_WORD
    sta VMADDL
    sep #$20
    .a8
    lda #$01
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

    ; chainer state
    stz state                       ; A8: clears state byte
    rep #$20
    .a16
    stz slot_index                  ; clear full 16-bit
    sep #$20
    .a8

    ; arm H+V IRQ @ (V=208, H=22), NMI off
    lda #<VIS_END
    sta VTIMEL
    lda #>VIS_END
    sta VTIMEH
    lda #22
    sta HTIMEL
    stz HTIMEH

    lda #$0F
    sta INIDISP
    lda #$30                        ; NMI off, H+V IRQ on (bits 4+5)
    sta NMITIMEN
    cli

@idle:
    wai
    bra @idle
.endproc

; ------------------------------------------------------------------
;  read_vcounter — latch + read the 9-bit V counter. Entry A8; returns
;  A16 = V (also stored in v_lo/v_hi). DBR=$00.
; ------------------------------------------------------------------
.proc read_vcounter
    .a8
    lda SLHV                        ; latch H/V counters
    lda STAT78                      ; reset OPHCT/OPVCT read toggle
    lda OPVCT                       ; V low byte
    sta v_lo
    lda OPVCT                       ; V high byte
    and #$01                        ; bit0 = V bit8
    sta v_hi
    rep #$20
    .a16
    lda v_lo                        ; 16-bit: v_lo | v_hi<<8
    rts
.endproc

; ------------------------------------------------------------------
;  compute_lines_rem — A16 = V in; sets lines_rem = scanlines until the
;  unblank deadline (V=16 next frame). Returns A16. DBR=$00.
;     V in [208,261]: 278 - V
;     V in [0,16]   : 16 - V
;     V in [17,207] : 0   (visible region — never fire)
; ------------------------------------------------------------------
.proc compute_lines_rem
    .a16
    cmp #208
    bcs @late
    cmp #17
    bcc @early
    stz lines_rem                   ; visible region
    rts
@late:
    sta lines_rem                   ; temp = V
    lda #278
    sec
    sbc lines_rem
    sta lines_rem
    rts
@early:
    sta lines_rem                   ; temp = V
    lda #DEADLINE_V
    sec
    sbc lines_rem
    sta lines_rem
    rts
.endproc

; ------------------------------------------------------------------
;  fire_slot — program + fire one DMA. Entry A8/I16, X = slot_index*2
;  (word-table index). Caller asserted forced blank. Returns A8.
; ------------------------------------------------------------------
.proc fire_slot
    .a8
    .i16
    rep #$20
    .a16
    lda slot_vram,x
    sta VMADDL
    lda slot_src,x
    sta A1T0L
    lda slot_size,x
    sta DAS0L
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    lda #$01                        ; 2 regs ascending
    sta DMAP0
    lda #<VMDATAL
    sta BBAD0
    lda #$01
    sta MDMAEN                      ; CPU paused for this slot's transfer
    rts
.endproc

; ------------------------------------------------------------------
;  irq_handler — self-chaining two-state machine + chainer.
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

    ; ===== State A: blank, then run the chainer =====
    lda #$80
    sta INIDISP

@chain_loop:
    rep #$20
    .a16
    lda slot_index
    cmp #NSLOTS
    bcc @have_slots
    stz slot_index                  ; cycle complete -> restart next burst
    bra @chain_done
@have_slots:
    sep #$20
    .a8
    jsr read_vcounter               ; -> A16 = V
    jsr compute_lines_rem           ; -> lines_rem (A16)

    rep #$30
    .a16
    .i16
    ldx slot_index                  ; byte-table index
    lda slot_cost,x
    and #$00FF                      ; isolate the cost byte
    clc
    adc #PAD_LINES                  ; required = cost + pad
    cmp lines_rem
    beq @fits                       ; required == remaining -> ok
    bcs @defer                      ; required >  remaining -> defer
@fits:
    lda slot_index
    asl a                           ; *2 for word tables
    tax
    sep #$20
    .a8
    jsr fire_slot
    rep #$20
    .a16
    inc slot_index
    sep #$20
    .a8
    bra @chain_loop

@defer:
@chain_done:
    sep #$20
    .a8
    lda #<DEADLINE_V
    sta VTIMEL
    lda #>DEADLINE_V
    sta VTIMEH
    lda #$01
    sta state                       ; -> state B
    bra @ack

@state_b:
    ; ===== State B: unblank for the visible region =====
    lda #$0F
    sta INIDISP
    lda #<VIS_END
    sta VTIMEL
    lda #>VIS_END
    sta VTIMEH
    stz state                       ; -> state A

@ack:
    lda TIMEUP                      ; ack IRQ
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

; Per-slot tables (NSLOTS entries). Uniform 2080 B / 13 lines; slot i
; targets VRAM word i*1040, source chr_data + i*2080.
slot_vram:
    .repeat NSLOTS, i
        .word i * (SLOT_BYTES / 2)
    .endrepeat
slot_src:
    .repeat NSLOTS, i
        .word .loword(chr_data + i * SLOT_BYTES)
    .endrepeat
slot_size:
    .repeat NSLOTS
        .word SLOT_BYTES
    .endrepeat
slot_cost:
    .repeat NSLOTS
        .byte SLOT_LINES
    .endrepeat

.macro solid_tile p0, p1, p2, p3
    .repeat 8
        .byte p0, p1
    .endrepeat
    .repeat 8
        .byte p2, p3
    .endrepeat
.endmacro

.macro cband p0, p1, p2, p3
    .repeat 130
        solid_tile p0, p1, p2, p3
    .endrepeat
.endmacro

; 6 bands x 130 tiles = 780 tiles = 24960 B.
chr_data:
    cband $FF, $00, $00, $00        ; idx 1 red
    cband $00, $FF, $00, $00        ; idx 2 green
    cband $FF, $FF, $00, $00        ; idx 3 yellow
    cband $00, $00, $FF, $00        ; idx 4 blue
    cband $FF, $00, $FF, $00        ; idx 5 magenta
    cband $00, $FF, $FF, $00        ; idx 6 cyan

palette_data:
    .word $0000                     ; 0 black
    .word $001F                     ; 1 red
    .word $03E0                     ; 2 green
    .word $03FF                     ; 3 yellow
    .word $7C00                     ; 4 blue
    .word $7C1F                     ; 5 magenta
    .word $7FE0                     ; 6 cyan
    .repeat 9
        .word $0000
    .endrepeat

tilemap_data:
    .repeat 1024, i
        .word (i & $03FF)
    .endrepeat

; ------------------------------------------------------------------
;  HiROM header + vectors
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "VCHAIN TEST          "
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
    .word irq_handler        ; FFEA native NMI (unused)
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
