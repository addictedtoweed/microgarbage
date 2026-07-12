; ============================================================
;  dogcat_test.s — STANDALONE proof of the tear-free 240x200 60-colour rolling +
;  H-blank-siphon double buffer, flipping between a real dog and cat image.
;
;  RESULT (verified clean on ares): full 240x200, 60 colours, flipping dog<->cat,
;  rolling thirds + per-line BG3 siphon + overlapping-base reveal — NO top-edge
;  flicker, centred. Key fix: the letterbox UNBLANK fires LATE in the scanline
;  (HTIME_EVT = dot 240, not dot 22). Force-blank then primes the PPU tile-fetch for
;  the visible rows before they're drawn, so there's no stale first line. This ports
;  to copro_r3d.c: move the emitter's `start` unblank to a late HTIME. (The VTIME/
;  H-V-counter marching was the fragile part; a free-running H-IRQ + OPVCT + WRAM
;  action table is the cleaner kernel direction — see docs/emitter-kernel.md.)
;
;  Mirrors src/mgapi/copro_r3d.c's emitted ISR (overlapping-base thirds, palette-1
;  shared tilemap, per-line BG3 siphon + finish BG3-tail burst) but runs bare-metal
;  (no VM / no mgapi) so ares (the accuracy reference) shows whether the SCHEME is
;  clean or whether the mgapi flicker is a pipeline (latency/DMA-rate) artifact.
;
;  Each subframe delivers ONE third of the current image to the off-screen parity:
;  bottom, middle, TOP-LAST (top is the shared slot; siphon delayed past line ~100
;  so it's written after the beam scans it -> no shared-top tear), then reveal. The
;  current image toggles dog<->cat every HOLD frames so the flip is visible.
;
;  Tunables (sweep on ares): HTIME_SIP, SIP_BYTES, SIP_FIRST (keep SIP_FIRST+SIP_LINES
;  = FINISH_LN and SIP_LINES*SIP_BYTES <= THIRD_BG3).
;
;  Build: ca65 --cpu 65816 -o dogcat_test.o dogcat_test.s
;         ld65 -C dogcat_test.cfg -o dogcat_test.sfc dogcat_test.o
;  Public domain (CC0). No warranty.
; ============================================================
.p816

; ---- PPU / DMA / IRQ registers ----
INIDISP  = $2100
BGMODE   = $2105
BG1SC    = $2107
BG3SC    = $2109
BG12NBA  = $210B
BG34NBA  = $210C
BG1HOFS  = $210D
BG1VOFS  = $210E
BG3HOFS  = $2111
BG3VOFS  = $2112
VMAIN    = $2115
VMADDL   = $2116
CGADD    = $2121
CGDATA   = $2122
TM       = $212C
TS       = $212D
CGWSEL   = $2130
CGADSUB  = $2131
NMITIMEN = $4200
HTIMEL   = $4207
HTIMEH   = $4208
VTIMEL   = $4209
VTIMEH   = $420A
TIMEUP   = $4211
DMAP0    = $4300
BBAD0    = $4301
A1T0L    = $4302
A1B0     = $4304
DAS0L    = $4305
MDMAEN   = $420B

; ---- VRAM word map (overlapping bases; mirror copro_r3d.c) ----
BG1_MIDA = $0000
BG1_BOTA = $1000
BG1_TOP  = $2000
BG1_MIDB = $3000
BG1_BOTB = $4000
BG3_MIDA = $5000
BG3_BOTA = $5800
BG3_TOP  = $6000
BG3_MIDB = $6800
BG3_BOTB = $7000
TMAP_A   = $7800
TMAP_B   = $7C00

THIRD_TILES = 250
THIRD_BG1   = THIRD_TILES*32       ; 8000
THIRD_BG3   = THIRD_TILES*16       ; 4000

; ---- timing (mirror the emitter) ----
TOP_LB      = 12
; main_start unblanks (full brightness) at START_LN. Force-blank stalls the PPU tile
; fetch, so the first fetched line is stale -> a single stale row at the very top.
; Unblanking early (9 vs 12) shrinks it to ~1 line. Banked as the clean base.
START_LN    = 9
FINISH_LN   = 213
SIP_FIRST   = 100
SIP_LINES   = 113
SIP_BYTES   = 28                   ; proven-clean H-blank rate (no strip)
HTIME_SIP   = 240
; LATE turn-on: main_start unblanks at dot 240 (right margin) instead of dot 22. The
; free-running test showed this kills the top-pixel stale line — the visible lines are
; fetched/primed while force-blank is still up for the left of the line, so line 12 is
; clean. (Was 22 = full-line unblank at line start = 1px stale.)
HTIME_EVT   = 240
SIP_DELIV   = SIP_LINES*SIP_BYTES  ; 3164 via siphon
TAIL_LEN    = THIRD_BG3 - SIP_DELIV ; 836 via finish burst (still ends by line ~5)
TAIL_WOFF   = SIP_DELIV/2

HOLD        = 80                   ; logical frames each image is held

; reveal register values
NBA1_A = $00
NBA1_B = $02
NBA3_A = $05
NBA3_B = $06
SC_A   = $78
SC_B   = $7C

; ---- direct-page state ----
build_par = $00        ; 0=A (off-screen parity being filled)
deliver   = $01        ; 0=bot 1=mid 2=top (order index)
cur_img   = $02        ; 0=dog 1=cat
hold_ctr  = $03        ; 8-bit logical-frame countdown
sip_src   = $04        ; 16-bit running BG3 source addr
sip_rem   = $06        ; 8-bit siphon lines left
sip_vline = $07        ; 8-bit current siphon V line
ramvec    = $08        ; 16-bit IRQ dispatch pointer
; per-subframe delivery descriptor (computed at end of finish / in reset)
d_b1src   = $10        ; 16
d_b1bank  = $12        ; 8
d_b1dst   = $13        ; 16
d_b3src   = $15        ; 16
d_b3bank  = $17        ; 8
d_b3dst   = $18        ; 16
d_reveal  = $1A        ; 8
d_nba1    = $1B
d_nba3    = $1C
d_sc      = $1D

.segment "CODE"

; ============================================================
.proc reset
    sei
    clc
    xce
    .a16
    .i16
    rep #$38
    ldx #$1FFF
    txs
    lda #$0000
    tcd
    sep #$20
    .a8
    lda #$00
    pha
    plb                     ; DBR = 0 (regs at $2100/$4200 reachable)

    lda #$80
    sta INIDISP             ; force-blank during setup

    ; --- static PPU config ---
    lda #$01
    sta BGMODE              ; mode 1
    lda #$01
    sta TM                  ; BG1 main
    lda #$04
    sta TS                  ; BG3 sub
    lda #$02
    sta CGWSEL
    lda #$41
    sta CGADSUB             ; BG1 half-add subscreen
    ; BGVOFS = -16 both layers ($3F0 write-twice): shift the 200px image DOWN ~8px so
    ; it fills to the bottom of the visible window (was -8 -> blank strip at the bottom).
    lda #$F0
    sta BG1VOFS
    lda #$03
    sta BG1VOFS
    lda #$F0
    sta BG3VOFS
    lda #$03
    sta BG3VOFS
    ; BGHOFS = -8 both layers ($3F8): centre the 240px image in the 256px screen
    ; (was unset -> image tile-column-aligned left, 8px off centre).
    lda #$F8
    sta BG1HOFS
    lda #$03
    sta BG1HOFS
    lda #$F8
    sta BG3HOFS
    lda #$03
    sta BG3HOFS

    ; --- palette -> CGRAM 0 (64 bytes) ---
    stz CGADD
    lda #$00
    sta DMAP0               ; 1 reg, 1 byte
    lda #$22
    sta BBAD0               ; CGDATA
    lda #^pal_data
    sta A1B0
    rep #$20
    .a16
    lda #.loword(pal_data)
    sta A1T0L
    lda #64
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    ; --- both tilemaps -> VRAM ---
    lda #$80
    sta VMAIN               ; word incr after high
    lda #$01
    sta DMAP0               ; 2 regs, incr
    lda #$18
    sta BBAD0               ; VMDATAL
    ; tmap_a -> TMAP_A
    lda #^tmap_a_data
    sta A1B0
    rep #$20
    .a16
    lda #TMAP_A
    sta VMADDL
    lda #.loword(tmap_a_data)
    sta A1T0L
    lda #2048
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN
    ; tmap_b -> TMAP_B
    lda #^tmap_b_data
    sta A1B0
    rep #$20
    .a16
    lda #TMAP_B
    sta VMADDL
    lda #.loword(tmap_b_data)
    sta A1T0L
    lda #2048
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN

    ; --- initial display = parity A ---
    lda #NBA1_A
    sta BG12NBA
    lda #NBA3_A
    sta BG34NBA
    lda #SC_A
    sta BG1SC
    sta BG3SC

    ; --- initial state: build B first, deliver bottom, dog ---
    lda #1
    sta build_par
    stz deliver
    stz cur_img
    lda #HOLD
    sta hold_ctr
    jsr compute_delivery
    jsr seed_siphon         ; seed the first subframe's siphon (still blanked)

    ; ramvec -> main_start; arm H+V IRQ, first event at V=TOP_LB
    lda #<main_start
    sta ramvec
    lda #>main_start
    sta ramvec+1
    lda #HTIME_EVT
    sta HTIMEL
    stz HTIMEH
    lda #START_LN
    sta VTIMEL
    stz VTIMEH
    lda #$30
    sta NMITIMEN            ; H+V IRQ, no NMI, no auto-joy
    cli
@idle:
    wai
    bra @idle
.endproc

; ============================================================
;  compute_delivery — fill the d_* descriptor for (deliver, build_par, cur_img).
;  A8/I16 in; preserves nothing important (called from reset + finish).
; ============================================================
.proc compute_delivery
    php
    rep #$30
    .a16
    .i16
    ; X = deliver*2 (word index)
    sep #$20
    .a8
    lda deliver
    asl
    rep #$20
    .a16
    and #$00FF
    tax

    ; --- BG1/BG3 source = image base + third byte offset ---
    ; base low + bank from cur_img
    sep #$20
    .a8
    lda cur_img
    asl
    rep #$20
    .a16
    and #$00FF
    tay                     ; Y = cur_img*2
    ; d_b1src = img_bg1_lo[cur] + bg1off[deliver]
    lda img_bg1_lo,y
    clc
    adc bg1off_tbl,x
    sta d_b1src
    lda img_bg3_lo,y
    clc
    adc bg3off_tbl,x
    sta d_b3src
    sep #$20
    .a8
    lda img_bg1_bank,y
    sta d_b1bank
    lda img_bg3_bank,y
    sta d_b3bank

    ; --- VRAM dests: pick A or B table by build_par ---
    rep #$20
    .a16
    lda build_par
    and #$00FF
    beq @parA
    ; parity B
    lda bg1dst_B,x
    sta d_b1dst
    lda bg3dst_B,x
    sta d_b3dst
    sep #$20
    .a8
    lda #NBA1_B
    sta d_nba1
    lda #NBA3_B
    sta d_nba3
    lda #SC_B
    sta d_sc
    bra @dests_done
@parA:
    lda bg1dst_A,x
    sta d_b1dst
    lda bg3dst_A,x
    sta d_b3dst
    sep #$20
    .a8
    lda #NBA1_A
    sta d_nba1
    lda #NBA3_A
    sta d_nba3
    lda #SC_A
    sta d_sc
@dests_done:
    ; reveal only on the top third (deliver == 2)
    lda deliver
    cmp #2
    beq @rev
    stz d_reveal
    bra @done
@rev:
    lda #1
    sta d_reveal
@done:
    plp
    rts
.endproc

; ============================================================
;  main_start (V=TOP_LB) — unblank, seed the BG3 siphon (VMADDL while blanked),
;  hand to siphon_isr.  A/X/Y scratch (main loop only wai's).
; ============================================================
; Seed the BG3 siphon channel from the d_* descriptor. MUST run force-blanked
; (VMADDL written mid-display corrupts) — called at the END of finish + in reset,
; so main_start only has to unblank (no per-frame unblank-timing jitter -> no top-row
; flicker). A8/I16 in/out. DMA regs + VMADDL persist untouched until the siphon fires.
.proc seed_siphon
    lda #$18
    sta BBAD0
    lda #$01
    sta DMAP0
    lda #$80
    sta VMAIN
    lda d_b3bank
    sta A1B0
    rep #$20
    .a16
    lda d_b3dst
    sta VMADDL
    lda d_b3src
    sta sip_src
    sep #$20
    .a8
    lda #SIP_LINES
    sta sip_rem
    lda #SIP_FIRST
    sta sip_vline
    rts
.endproc

; main_start (V=START_LN) — unblank to full brightness, arm the siphon. The first
; fetched line after force-blank is stale (1px at the very top); banked as the best
; clean base while we explore hiding it (240x199 / scroll-based letterbox).
.proc main_start
    .a8
    .i16
    lda TIMEUP              ; ack
    lda #$0F
    sta INIDISP            ; unblank (full brightness)
    lda #<HTIME_SIP
    sta HTIMEL
    lda #>HTIME_SIP
    sta HTIMEH
    lda #SIP_FIRST
    sta VTIMEL
    stz VTIMEH
    lda #<siphon_isr
    sta ramvec
    lda #>siphon_isr
    sta ramvec+1
    rti
.endproc

; ============================================================
;  siphon_isr (V marches, H=HTIME_SIP) — lean per-line BG3 chunk.
; ============================================================
.proc siphon_isr
    .a8
    .i16
    lda TIMEUP             ; ack
    lda sip_rem
    beq @done
    ; A1T0L = sip_src, DAS0L = SIP_BYTES  (A1B0 + VMADDL persist from start)
    rep #$20
    .a16
    lda sip_src
    sta A1T0L
    lda #SIP_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$8F
    sta INIDISP           ; force-blank (right margin)
    lda #$01
    sta MDMAEN
    lda #$0F
    sta INIDISP           ; unblank
    rep #$20
    .a16
    lda sip_src
    clc
    adc #SIP_BYTES
    sta sip_src
    sep #$20
    .a8
    dec sip_rem
    beq @done
    inc sip_vline
    lda sip_vline
    sta VTIMEL
    stz VTIMEH
    rti
@done:
    ; hand to finish: H relaxed, V = FINISH_LN
    lda #HTIME_EVT
    sta HTIMEL
    stz HTIMEH
    lda #FINISH_LN
    sta VTIMEL
    stz VTIMEH
    lda #<main_finish
    sta ramvec
    lda #>main_finish
    sta ramvec+1
    rti
.endproc

; ============================================================
;  main_finish (V=FINISH_LN) — force-blank, burst BG1 + BG3 tail, reveal, advance,
;  compute next descriptor, hand to main_start.
; ============================================================
.proc main_finish
    .a8
    .i16
    lda TIMEUP             ; ack
    lda #$80
    sta INIDISP           ; force-blank
    lda #$18
    sta BBAD0
    lda #$01
    sta DMAP0
    lda #$80
    sta VMAIN
    ; --- BG1 third burst ---
    lda d_b1bank
    sta A1B0
    rep #$20
    .a16
    lda d_b1dst
    sta VMADDL
    lda d_b1src
    sta A1T0L
    lda #THIRD_BG1
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN
    ; --- BG3 tail burst (src+SIP_DELIV -> dst+TAIL_WOFF) ---
    lda d_b3bank
    sta A1B0
    rep #$20
    .a16
    lda d_b3dst
    clc
    adc #TAIL_WOFF
    sta VMADDL
    lda d_b3src
    clc
    adc #SIP_DELIV
    sta A1T0L
    lda #TAIL_LEN
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta MDMAEN
    ; --- reveal (atomic parity flip) if this was the top third ---
    lda d_reveal
    beq @norev
    lda d_nba1
    sta BG12NBA
    lda d_nba3
    sta BG34NBA
    lda d_sc
    sta BG1SC
    sta BG3SC
@norev:
    ; --- advance delivery ---
    lda deliver
    inc a
    cmp #3
    bcc @store_deliver
    ; wrapped: next parity, count a logical frame, maybe flip image
    stz deliver
    lda build_par
    eor #$01
    sta build_par
    dec hold_ctr
    bne @compute
    lda #HOLD
    sta hold_ctr
    lda cur_img
    eor #$01
    sta cur_img
    bra @compute
@store_deliver:
    sta deliver
@compute:
    jsr compute_delivery
    jsr seed_siphon         ; seed next subframe's siphon while still blanked
    ; leave force-blank ON (main_start drops to brightness-0 at START_LN)
    ; hand to main_start at V=START_LN
    lda #HTIME_EVT
    sta HTIMEL
    stz HTIMEH
    lda #START_LN
    sta VTIMEL
    stz VTIMEH
    lda #<main_start
    sta ramvec
    lda #>main_start
    sta ramvec+1
    rti
.endproc

; ============================================================
;  IRQ trampoline (kernel-faithful indirect dispatch)
; ============================================================
.proc irq_tramp
    jmp (ramvec)
.endproc
.proc nmi_stub
    rti
.endproc

; ---- per-deliver (bot,mid,top) tables ----
bg1off_tbl: .word 16000, 8000, 0      ; third byte offset into BG1 (bot=500t,mid=250t,top=0)
bg3off_tbl: .word  8000, 4000, 0
bg1dst_A:   .word BG1_BOTA, BG1_MIDA, BG1_TOP
bg1dst_B:   .word BG1_BOTB, BG1_MIDB, BG1_TOP
bg3dst_A:   .word BG3_BOTA, BG3_MIDA, BG3_TOP
bg3dst_B:   .word BG3_BOTB, BG3_MIDB, BG3_TOP
; ---- per-image (dog,cat) source bases ----
img_bg1_lo:   .word .loword(dog_bg1), .loword(cat_bg1)
img_bg3_lo:   .word .loword(dog_bg3), .loword(cat_bg3)
img_bg1_bank: .byte ^dog_bg1, ^cat_bg1
img_bg3_bank: .byte ^dog_bg3, ^cat_bg3

; ============================================================
;  Image data
; ============================================================
.segment "RODATA0"
pal_data:    .incbin "gen/dogcat/pal.bin"
tmap_a_data: .incbin "gen/dogcat/tmap_a.bin"
tmap_b_data: .incbin "gen/dogcat/tmap_b.bin"
dog_bg1:     .incbin "gen/dogcat/dog_bg1.bin"

.segment "RODATA1"
dog_bg3:     .incbin "gen/dogcat/dog_bg3.bin"
cat_bg1:     .incbin "gen/dogcat/cat_bg1.bin"
cat_bg3:     .incbin "gen/dogcat/cat_bg3.bin"

; ============================================================
;  HiROM header + vectors
; ============================================================
.segment "HEADER"
    .byte "DOGCAT ROLLING TEST  "   ; 21 chars title
    .byte $21                        ; map mode: HiROM, slow
    .byte $00                        ; ROM type
    .byte $08                        ; ROM size (128 KB -> log2(128)=17 -> code 8..? use 8)
    .byte $00                        ; RAM size
    .byte $01                        ; country
    .byte $00                        ; dev id
    .byte $00                        ; version
    .word $AAAA                      ; checksum complement (placeholder)
    .word $5555                      ; checksum

.segment "VECTORS"
    ; native: COP BRK ABORT NMI - IRQ
    .word $0000, $0000, $0000, .loword(nmi_stub), $0000, .loword(irq_tramp)
    ; emulation: - - COP - ABORT NMI RESET IRQ/BRK
    .word $0000, $0000, $0000, $0000
    .word $0000, .loword(nmi_stub), .loword(reset), .loword(irq_tramp)
