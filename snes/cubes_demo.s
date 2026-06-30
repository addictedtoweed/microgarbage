; ============================================================
;  cube3d.s — standalone SNES wireframe 3D cube (real-hardware test ROM).
;
;  Stage v1: a STATIC wireframe cube. Validates the whole non-3D pipeline —
;  the tiled bitmap framebuffer (plot pixel), Bresenham line draw, the
;  per-frame WRAM->VRAM vblank DMA, and the centered BG display — with the 8
;  cube vertices PRE-projected on the host (no runtime rotate/divide yet).
;  v2 adds the sin/cos rotation, v3 the bounce.
;
;  Framebuffer: 128x128, 2bpp, in WRAM $7F0000 (4096 B). Laid out as 16x16
;  linear 8x8 tiles (tile = (y>>3)*16 + (x>>3)); DMA'd to VRAM CHR word 0 each
;  vblank. A 16x16 tile window centered in the 32x32 map shows it; margins use
;  the zeroed blank tile 256. Mode 0, BG1 2bpp, colour 1 = the wire.
;
;  Build:  ca65 --cpu 65816 -o build/cube3d.o cube3d.s
;          ld65 -C siphon_hblank_test.cfg -o build/cube3d.sfc build/cube3d.o
;  Public domain (CC0).
; ============================================================
.p816
.smart +                   ; track sep/rep so immediates get the right size (M/X)
.include "snes.inc"

