; ============================================================
;  snes.inc — the SNES registers this kernel touches (subset).
;  Public domain (CC0). No warranty.
; ============================================================

; --- PPU ($21xx) ---
INIDISP   = $2100      ; b7 force-blank, b0-3 brightness
OBSEL     = $2101      ; OBJ size/base
BGMODE    = $2105      ; BG mode + tile-size bits
BG1SC     = $2107      ; BG1 tilemap base + screen size
BG12NBA   = $210B      ; BG1/BG2 char base
VMAIN     = $2115      ; VRAM addr increment mode
VMADDL    = $2116      ; VRAM word address low
VMADDH    = $2117      ; VRAM word address high
VMDATAL   = $2118      ; VRAM data write low
VMDATAH   = $2119      ; VRAM data write high
CGADD     = $2121      ; CGRAM word address
CGDATA    = $2122      ; CGRAM data write
TM        = $212C      ; main-screen layer enable

; --- CPU / DMA ($42xx, $43xx) ---
NMITIMEN  = $4200      ; b7 NMI enable, b0 auto-joypad read
JOYSER0   = $4016      ; manual joypad serial port 1 (write latch / read bits)
JOYSER1   = $4017      ; manual joypad serial port 2
RDNMI     = $4210      ; read to acknowledge NMI (b7 = vblank flag)
HVBJOY    = $4212      ; b7 vblank, b0 auto-joypad busy
JOY1L     = $4218      ; auto-joypad pad 1 low  (unused when auto-read is off)
JOY1H     = $4219      ; auto-joypad pad 1 high (unused when auto-read is off)
MDMAEN    = $420B      ; general-DMA enable (1 bit per channel)

; DMA channel 0 registers (others at +$10)
DMAP0     = $4300      ; transfer pattern + direction
BBAD0     = $4301      ; B-bus address ($21xx low byte)
A1T0L     = $4302      ; A-bus address low
A1T0H     = $4303      ; A-bus address high
A1B0      = $4304      ; A-bus bank
DAS0L     = $4305      ; byte count low
DAS0H     = $4306      ; byte count high
