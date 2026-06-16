; ============================================================
;  dma_test.s — standalone CHR DMA chunk test ROM.
;
;  Reproduces demo_fmv_flip's CHR upload pattern in a pure-SNES
;  HiROM with NO microgarbage / mgapi cart class.
;
;  Setup:
;    * BG1 Mode 1, 4bpp, BG1 CHR base = word $0000
;    * BG1 tilemap @ word $7C00 (TM_A_WORD)
;    * Force-blank throughout init; unblank at end
;    * Tilemap = scan-order cell N → tile N for N=0..1023
;
;  Upload sequence (matching the flip demo):
;    * 6 sequential CPU DMA fires, each 4160 B from ROM to VRAM
;    * Chunk i targets VRAM word i * 2080 (i = 0..5)
;    * Each chunk is its own MDMAEN trigger (separate DMA fires)
;    * All run during force-blank, ~30 scanlines each
;
;  CHR test pattern — each chunk = 130 copies of one "solid color
;  N" 4bpp tile, so the result is a rainbow gradient top-to-bottom:
;
;    Chunk 0 (tiles 0..129)    palette idx 1   RED
;    Chunk 1 (tiles 130..259)  palette idx 2   GREEN
;    Chunk 2 (tiles 260..389)  palette idx 3   YELLOW
;    Chunk 3 (tiles 390..519)  palette idx 4   BLUE
;    Chunk 4 (tiles 520..649)  palette idx 5   MAGENTA
;    Chunk 5 (tiles 650..779)  palette idx 6   CYAN
;
;  If all 6 chunks land in VRAM, you see a clean 6-color rainbow.
;  If chunks 4/5 fail (the bug we're chasing), the bottom of the
;  screen shows black instead of magenta/cyan.
;
;  Load this .sfc in bsnes-plus AND ares; compare results to
;  determine whether the chunk-5 issue is bsnes-plus specific.
;
;  Public domain (CC0). No warranty.
; ============================================================
.p816
.include "snes.inc"

; --- chunk parameters ---
CHR_CHUNK_BYTES = 4160          ; 130 tiles × 32 B/tile
CHR_CHUNK_WORDS = CHR_CHUNK_BYTES / 2     ; 2080
NUM_CHUNKS      = 6

TILEMAP_WORD    = $7C00         ; BG1 tilemap VRAM word address
TILEMAP_BYTES   = 32 * 32 * 2   ; 2048

PAL_BYTES       = 16 * 2        ; 16-color palette = 32 B

.segment "CODE"

; ------------------------------------------------------------------
;  reset_handler — entry at SNES reset.
; ------------------------------------------------------------------
.proc reset_handler
    .a8
    .i8
    sei                         ; mask IRQ
    clc
    xce                         ; native mode
    rep #$38                    ; M=0 (A 16-bit), X=0 (XY 16-bit), D=0
    .a16
    .i16
    ldx #$1FFF
    txs                         ; stack pointer
    sep #$20                    ; A 8-bit
    .a8

    ; DBR = $00 (so abs addressing lands in bank $00 where this ROM is mirrored)
    lda #$00
    pha
    plb

    ; Force-blank during all VRAM/CGRAM uploads
    lda #$80
    sta INIDISP

    ; BG mode 1; BG1 CHR base $0000; BG1 tilemap @ $7C00
    lda #$01
    sta BGMODE
    lda #$7C                    ; BG1SC: (TM_A_WORD>>10)<<2 | size(0)
                                ; = ($7C00/$400)<<2 = 31<<2 = $7C
    sta BG1SC
    stz BG12NBA                 ; BG1 CHR base nibble = 0 (word $0000)

    ; Main screen: BG1 only
    lda #$01
    sta TM

    ; Source bank for DMA channel 0 = $00 (where this ROM lives via HiROM mirror)
    stz A1B0

    ; --- palette upload (CGRAM 32 B) ---
    stz CGADD                   ; CGRAM word index = 0
    stz DMAP0                   ; transfer pattern 0 (1B → 1 reg, A inc)
    lda #<CGDATA                ; $22
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
    sta MDMAEN                  ; fire CGRAM DMA

    ; --- VRAM auto-increment: word-step, inc after VMDATAH ---
    lda #$80
    sta VMAIN

    ; --- 6 CHR chunks ---
    jsr upload_chr_chunks

    ; --- tilemap upload (2 KB to VRAM word $7C00) ---
    rep #$20
    .a16
    lda #TILEMAP_WORD
    sta VMADDL
    sep #$20
    .a8
    lda #$01                    ; pattern 1 (2B → 2 regs ascending)
    sta DMAP0
    lda #<VMDATAL               ; $18
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

    ; Unblank — show the result
    lda #$0F
    sta INIDISP

    ; Idle loop (no NMI used — image is static)
@idle:
    wai
    bra @idle
.endproc

; ------------------------------------------------------------------
;  upload_chr_chunks — fire 6 separate CPU DMA chunks.
;
;  Each chunk: 4160 B from chr_data + i*4160 to VRAM word i*2080.
;  This mimics demo_fmv_flip exactly: 6 independent MDMAEN triggers
;  with sequential VRAM targets.
; ------------------------------------------------------------------
.proc upload_chr_chunks
    .a8
    .i16

    ldx #0                      ; chunk index counter (0..5)
@loop:
    ; All index math in 16-bit mode. txa+tay across MX mismatches
    ; gives undefined high-byte transfers — was the bug behind the
    ; first revision of this test ROM showing chunk 0 OK + chunks
    ; 1-5 garbage.
    rep #$30                    ; M=0 (A 16-bit), X=0 (XY 16-bit)
    .a16
    .i16

    txa                         ; A = chunk_index (full 16-bit)
    asl a                       ; A = chunk_index * 2 (word-table index)
    tay                         ; Y = chunk_index * 2

    lda chunk_vram_word_table,y
    sta VMADDL                  ; 16-bit store: low → $2116, high → $2117

    lda chunk_src_addr_table,y
    sta A1T0L                   ; 16-bit store: low → $4302, high → $4303

    lda #CHR_CHUNK_BYTES
    sta DAS0L                   ; 16-bit store: low → $4305, high → $4306

    sep #$20                    ; A 8-bit for the byte-wide DMA control regs
    .a8

    lda #$01                    ; DMAP pattern 1 (2B → 2 regs ascending)
    sta DMAP0
    lda #<VMDATAL               ; $18
    sta BBAD0

    lda #$01
    sta MDMAEN                  ; fire chunk DMA

    inx
    cpx #NUM_CHUNKS
    bne @loop
    rts
.endproc

; ------------------------------------------------------------------
;  Data tables
; ------------------------------------------------------------------

; VRAM word target for each chunk (i × 2080).
chunk_vram_word_table:
    .word $0000                 ; chunk 0 → word 0
    .word $0820                 ; chunk 1 → word 2080
    .word $1040                 ; chunk 2 → word 4160
    .word $1860                 ; chunk 3 → word 6240
    .word $2080                 ; chunk 4 → word 8320
    .word $28A0                 ; chunk 5 → word 10400

; Source address within bank $00 for each chunk (low 16 bits).
chunk_src_addr_table:
    .word .loword(chr_data + 0 * CHR_CHUNK_BYTES)
    .word .loword(chr_data + 1 * CHR_CHUNK_BYTES)
    .word .loword(chr_data + 2 * CHR_CHUNK_BYTES)
    .word .loword(chr_data + 3 * CHR_CHUNK_BYTES)
    .word .loword(chr_data + 4 * CHR_CHUNK_BYTES)
    .word .loword(chr_data + 5 * CHR_CHUNK_BYTES)

; Scratch (DP).
tmp_word = $00

; ------------------------------------------------------------------
;  Palette: 16 BGR555 entries. Colors 1..6 picked to be distinctive
;  per chunk so the rainbow is obvious.
; ------------------------------------------------------------------
palette_data:
    .word $0000                 ; 0: black (backdrop)
    .word $001F                 ; 1: red    (R=31)
    .word $03E0                 ; 2: green  (G=31)
    .word $03FF                 ; 3: yellow (R=31, G=31)
    .word $7C00                 ; 4: blue   (B=31)
    .word $7C1F                 ; 5: magenta(R=31, B=31)
    .word $7FE0                 ; 6: cyan   (G=31, B=31)
    .word $0000                 ; 7..15: black (unused)
    .word $0000
    .word $0000
    .word $0000
    .word $0000
    .word $0000
    .word $0000
    .word $0000
    .word $0000

; ------------------------------------------------------------------
;  CHR data: 6 chunks × 130 tiles × 32 B = 24960 B.
;
;  Each "solid color N" tile is 32 B of bitplane pattern:
;    bytes 0-15: planes 0+1 interleaved by row (16 B)
;    bytes 16-31: planes 2+3 interleaved by row (16 B)
;
;  For a solid color N tile, each plane is either all $FF (bit set
;  in N) or all $00 (bit clear in N). For 16-color tiles, the
;  4-bit color index N has bits:
;    bit 0 = plane 0, bit 1 = plane 1, bit 2 = plane 2, bit 3 = plane 3
;
;  Macro emits one tile of solid color (specified by which planes).
; ------------------------------------------------------------------

.macro solid_tile p0, p1, p2, p3
    ; 8 rows of (plane 0 byte, plane 1 byte)
    .repeat 8
        .byte p0, p1
    .endrepeat
    ; 8 rows of (plane 2 byte, plane 3 byte)
    .repeat 8
        .byte p2, p3
    .endrepeat
.endmacro

.macro chunk_solid_color p0, p1, p2, p3
    ; 130 copies of the same solid-color tile
    .repeat 130
        solid_tile p0, p1, p2, p3
    .endrepeat
.endmacro

chr_data:
    chunk_solid_color $FF, $00, $00, $00   ; chunk 0: color 1 = red    (plane 0 only)
    chunk_solid_color $00, $FF, $00, $00   ; chunk 1: color 2 = green  (plane 1 only)
    chunk_solid_color $FF, $FF, $00, $00   ; chunk 2: color 3 = yellow (planes 0+1)
    chunk_solid_color $00, $00, $FF, $00   ; chunk 3: color 4 = blue   (plane 2 only)
    chunk_solid_color $FF, $00, $FF, $00   ; chunk 4: color 5 = magenta(planes 0+2)
    chunk_solid_color $00, $FF, $FF, $00   ; chunk 5: color 6 = cyan   (planes 1+2)

; ------------------------------------------------------------------
;  Tilemap: 1024 cells in scan order. Cell N references tile N
;  with palette 0 (the first 16-color palette in CGRAM).
;  Cells 780..1023 reference tiles 780..1023 — those have no CHR
;  uploaded so they render as palette index 0 = backdrop (black).
; ------------------------------------------------------------------
tilemap_data:
    .repeat 1024, i
        .word i & $03FF
    .endrepeat

; ------------------------------------------------------------------
;  SNES HiROM header at $FFC0
; ------------------------------------------------------------------
.segment "HEADER"
    .byte "DMA CHUNK TEST       "    ; 21-char title (14 + 7 spaces)
    .byte $31                          ; HiROM, FastROM
    .byte $00                          ; ROM only
    .byte $07                          ; 128 KB ROM size code (overestimate is fine)
    .byte $00                          ; no SRAM
    .byte $01                          ; region: US
    .byte $00                          ; dev ID
    .byte $00                          ; version
    .word $AAAA                        ; checksum complement (not validated)
    .word $5555                        ; checksum (not validated)

; ------------------------------------------------------------------
;  65816 native-mode vectors at $FFE4..$FFFF
; ------------------------------------------------------------------
.segment "VECTORS"
    .word $0000      ; $FFE4 native COP
    .word $0000      ; $FFE6 native BRK
    .word $0000      ; $FFE8 native ABORT
    .word irq_stub   ; $FFEA native NMI
    .word $0000      ; $FFEC reserved
    .word irq_stub   ; $FFEE native IRQ
    .word $0000      ; $FFF0 reserved
    .word $0000      ; $FFF2 reserved
    .word $0000      ; $FFF4 emul COP
    .word $0000      ; $FFF6 reserved
    .word $0000      ; $FFF8 emul ABORT
    .word irq_stub   ; $FFFA emul NMI
    .word reset_handler  ; $FFFC emul RESET (entry point)
    .word irq_stub   ; $FFFE emul IRQ/BRK

.segment "CODE"
irq_stub:
    rti