; cubes_demo: 3 spinning high-colour cubes + a colour bar (sd2snes showpiece).
; ONE proven 64x64 dual-layer framebuffer is reused round-robin: each PPU frame
; re-renders ONE cube into FB1/FB3 and DMAs it to that cube's own VRAM CHR region
; (so each cube animates at 20 fps and all three display at once). Mode 1, BG1
; 4bpp main + BG3 2bpp sub, half-add colour math — same renderer as cube3d_hc.s.
FB_BANK   = $7F
FB_ADDR   = $7F0000        ; WRAM framebuffer base (BG1 4bpp), shared by all cubes
FB_BYTES  = $0800          ; 64x64 4bpp = 8x8 tiles x 32 B = 2048
FBW       = 64
FBTW      = 8              ; tiles wide
CENTER    = 32
; BG1 4bpp CHR: cube i at word i*$400 (tiles 64*i..). cube0 $0000, 1 $0400, 2 $0800.
CHR_W     = $0000          ; VRAM word: BG1 4bpp CHR base (cube 0)
BLANK_W   = $0C00          ; VRAM word: blank tile 192 (past all 3 cubes' CHR)
SWATCH_W  = $0C10          ; VRAM word: 16 colour-bar swatch tiles (193..208)
TMAP_W    = $1000          ; VRAM word: BG1 tilemap base

; BG3 2bpp sub layer (colour-math sub screen).
; NOTE: must live in $7F (or above $7E:2000) — $7E:0000-1FFF mirrors low RAM
; (DP/stack/XL/XR), so a framebuffer there corrupts the program's own memory.
FB3_ADDR  = $7F0800        ; right after FB1 ($7F0000..$7F07FF), still in bank $7F
FB3_BYTES = $0400          ; 64x64 2bpp = 8x8 tiles x 16 B = 1024
; BG3 2bpp CHR: cube i at word $2000+i*$200. cube0 $2000, 1 $2200, 2 $2400.
FB3_CHR   = $2000          ; VRAM word: BG3 2bpp CHR base (char base unit 2, cube 0)
FB3_BLANK = $2600          ; VRAM word: BG3 blank tile 192 (past all 3 cubes' CHR)
SUB_SWATCH_W = $2608       ; VRAM word: 4 sub-level swatch tiles (BG3 tiles 193..196)
FB3_TMAP  = $2800          ; VRAM word: BG3 tilemap base

; ---- direct-page scratch ----
PX   = $00     ; 16-bit (clean: coords 0..127)
PY   = $02
OFFS = $04
MASK = $06
LX0  = $08
LY0  = $0A
X1   = $0C
Y1   = $0E
DX   = $10
DYn  = $12     ; negative dy (Bresenham convention)
SX   = $14
SY   = $16
ERR  = $18
E2   = $1A
READY = $1C    ; 8-bit: 1 = fb holds a complete frame awaiting DMA

; runtime 3D transform scratch
PTS   = $0020  ; 8 verts x (sx,sy) projected — written by transform, read by draw_cube
MA    = $30    ; signed-multiply inputs / sign
MB    = $31
MSIGN = $32
MP    = $34    ; 16-bit product
CY    = $36    ; cos(ANG) (signed Q0.7)
SN    = $37    ; sin(ANG)
VX    = $38    ; current model vertex
VY    = $39
VZ    = $3A
VI    = $3B    ; vertex counter 0..7
IDX   = $3C    ; 16-bit table index scratch
PIDX  = $3E
SXV   = $40
SYV   = $41
T1    = $42    ; 16-bit accumulator
ANG   = $44    ; Y rotation angle 0..255
ANG2  = $45    ; X rotation angle 0..255
CX    = $46    ; cos(ANG2)
SN2   = $47    ; sin(ANG2)
Z1    = $48    ; rotated z after Y-rotation
X1V   = $49    ; rotated x
Y2V   = $4A    ; rotated y after X-rotation
BX    = $4B    ; bounce offset x (signed)
BY    = $4C    ; bounce offset y (signed)
VBX   = $4D    ; bounce velocity x (signed)
VBY   = $4E    ; bounce velocity y (signed)

; filled-face scratch (projected face verts, 16-bit-clean slots)
FX0   = $50
FY0   = $52
FX1   = $54
FY1   = $56
FX2   = $58
FY2   = $5A
FX3   = $5C
FY3   = $5E
YMIN  = $60
YMAX  = $62
YMAX1 = $64
AREA  = $66
FI    = $68    ; face counter
COL   = $69    ; current face colour (1..3)
RY    = $6A    ; span row y (16-bit)
SPL   = $6C    ; span left x
SPR   = $6D    ; span right x
CXB   = $6E    ; span fill x cursor

; per-scanline fill extents (low RAM / WRAM mirror, indexed by y 0..127)
XL    = $0200
XR    = $0300

; byte-span fill scratch
P0M   = $6F    ; plane fill bytes for COL ($FF or $00), 4bpp -> 4 planes
P1M   = $70
P2M   = $75
P3M   = $76
LXT   = $71    ; left tile column
RXT   = $72    ; right tile column
TC    = $73    ; current tile column
TM_   = $74    ; this tile's pixel mask
NTM   = $77    ; ~mask (partial-tile replace)
SCR   = $78    ; scratch
Q0M   = $79    ; BG3 2bpp sub plane fill bytes
Q1M   = $7A
COL3  = $7B    ; current face sub colour (0..3)
CUBE  = $7C    ; 16-bit: current cube index (0..2) for round-robin render
CUBEOFF = $7E  ; 16-bit: CUBE*6 = base into the 18-byte FACECOL/FACECOL3 tables

; per-cube rotation state (low RAM, indexed by CUBE 0..2)
ANGS  = $0420
ANG2S = $0424

.segment "CODE"

.proc reset_handler
    sei
    clc
    xce                     ; native mode
    rep #$38
    .a16
    .i16
    ldx #$1FFF
    txs
    lda #$0000
    tcd                     ; DP = $0000
    sep #$20
    .a8
    lda #$00
    pha
    plb                     ; DBR = $00

    lda #$80
    sta INIDISP             ; force-blank during setup

    ; --- BG/mode: Mode 1 makes BG1 4bpp ---
    lda #$01
    sta BGMODE              ; mode 1 (BG1/BG2 4bpp, BG3 2bpp)
    lda #($10)              ; BG1SC: tilemap base = TMAP_W/$400 = 4 -> 4<<2; size 32x32
    sta BG1SC
    stz BG12NBA             ; BG1 char base = CHR_W/$1000 = 0
    ; explicit BG1 scroll = 0 (power-on garbage on real HW / bsnes-accuracy)
    stz BG1HOFS
    stz BG1HOFS
    stz BG1VOFS
    stz BG1VOFS
    lda #$01
    sta TM                  ; BG1 on main

    ; --- BG3 (2bpp sub layer) ---
    lda #$28                ; BG3SC: map base FB3_TMAP/$400 = 10 -> 10<<2
    sta BG3SC
    lda #$02                ; BG34NBA: BG3 char base = FB3_CHR/$1000 = 2
    sta BG34NBA
    stz BG3HOFS
    stz BG3HOFS
    stz BG3VOFS
    stz BG3VOFS
    lda #$04
    sta TS                  ; BG3 on sub screen
    lda #$02
    sta CGWSEL              ; colour math always; use sub screen
    lda #$41
    sta CGADSUB            ; BG1 participates; half; add (sub)

    ; --- palette: 16 4bpp colours (0..15) + BG3 sub palette 4 (16..19) ---
    stz CGADD
    stz DMAP0
    lda #<CGDATA
    sta BBAD0
    rep #$20
    .a16
    lda #.loword(palette_data)
    sta A1T0L
    lda #(20*2)
    sta DAS0L
    sep #$20
    .a8
    lda #^palette_data
    sta A1B0
    lda #$01
    sta MDMAEN

    ; --- zero the blank tile (16 words = one 4bpp tile at word BLANK_W) ---
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda #BLANK_W
    sta VMADDL
    ldx #16
    sep #$20
    .a8
@zblank:
    stz VMDATAL
    stz VMDATAH
    dex
    bne @zblank

    ; --- tilemap -> VRAM word TMAP_W (1024 words = 2048 B) ---
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda #TMAP_W
    sta VMADDL
    lda #.loword(tilemap_data)
    sta A1T0L
    lda #2048
    sta DAS0L
    sep #$20
    .a8
    lda #^tilemap_data
    sta A1B0
    lda #$01
    sta DMAP0
    lda #$18                ; $2118 VMDATA
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; --- BG3 blank tile (8 words at FB3_BLANK) ---
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda #FB3_BLANK
    sta VMADDL
    ldx #8
    sep #$20
    .a8
@zblank3:
    stz VMDATAL
    stz VMDATAH
    dex
    bne @zblank3

    ; --- BG3 tilemap -> FB3_TMAP ---
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda #FB3_TMAP
    sta VMADDL
    lda #.loword(bg3_tilemap_data)
    sta A1T0L
    lda #2048
    sta DAS0L
    sep #$20
    .a8
    lda #^bg3_tilemap_data
    sta A1B0
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; --- colour-bar swatch CHR (16 4bpp tiles, colours 0..15) -> BG1 tile 193 ---
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda #SWATCH_W
    sta VMADDL
    lda #.loword(base_swatch_chr)
    sta A1T0L
    lda #512
    sta DAS0L
    sep #$20
    .a8
    lda #^base_swatch_chr
    sta A1B0
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; --- sub-level swatch CHR (4 2bpp tiles, sub colours 0..3) -> BG3 tile 193 ---
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda #SUB_SWATCH_W
    sta VMADDL
    lda #.loword(sub_swatch_chr)
    sta A1T0L
    lda #64
    sta DAS0L
    sep #$20
    .a8
    lda #^sub_swatch_chr
    sta A1B0
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; --- go ---
    stz NMITIMEN            ; no interrupts — we poll vblank synchronously
    stz BX                  ; cubes spin in place (no bounce) — centred in each window
    stz BY
    stz VBX
    stz VBY
    stz CUBE                ; round-robin starts at cube 0 (16-bit, clear both bytes)
    stz CUBE+1
    ; stagger each cube's start angles so the three look distinct
    lda #0
    sta ANGS+0
    lda #40
    sta ANGS+1
    lda #128
    sta ANGS+2
    lda #0
    sta ANG2S+0
    lda #96
    sta ANG2S+1
    lda #200
    sta ANG2S+2
    lda #$0F
    sta INIDISP             ; unblank

main_loop:
.if .defined(DIAG)
    jsr fill_solid          ; isolate DMA+display: fill the box solid colour 1
.elseif .defined(FILLTEST)
    jsr clearfb
    jsr fill_test           ; isolate plotc: fixed rectangle, no transform/cull
.elseif .defined(EXTTEST)
    jsr clearfb
    jsr fill_test2          ; isolate span/extent machinery with fixed XL/XR
.elseif .defined(RASTTEST)
    jsr clearfb
    jsr fill_test3          ; isolate rast_edge: fixed box edges -> extents -> fill
.else
    jsr clearfb
    ; load cube CUBE's rotation angles into the renderer's ANG/ANG2
    sep #$20
    .a8
    ldx CUBE                ; X is 16-bit (i16) and CUBE is a clean word
    lda ANGS,x
    sta ANG
    lda ANG2S,x
    sta ANG2
    ; CUBEOFF = cube_off6[CUBE] = CUBE*6 -> base into the 18-byte FACECOL tables
    rep #$20
    .a16
    lda CUBE
    asl a
    tax
    lda cube_off6,x
    sta CUBEOFF
    sep #$20
    .a8
    jsr transform           ; rotate model verts -> projected PTS
.ifdef WIRE
    jsr draw_cube
.else
    jsr fill_cube
.endif
    ; advance + store this cube's angles (each cube updates every 3rd frame -> 20fps)
    sep #$20
    .a8
    ldx CUBE
    lda ANGS,x
    clc
    adc #4                  ; Y rotation speed
    sta ANGS,x
    lda ANG2S,x
    clc
    adc #6                  ; X rotation speed (different rate -> tumble)
    sta ANG2S,x
.endif
    jsr wait_vblank         ; frame complete; sync to vblank
    jsr dma_fb              ; copy fb -> cube CUBE's CHR region (VRAM-safe)
    ; advance round-robin index 0->1->2->0
    rep #$20
    .a16
    lda CUBE
    inc a
    cmp #3
    bcc @cubeok
    lda #0
@cubeok:
    sta CUBE
    sep #$20
    .a8
    jmp main_loop
.endproc

; ---- DIAG: fill the whole fb with colour 1 (plane0 = $FF, plane1 = $00) ----
.proc fill_solid
    php
    rep #$30
    .a16
    .i16
    ldx #$0000
    lda #$00FF              ; low byte (plane0)=$FF, high byte (plane1)=$00
@fl:
    sta FB_ADDR, x
    inx
    inx
    cpx #FB_BYTES
    bne @fl
    plp
    rts
.endproc

; ---- clear both framebuffers (FB1 4bpp + FB3 2bpp) ----
.proc clearfb
    php
    rep #$30
    .a16
    .i16
    ldx #$0000
    lda #$0000
@cl:
    sta FB_ADDR, x          ; long,x -> $7F0000+X
    inx
    inx
    cpx #FB_BYTES
    bne @cl
    ldx #$0000
@cl3:
    sta FB3_ADDR, x         ; long,x -> $7E0000+X
    inx
    inx
    cpx #FB3_BYTES
    bne @cl3
    plp
    rts
.endproc

; ---- plot colour-1 pixel at (PX,PY) into the fb ----
.proc plot
    php
    rep #$20
    .a16
    lda PX
    and #$00F8
    asl a                   ; (px>>3)*16
    sta OFFS
    lda PY
    and #$0007
    asl a                   ; (py&7)*2
    clc
    adc OFFS
    sta OFFS                ; low part (high byte 0)
    sep #$20
    .a8
    lda PY
    lsr a
    lsr a
    lsr a                   ; py>>3
    sta OFFS+1              ; high byte of offset
    lda PX
    and #$07
    tay
    lda bitmasks,y
    sta MASK
    ldx OFFS
    lda MASK
    ora FB_ADDR, x
    sta FB_ADDR, x
    plp
    rts
.endproc

; ---- Bresenham line (LX0,LY0)-(X1,Y1), colour 1 ----
.proc line
    rep #$30
    .a16
    .i16
    ; DX = abs(X1-LX0), SX = sign
    lda X1
    sec
    sbc LX0
    bpl @dxp
    eor #$FFFF
    inc a
    sta DX
    lda #$FFFF
    sta SX
    bra @dy
@dxp:
    sta DX
    lda #$0001
    sta SX
@dy:
    ; DYn = -abs(Y1-LY0)  (negative), SY = sign
    lda Y1
    sec
    sbc LY0
    bpl @dyp
    sta DYn                 ; diff already negative = -abs
    lda #$FFFF
    sta SY
    bra @err
@dyp:
    eor #$FFFF
    inc a
    sta DYn                 ; -diff
    lda #$0001
    sta SY
@err:
    lda DX
    clc
    adc DYn
    sta ERR                 ; err = dx + dy
@loop:
    lda LX0
    sta PX
    lda LY0
    sta PY
    jsr plot                ; preserves P via php/plp
    lda LX0
    cmp X1
    bne @cont
    lda LY0
    cmp Y1
    bne @cont
    rts
@cont:
    lda ERR
    asl a
    sta E2                  ; e2 = 2*err
    ; if e2 >= dy: err += dy; x += sx
    lda E2
    sec
    sbc DYn
    bmi @nox
    lda ERR
    clc
    adc DYn
    sta ERR
    lda LX0
    clc
    adc SX
    sta LX0
@nox:
    ; if e2 <= dx: err += dx; y += sy
    lda DX
    sec
    sbc E2
    bmi @noy
    lda ERR
    clc
    adc DX
    sta ERR
    lda LY0
    clc
    adc SY
    sta LY0
@noy:
    bra @loop
.endproc

; ---- draw the 12 cube edges from PTS[] via EDGES[] ----
.proc draw_cube
    rep #$30
    .a16
    .i16
    ldx #$0000
@e:
    lda EDGES,x
    and #$00FF
    asl a
    tay                     ; Y = a*2
    lda PTS,y
    and #$00FF
    sta LX0                 ; sx
    lda PTS,y
    xba
    and #$00FF
    sta LY0                 ; sy
    inx
    lda EDGES,x
    and #$00FF
    asl a
    tay                     ; Y = b*2
    lda PTS,y
    and #$00FF
    sta X1
    lda PTS,y
    xba
    and #$00FF
    sta Y1
    inx
    phx
    jsr line
    plx
    cpx #24
    bne @e
    rts
.endproc

; ---- transform: rotate the 8 model verts about Y by ANG then X by ANG2,
;      ortho-project to PTS. Y: x1=x*cy-z*sy, z1=x*sy+z*cy. X: y2=y*cx-z1*sx2.
;      Project (drop z2): sx=64+x1, sy=64-y2. ----
.proc transform
    sep #$20
    .a8
    ; CY=cos(ANG) SN=sin(ANG) ; CX=cos(ANG2) SN2=sin(ANG2)
    lda ANG
    clc
    adc #64
    sta IDX
    stz IDX+1
    ldx IDX
    lda sintab,x
    sta CY
    lda ANG
    sta IDX
    stz IDX+1
    ldx IDX
    lda sintab,x
    sta SN
    lda ANG2
    clc
    adc #64
    sta IDX
    stz IDX+1
    ldx IDX
    lda sintab,x
    sta CX
    lda ANG2
    sta IDX
    stz IDX+1
    ldx IDX
    lda sintab,x
    sta SN2
    ; per vertex
    stz VI
    ldx #$0000              ; MODELV byte index (0,3,6,...)
@vl:
    lda MODELV+0,x
    sta VX
    lda MODELV+1,x
    sta VY
    lda MODELV+2,x
    sta VZ
    phx                     ; preserve model index
    ; x1 = (VX*CY - VZ*SN) >> 7
    lda VX
    sta MA
    lda CY
    sta MB
    jsr smul
    rep #$20
    .a16
    lda MP
    sta T1
    sep #$20
    .a8
    lda VZ
    sta MA
    lda SN
    sta MB
    jsr smul
    rep #$20
    .a16
    lda T1
    sec
    sbc MP
    jsr asr7
    sep #$20
    .a8
    sta X1V
    ; z1 = (VX*SN + VZ*CY) >> 7
    lda VX
    sta MA
    lda SN
    sta MB
    jsr smul
    rep #$20
    .a16
    lda MP
    sta T1
    sep #$20
    .a8
    lda VZ
    sta MA
    lda CY
    sta MB
    jsr smul
    rep #$20
    .a16
    lda T1
    clc
    adc MP
    jsr asr7
    sep #$20
    .a8
    sta Z1
    ; y2 = (VY*CX - Z1*SN2) >> 7
    lda VY
    sta MA
    lda CX
    sta MB
    jsr smul
    rep #$20
    .a16
    lda MP
    sta T1
    sep #$20
    .a8
    lda Z1
    sta MA
    lda SN2
    sta MB
    jsr smul
    rep #$20
    .a16
    lda T1
    sec
    sbc MP
    jsr asr7
    sep #$20
    .a8
    sta Y2V
    ; project + bounce: sx = CENTER + BX + x1 ; sy = CENTER + BY - y2
    lda X1V
    clc
    adc #CENTER
    clc
    adc BX
    sta SXV
    lda #CENTER
    clc
    adc BY
    sec
    sbc Y2V
    sta SYV
    ; PTS[VI*2] = sx ; PTS[VI*2+1] = sy
    lda VI
    asl a
    sta PIDX
    stz PIDX+1
    ldy PIDX
    lda SXV
    sta PTS,y
    iny
    lda SYV
    sta PTS,y
    plx
    inx
    inx
    inx
    inc VI
    lda VI
    cmp #8
    beq @done               ; loop body > 127 B -> branch can't reach @vl; use jmp
    jmp @vl
@done:
    rts
.endproc

; ---- asr7: arithmetic shift A (16-bit signed) right by 7. cmp #$8000 sets
;      carry = sign bit; ror pulls it into bit15, preserving sign. ----
.proc asr7
    .a16
    .i16
    ldy #7
@l:
    cmp #$8000
    ror a
    dey
    bne @l
    rts
.endproc

; ---- smul: MP (16-bit) = (int8)MA * (int8)MB, signed. Uses the HW multiplier. ----
.proc smul
    sep #$20
    .a8
    lda MA
    eor MB
    sta MSIGN               ; bit7 = product sign
    lda MA
    bpl @a
    eor #$FF
    inc a
@a:
    sta $4202               ; |MA| -> multiplicand
    lda MB
    bpl @b
    eor #$FF
    inc a
@b:
    sta $4203               ; |MB| -> multiplier (starts multiply)
    nop
    nop
    nop
    nop                     ; >= 8-cycle multiply latency
    rep #$20
    .a16
    lda $4216
    sta MP                  ; unsigned product
    sep #$20
    .a8
    lda MSIGN
    bpl @done
    rep #$20
    .a16
    lda #$0000
    sec
    sbc MP
    sta MP                  ; negate for signed result
    sep #$20
    .a8
@done:
    rts
.endproc

; ---- fill_test: draw a fixed 40..90 square via plotc (isolates plotc) ----
.proc fill_test
    sep #$20
    .a8
    lda #$01
    sta COL
    ldx #40                 ; y
@yl:
    stx PY
    ldy #40                 ; x
@xl:
    sty PX
    phx
    phy
    jsr plotc
    ply
    plx
    iny
    cpy #90
    bne @xl
    inx
    cpx #90
    bne @yl
    rts
.endproc

; ---- fill_test2: span/extent machinery test — fixed XL=45/XR=85 over rows
;      40..80, then run the same @row/@px span fill fill_cube uses. ----
.proc fill_test2
    sep #$20
    .a8
    lda #$01
    sta COL
    ldx #40
@set:
    lda #45
    sta XL,x
    lda #85
    sta XR,x
    inx
    cpx #81
    bne @set
    rep #$20
    .a16
    lda #40
    sta YMIN
    lda #80
    sta YMAX
    lda #81
    sta YMAX1
    lda #40
    sta RY
@row:
    ldx RY
    sep #$20
    lda XL,x
    sta SPL
    lda XR,x
    sta SPR
    cmp SPL
    bcc @rn
    rep #$20
    lda RY
    sta PY
    sep #$20
    lda SPL
    sta CXB
@px:
    stz PX+1
    lda CXB
    sta PX
    jsr plotc
    lda CXB
    cmp SPR
    bcs @rn
    inc CXB
    bra @px
@rn:
    rep #$20
    lda RY
    inc a
    sta RY
    cmp YMAX1
    beq @done
    jmp @row
@done:
    rts
.endproc

; ---- fill_test3: rast_edge test — rasterize a fixed box, then span-fill it ----
.proc fill_test3
    sep #$20
    .a8
    lda #$01
    sta COL
    ldx #40
@c:
    lda #$FF
    sta XL,x
    stz XR,x
    inx
    cpx #81
    bne @c
    rep #$20
    .a16
    lda #50
    sta LX0
    lda #40
    sta LY0
    lda #80
    sta X1
    lda #40
    sta Y1
    jsr rast_edge
    lda #80
    sta LX0
    lda #40
    sta LY0
    lda #80
    sta X1
    lda #80
    sta Y1
    jsr rast_edge
    lda #80
    sta LX0
    lda #80
    sta LY0
    lda #50
    sta X1
    lda #80
    sta Y1
    jsr rast_edge
    lda #50
    sta LX0
    lda #80
    sta LY0
    lda #50
    sta X1
    lda #40
    sta Y1
    jsr rast_edge
    lda #40
    sta YMIN
    lda #80
    sta YMAX
    lda #81
    sta YMAX1
    lda #40
    sta RY
@row:
    ldx RY
    sep #$20
    lda XL,x
    sta SPL
    lda XR,x
    sta SPR
    cmp SPL
    bcc @rn
    rep #$20
    lda RY
    sta PY
    sep #$20
    lda SPL
    sta CXB
@px:
    stz PX+1
    lda CXB
    sta PX
    jsr plotc
    lda CXB
    cmp SPR
    bcs @rn
    inc CXB
    bra @px
@rn:
    rep #$20
    lda RY
    inc a
    sta RY
    cmp YMAX1
    beq @done
    jmp @row
@done:
    rts
.endproc

; ---- plotc: set pixel (PX,PY) to colour COL (0..15) in the 4bpp fb ----
.proc plotc
    php
    rep #$20
    .a16
    lda PX
    and #$00F8
    asl a
    asl a                   ; (px>>3)*32  (4bpp tile = 32 B)
    sta OFFS
    lda PY
    and #$0007
    asl a
    clc
    adc OFFS
    sta OFFS
    sep #$20
    .a8
    lda PY
    lsr a
    lsr a
    lsr a
    sta OFFS+1
    lda PX
    and #$07
    sta IDX
    stz IDX+1
    ldy IDX
    lda bitmasks,y
    sta MASK
    ldx OFFS
    lda COL
    and #$01
    beq @no0
    lda MASK
    ora FB_ADDR,x
    sta FB_ADDR,x
@no0:
    lda COL
    and #$02
    beq @no1
    lda MASK
    ora FB_ADDR+1,x
    sta FB_ADDR+1,x
@no1:
    lda COL
    and #$04
    beq @no2
    lda MASK
    ora FB_ADDR+16,x
    sta FB_ADDR+16,x
@no2:
    lda COL
    and #$08
    beq @no3
    lda MASK
    ora FB_ADDR+17,x
    sta FB_ADDR+17,x
@no3:
    plp
    rts
.endproc

; ---- hspan_fast: fill row PY from SPL..SPR in COL (P0M/P1M) by whole bytes,
;      only masking the partial tiles at each end. ~8x fewer writes than plotc. ----
.proc hspan_fast
    ; A8 on entry. base byte offset = plane0 of (SPL, PY)'s tile
    rep #$20
    .a16
    lda PY
    and #$0007
    asl a
    sta OFFS                ; (y&7)*2
    lda SPL
    and #$00F8
    asl a
    asl a                   ; (SPL>>3)*32  (4bpp)
    clc
    adc OFFS
    sta OFFS
    sep #$20
    .a8
    lda PY
    lsr a
    lsr a
    lsr a
    sta OFFS+1              ; (y>>3) -> high byte (*256)
    lda SPL
    lsr a
    lsr a
    lsr a
    sta LXT
    lda SPR
    lsr a
    lsr a
    lsr a
    sta RXT
    ldx OFFS
    lda LXT
    sta TC
@tile:
    lda #$FF
    sta TM_
    lda TC
    cmp LXT
    bne @notL
    lda SPL
    and #$07
    sta IDX
    stz IDX+1
    ldy IDX
    lda leftmask,y
    and TM_
    sta TM_
@notL:
    lda TC
    cmp RXT
    bne @notR
    lda SPR
    and #$07
    sta IDX
    stz IDX+1
    ldy IDX
    lda rightmask,y
    and TM_
    sta TM_
@notR:
    lda TM_
    cmp #$FF
    bne @part
    ; full tile: overwrite all 4 planes (no seam blend with neighbours)
    lda P0M
    sta FB_ADDR,x
    lda P1M
    sta FB_ADDR+1,x
    lda P2M
    sta FB_ADDR+16,x
    lda P3M
    sta FB_ADDR+17,x
    bra @adv
@part:
    ; partial tile: replace only the masked pixels, keep the rest
    lda TM_
    eor #$FF
    sta NTM
    lda NTM
    and FB_ADDR,x
    sta SCR
    lda TM_
    and P0M
    ora SCR
    sta FB_ADDR,x
    lda NTM
    and FB_ADDR+1,x
    sta SCR
    lda TM_
    and P1M
    ora SCR
    sta FB_ADDR+1,x
    lda NTM
    and FB_ADDR+16,x
    sta SCR
    lda TM_
    and P2M
    ora SCR
    sta FB_ADDR+16,x
    lda NTM
    and FB_ADDR+17,x
    sta SCR
    lda TM_
    and P3M
    ora SCR
    sta FB_ADDR+17,x
@adv:
    lda TC
    cmp RXT
    bcs @done               ; processed the right tile -> finished
    inc TC
    rep #$20
    .a16
    txa
    clc
    adc #32                 ; next 4bpp tile = +32 bytes
    tax
    sep #$20
    .a8
    jmp @tile               ; loop body > 127 B -> jmp, not bra
@done:
    rts
.endproc

; pixel masks for a partial tile. leftmask[a]=pixels a..7 set; rightmask[b]=0..b.
leftmask:
    .byte $FF,$7F,$3F,$1F,$0F,$07,$03,$01
rightmask:
    .byte $80,$C0,$E0,$F0,$F8,$FC,$FE,$FF

; ---- hspan_fast3: same byte-span fill into the 2bpp BG3 framebuffer (FB3) in
;      COL3 (Q0M/Q1M). 2bpp tiles (16 B, stride 16); 64-wide row stride 128. ----
.proc hspan_fast3
    rep #$20
    .a16
    lda PY
    and #$0007
    asl a
    sta OFFS                ; (y&7)*2
    lda PY
    and #$00F8
    asl a
    asl a
    asl a
    asl a                   ; (y>>3)*128  (8 tiles x 16 B row stride)
    clc
    adc OFFS
    sta OFFS
    lda SPL
    and #$00F8
    asl a                   ; (SPL>>3)*16
    clc
    adc OFFS
    sta OFFS
    sep #$20
    .a8
    lda SPL
    lsr a
    lsr a
    lsr a
    sta LXT
    lda SPR
    lsr a
    lsr a
    lsr a
    sta RXT
    ldx OFFS
    lda LXT
    sta TC
@tile:
    lda #$FF
    sta TM_
    lda TC
    cmp LXT
    bne @notL
    lda SPL
    and #$07
    sta IDX
    stz IDX+1
    ldy IDX
    lda leftmask,y
    and TM_
    sta TM_
@notL:
    lda TC
    cmp RXT
    bne @notR
    lda SPR
    and #$07
    sta IDX
    stz IDX+1
    ldy IDX
    lda rightmask,y
    and TM_
    sta TM_
@notR:
    lda TM_
    cmp #$FF
    bne @part
    lda Q0M
    sta FB3_ADDR,x
    lda Q1M
    sta FB3_ADDR+1,x
    bra @adv
@part:
    lda TM_
    eor #$FF
    sta NTM
    lda NTM
    and FB3_ADDR,x
    sta SCR
    lda TM_
    and Q0M
    ora SCR
    sta FB3_ADDR,x
    lda NTM
    and FB3_ADDR+1,x
    sta SCR
    lda TM_
    and Q1M
    ora SCR
    sta FB3_ADDR+1,x
@adv:
    lda TC
    cmp RXT
    bcs @done
    inc TC
    rep #$20
    .a16
    txa
    clc
    adc #16
    tax
    sep #$20
    .a8
    jmp @tile
@done:
    rts
.endproc

; ---- rast_edge: walk (LX0,LY0)-(X1,Y1), recording per-scanline min/max x into
;      XL[y]/XR[y] (Bresenham, same as line but updates extents). ----
.proc rast_edge
    rep #$30
    .a16
    .i16
    lda X1
    sec
    sbc LX0
    bpl @dxp
    eor #$FFFF
    inc a
    sta DX
    lda #$FFFF
    sta SX
    bra @dy
@dxp:
    sta DX
    lda #$0001
    sta SX
@dy:
    lda Y1
    sec
    sbc LY0
    bpl @dyp
    sta DYn
    lda #$FFFF
    sta SY
    bra @err
@dyp:
    eor #$FFFF
    inc a
    sta DYn
    lda #$0001
    sta SY
@err:
    lda DX
    clc
    adc DYn
    sta ERR
@loop:
    ldx LY0                 ; y (0..127)
    sep #$20
    lda LX0                 ; x low byte
    cmp XL,x
    bcs @noL
    sta XL,x
@noL:
    cmp XR,x
    bcc @noR
    beq @noR
    sta XR,x
@noR:
    rep #$20
    lda LX0
    cmp X1
    bne @cont
    lda LY0
    cmp Y1
    bne @cont
    rts
@cont:
    lda ERR
    asl a
    sta E2
    lda E2
    sec
    sbc DYn
    bmi @nox
    lda ERR
    clc
    adc DYn
    sta ERR
    lda LX0
    clc
    adc SX
    sta LX0
@nox:
    lda DX
    sec
    sbc E2
    bmi @noy
    lda ERR
    clc
    adc DX
    sta ERR
    lda LY0
    clc
    adc SY
    sta LY0
@noy:
    bra @loop
.endproc

; ---- dbgblock: fill a fixed 16x16 block at (8,8) colour 1 (DBGMARK probe) ----
.proc dbgblock
    sep #$20
    .a8
    lda #$01
    sta COL
    ldx #8
@y:
    stx PY
    ldy #8
@x:
    sty PX
    phx
    phy
    jsr plotc
    ply
    plx
    iny
    cpy #24
    bne @x
    inx
    cpx #24
    bne @y
    rts
.endproc

; ---- fill_cube: cull back faces, scanline-fill the <=3 visible faces in their
;      paired colours (no z-sort: convex solid, visible faces don't overlap). ----
.proc fill_cube
    stz FI
.ifdef DBGMARK
    jsr dbgblock            ; probe: does fill_cube run + DMA at all?
.endif
    ldx #$0000              ; FACES byte index
@floop:
    rep #$20
    .a16
    ; load the 4 projected verts of this face
    lda FACES+0,x
    and #$00FF
    asl a
    tay
    lda PTS,y
    and #$00FF
    sta FX0
    lda PTS,y
    xba
    and #$00FF
    sta FY0
    lda FACES+1,x
    and #$00FF
    asl a
    tay
    lda PTS,y
    and #$00FF
    sta FX1
    lda PTS,y
    xba
    and #$00FF
    sta FY1
    lda FACES+2,x
    and #$00FF
    asl a
    tay
    lda PTS,y
    and #$00FF
    sta FX2
    lda PTS,y
    xba
    and #$00FF
    sta FY2
    lda FACES+3,x
    and #$00FF
    asl a
    tay
    lda PTS,y
    and #$00FF
    sta FX3
    lda PTS,y
    xba
    and #$00FF
    sta FY3
    phx                     ; save FACES index
    ; backface cull: AREA = (FX1-FX0)*(FY2-FY0) - (FX2-FX0)*(FY1-FY0)
    lda FX1
    sec
    sbc FX0
    sep #$20
    sta MA
    rep #$20
    lda FY2
    sec
    sbc FY0
    sep #$20
    sta MB
    jsr smul
    rep #$20
    lda MP
    sta AREA
    lda FX2
    sec
    sbc FX0
    sep #$20
    sta MA
    rep #$20
    lda FY1
    sec
    sbc FY0
    sep #$20
    sta MB
    jsr smul
    rep #$20
    lda AREA
    sec
    sbc MP
    sta AREA
.ifndef NOCULL
    bmi @draw               ; front-facing (flip to bpl if cube looks inside-out)
    jmp @next
.endif
@draw:
    ; COL = FACECOL[face]
    rep #$20
    txa                     ; (after phx, X still = FACES index)
    lsr a
    lsr a                   ; /4 = face number
    and #$00FF
    clc
    adc CUBEOFF             ; + CUBE*6 -> select this cube's 6-colour set
    tay
    sep #$20
    lda FACECOL,y
    sta COL
    ; plane fill bytes (4bpp -> 4 planes) for the byte-span fill
    stz P0M
    lda COL
    and #$01
    beq @np0
    lda #$FF
    sta P0M
@np0:
    stz P1M
    lda COL
    and #$02
    beq @np1
    lda #$FF
    sta P1M
@np1:
    stz P2M
    lda COL
    and #$04
    beq @np2
    lda #$FF
    sta P2M
@np2:
    stz P3M
    lda COL
    and #$08
    beq @np3
    lda #$FF
    sta P3M
@np3:
    ; sub-layer colour + 2bpp plane fill bytes (Y still = face number)
    lda FACECOL3,y
    sta COL3
    stz Q0M
    lda COL3
    and #$01
    beq @nq0
    lda #$FF
    sta Q0M
@nq0:
    stz Q1M
    lda COL3
    and #$02
    beq @nq1
    lda #$FF
    sta Q1M
@nq1:
    rep #$20
    ; ymin/ymax over the 4 ys
    lda FY0
    sta YMIN
    sta YMAX
    lda FY1
    cmp YMIN
    bcs @a1
    sta YMIN
@a1:
    lda FY1
    cmp YMAX
    bcc @b1
    sta YMAX
@b1:
    lda FY2
    cmp YMIN
    bcs @a2
    sta YMIN
@a2:
    lda FY2
    cmp YMAX
    bcc @b2
    sta YMAX
@b2:
    lda FY3
    cmp YMIN
    bcs @a3
    sta YMIN
@a3:
    lda FY3
    cmp YMAX
    bcc @b3
    sta YMAX
@b3:
    lda YMAX
    inc a
    sta YMAX1
    ; clear extents for [YMIN..YMAX]
    ldx YMIN
@clr:
    sep #$20
    lda #$FF
    sta XL,x
    stz XR,x
    rep #$20
    inx
    cpx YMAX1
    bne @clr
.ifdef STOPCLR
    plx                     ; balance the phx, then bail (probe: did clear/ymin/cull finish?)
    rts
.endif
    ; rasterize the 4 edges into the extents
    lda FX0
    sta LX0
    lda FY0
    sta LY0
    lda FX1
    sta X1
    lda FY1
    sta Y1
    jsr rast_edge
    lda FX1
    sta LX0
    lda FY1
    sta LY0
    lda FX2
    sta X1
    lda FY2
    sta Y1
    jsr rast_edge
    lda FX2
    sta LX0
    lda FY2
    sta LY0
    lda FX3
    sta X1
    lda FY3
    sta Y1
    jsr rast_edge
    lda FX3
    sta LX0
    lda FY3
    sta LY0
    lda FX0
    sta X1
    lda FY0
    sta Y1
    jsr rast_edge
    ; fill spans
    lda YMIN
    sta RY
@row:
    ldx RY
    sep #$20
    lda XL,x
    sta SPL
    lda XR,x
    sta SPR
    cmp SPL
    bcc @rownext            ; XR < XL -> empty row
    rep #$20
    lda RY
    sta PY
    sep #$20
    jsr hspan_fast          ; BG1 4bpp base layer
    jsr hspan_fast3         ; BG3 2bpp sub layer
@rownext:
    rep #$20
    lda RY
    inc a
    sta RY
    cmp YMAX1
    beq @next
    jmp @row
@next:
    plx
    inx
    inx
    inx
    inx
    sep #$20                ; FI is a byte — count in 8-bit, not 16
    .a8
    inc FI
    lda FI
    cmp #6
    beq @done
    jmp @floop
@done:
    rts
.endproc

; ---- spin until the start of vblank (catch the rising edge) ----
.proc wait_vblank
    php
    sep #$20
    .a8
@active:
    lda HVBJOY              ; wait until we are NOT in vblank (in active display)
    bmi @active
@vbl:
    lda HVBJOY             ; then wait for vblank to start
    bpl @vbl
    plp
    rts
.endproc

; ---- DMA the completed fb (WRAM) -> VRAM CHR, during vblank ----
.proc dma_fb
    php
    sep #$20
    .a8
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda CUBE                ; VMADD = cube_bg1_vmadd[CUBE] (this cube's BG1 CHR region)
    asl a
    tax
    lda cube_bg1_vmadd,x
    sta VMADDL
    lda #$0000
    sta A1T0L
    sep #$20
    .a8
    lda #FB_BANK
    sta A1B0
    rep #$20
    .a16
    lda #FB_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #$01
    sta MDMAEN

    ; --- FB3 (2bpp) -> cube CUBE's BG3 CHR region ---
    lda #$80
    sta VMAIN
    rep #$20
    .a16
    lda CUBE
    asl a
    tax
    lda cube_bg3_vmadd,x
    sta VMADDL
    lda #.loword(FB3_ADDR)  ; FB3 at $7F0800 — source offset is $0800, not 0
    sta A1T0L
    sep #$20
    .a8
    lda #^FB3_ADDR
    sta A1B0
    rep #$20
    .a16
    lda #FB3_BYTES
    sta DAS0L
    sep #$20
    .a8
    lda #$01
    sta DMAP0
    lda #$18
    sta BBAD0
    lda #$01
    sta MDMAEN
    plp
    rts
.endproc

.proc nmi_handler
    rti
.endproc

.proc irq_stub
    rti
.endproc

; ------------------------------------------------------------------
bitmasks:
    .byte $80,$40,$20,$10,$08,$04,$02,$01

; cube model vertices: 8 corners at (+-16, +-16, +-16), signed bytes (x,y,z).
; $F0 = -16. r=16 keeps the rotated corner (r*sqrt3 ~= 27.7) inside the 64px
; window around centre 32 (range ~4..60); no bounce now so it can fill the window.
MODELV:
    .byte $F0,$F0,$F0      ; -16,-16,-16
    .byte  16,$F0,$F0      ;  16,-16,-16
    .byte  16, 16,$F0      ;  16, 16,-16
    .byte $F0, 16,$F0      ; -16, 16,-16
    .byte $F0,$F0, 16      ; -16,-16, 16
    .byte  16,$F0, 16      ;  16,-16, 16
    .byte  16, 16, 16      ;  16, 16, 16
    .byte $F0, 16, 16      ; -16, 16, 16

; 12 edges as vertex-index pairs (bottom face, top face, verticals).
EDGES:
    .byte 0,1, 1,2, 2,3, 3,0,  4,5, 5,6, 6,7, 7,4,  0,4, 1,5, 2,6, 3,7

; 6 faces, each 4 vertex indices wound CCW as seen from outside the cube.
; Order: +z, -z, +x, -x, +y, -y.
FACES:
    .byte 4,5,6,7
    .byte 1,0,3,2
    .byte 5,1,2,6
    .byte 0,4,7,3
    .byte 7,6,2,3
    .byte 0,1,5,4
; 4bpp base colour per face, THREE colour sets (cube 0/1/2) of 6 faces each.
; cube 0 = bright ROYGBV (1-6), cube 1 = dark ROYGBV (7-12), cube 2 = greys/mix.
; fill_cube indexes with face + CUBEOFF (=cube*6).
FACECOL:
    .byte  1, 4,  2, 5,  3, 6    ; cube 0: R G O B Y V (bright)
    .byte  7,10,  8,11,  9,12    ; cube 1: same spread, dark
    .byte 13,15, 14,13, 15,14    ; cube 2: white / dark-grey / grey
; BG3 2bpp sub colour per face (1=bright,2=mid,3=dark -> CGRAM 17/18/19), one set
; per cube. The half-add of base+sub gives each face a richer blended tone.
FACECOL3:
    .byte  1, 3,  1, 3,  2, 2    ; cube 0
    .byte  2, 1,  3, 1,  2, 3    ; cube 1
    .byte  1, 1,  2, 2,  3, 3    ; cube 2

; cube-indexed lookup tables (word, index = CUBE*2)
cube_off6:       .word 0, 6, 12              ; FACECOL base = cube*6
cube_bg1_vmadd:  .word $0000, $0400, $0800   ; BG1 4bpp CHR word base per cube
cube_bg3_vmadd:  .word $2000, $2200, $2400   ; BG3 2bpp CHR word base per cube

.include "cube_sintab.inc"

; BG1 tilemap (32x32): three 8x8 cube windows side by side (rows 13..20) + a
; 16x4 colour-grid (rows 2..5, cols 8..23). The grid shows the ~60 blended
; colours: BG1 = base colour per COLUMN (tile 193 + col), BG3 = sub level per ROW
; (see bg3 map) -> half-add = 16 bases x 4 sub levels. Else -> blank tile 192.
tilemap_data:
.repeat 32, ROW
  .repeat 32, COL
    .if (ROW >= 13) && (ROW < 21) && (COL >= 2) && (COL < 10)
      .word ((ROW-13)*8) + (COL-2)            ; cube 0 (tiles 0..63)
    .elseif (ROW >= 13) && (ROW < 21) && (COL >= 12) && (COL < 20)
      .word ((ROW-13)*8) + (COL-12) + 64      ; cube 1 (tiles 64..127)
    .elseif (ROW >= 13) && (ROW < 21) && (COL >= 22) && (COL < 30)
      .word ((ROW-13)*8) + (COL-22) + 128     ; cube 2 (tiles 128..191)
    .elseif (ROW >= 2) && (ROW < 6) && (COL >= 8) && (COL < 24)
      .word 193 + (COL-8)                     ; colour grid: base = column (193..208)
    .else
      .word 192                               ; blank
    .endif
  .endrepeat
.endrepeat

; 16 solid 4bpp swatch tiles (colour 0..15) for the grid columns, tiles 193..208.
base_swatch_chr:
.repeat 16, I
  .repeat 8
    .byte (I & 1) * $FF, ((I >> 1) & 1) * $FF
  .endrepeat
  .repeat 8
    .byte ((I >> 2) & 1) * $FF, ((I >> 3) & 1) * $FF
  .endrepeat
.endrepeat

; 4 solid 2bpp swatch tiles (sub colour 0..3) for the grid rows, BG3 tiles 193..196.
sub_swatch_chr:
.repeat 4, J
  .repeat 8
    .byte (J & 1) * $FF, ((J >> 1) & 1) * $FF
  .endrepeat
.endrepeat

; 16-colour 4bpp base palette: 0 backdrop, 1-6 ROYGBV bright, 7-12 ROYGBV dark,
; 13-15 white/grey/dark-grey. Then BG3 sub palette 4 (CGRAM 16..19).
palette_data:
    .word $0000, $001F, $021F, $03FF, $0360, $7D40, $7C14, $0011
    .word $0111, $0231, $01E0, $44A0, $440B, $7FFF, $4A52, $2108
    .word $0000, $6739, $35AD, $14A5     ; 16: transp, 17/18/19: bright/mid/dark sub

; BG3 tilemap: the same three cube windows (palette 4 = +$1000 so the sub colours
; come from CGRAM 16..19), plus the colour-grid's sub level keyed by ROW (tiles
; 193..196 = sub 0..3). Else -> blank tile 192.
bg3_tilemap_data:
.repeat 32, ROW
  .repeat 32, COL
    .if (ROW >= 13) && (ROW < 21) && (COL >= 2) && (COL < 10)
      .word ((ROW-13)*8) + (COL-2) + $1000        ; cube 0
    .elseif (ROW >= 13) && (ROW < 21) && (COL >= 12) && (COL < 20)
      .word ((ROW-13)*8) + (COL-12) + 64 + $1000  ; cube 1
    .elseif (ROW >= 13) && (ROW < 21) && (COL >= 22) && (COL < 30)
      .word ((ROW-13)*8) + (COL-22) + 128 + $1000 ; cube 2
    .elseif (ROW >= 2) && (ROW < 6) && (COL >= 8) && (COL < 24)
      .word (193 + (ROW-2)) + $1000               ; colour grid: sub = row (193..196)
    .else
      .word 192                                   ; blank
    .endif
  .endrepeat
.endrepeat

.segment "HEADER"
    .byte "MG CUBES DEMO        "
    .byte $20                  ; map mode: LoROM, slow
    .byte $00, $05, $00, $01, $00, $00   ; type ROM, size 32KB, no RAM, NTSC
    .word $AAAA, $5555

.segment "VECTORS"
    .word $0000,$0000,$0000
    .word nmi_handler       ; FFEA native NMI
    .word $0000
    .word irq_stub          ; FFEE native IRQ
    .word $0000,$0000
    .word $0000,$0000,$0000
    .word nmi_handler       ; FFFA emu NMI
    .word reset_handler     ; FFFC
    .word irq_stub          ; FFFE
