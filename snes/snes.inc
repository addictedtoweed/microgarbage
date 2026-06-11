; ============================================================
;  snes.inc — the SNES registers this kernel touches (subset).
;  Public domain (CC0). No warranty.
; ============================================================

; --- PPU ($21xx) ---
INIDISP   = $2100      ; b7 force-blank, b0-3 brightness
OBSEL     = $2101      ; OBJ size/base
BGMODE    = $2105      ; BG mode + tile-size bits
MOSAIC    = $2106      ; mosaic enable + size
BG1SC     = $2107      ; BG1 tilemap base + screen size
BG2SC     = $2108      ; BG2 tilemap base + screen size
BG3SC     = $2109      ; BG3 tilemap base + screen size
BG4SC     = $210A      ; BG4 tilemap base + screen size
BG12NBA   = $210B      ; BG1/BG2 char base
BG34NBA   = $210C      ; BG3/BG4 char base
BG1HOFS   = $210D      ; BG1 horizontal scroll (write-twice)
BG1VOFS   = $210E      ; BG1 vertical scroll
BG2HOFS   = $210F      ; BG2 horizontal scroll
BG2VOFS   = $2110      ; BG2 vertical scroll
BG3HOFS   = $2111      ; BG3 horizontal scroll
BG3VOFS   = $2112      ; BG3 vertical scroll
BG4HOFS   = $2113      ; BG4 horizontal scroll
BG4VOFS   = $2114      ; BG4 vertical scroll
VMAIN     = $2115      ; VRAM addr increment mode
VMADDL    = $2116      ; VRAM word address low
VMADDH    = $2117      ; VRAM word address high
VMDATAL   = $2118      ; VRAM data write low
VMDATAH   = $2119      ; VRAM data write high
OAMADDL   = $2102      ; OAM word address low
OAMADDH   = $2103      ; OAM word address high (bit 0 useful; bit 7 = priority rotate)
OAMDATA   = $2104      ; OAM data write
CGADD     = $2121      ; CGRAM word address
CGDATA    = $2122      ; CGRAM data write
TM        = $212C      ; main-screen layer enable
TS        = $212D      ; sub-screen layer enable

; --- Mode 7 PPU registers (write-twice 8-bit each) ---
M7SEL     = $211A      ; wrap / fill / flip
M7A       = $211B      ; matrix a (16-bit, low then high)
M7B       = $211C      ; matrix b
M7C       = $211D      ; matrix c
M7D       = $211E      ; matrix d
M7X       = $211F      ; center x (13-bit signed)
M7Y       = $2120      ; center y

; --- CPU / DMA ($42xx, $43xx) ---
NMITIMEN  = $4200      ; b7 NMI enable, b0 auto-joypad read, b4 HIRQ, b5 VIRQ
HTIMEL    = $4207      ; H-counter IRQ target low byte
HTIMEH    = $4208      ; H-counter IRQ target high byte (bit 0)
VTIMEL    = $4209      ; V-counter IRQ target low byte
VTIMEH    = $420A      ; V-counter IRQ target high byte (bit 0)
JOYSER0   = $4016      ; manual joypad serial port 1 (write latch / read bits)
JOYSER1   = $4017      ; manual joypad serial port 2
RDNMI     = $4210      ; read to acknowledge NMI (b7 = vblank flag)
HVBJOY    = $4212      ; b7 vblank, b0 auto-joypad busy
JOY1L     = $4218      ; auto-joypad pad 1 low  (unused when auto-read is off)
JOY1H     = $4219      ; auto-joypad pad 1 high (unused when auto-read is off)
MDMAEN    = $420B      ; general-DMA enable (1 bit per channel)
HDMAEN    = $420C      ; HDMA enable (1 bit per channel)

; HDMA channel 7 registers — kernel reserves for INIDISP letterbox
DMAP7     = $4370      ; transfer pattern + direction
BBAD7     = $4371      ; B-bus address (low byte of $21xx target)
A1T7L     = $4372      ; HDMA table address low
A1T7H     = $4373      ; HDMA table address high
A1B7      = $4374      ; HDMA table bank

; HDMA channels 0..6 base — each channel's regs are at $43X0..$43XF.
; Stride is $10 per channel (X = channel index).
HDMA_CH_BASE = $4300   ; channel 0 base; channel C is at HDMA_CH_BASE + C*$10

; DMA channel 0 registers
DMAP0     = $4300      ; transfer pattern + direction
BBAD0     = $4301      ; B-bus address ($21xx low byte)
A1T0L     = $4302      ; A-bus address low
A1T0H     = $4303      ; A-bus address high
A1B0      = $4304      ; A-bus bank
DAS0L     = $4305      ; byte count low
DAS0H     = $4306      ; byte count high

; DMA channel 1 registers
DMAP1     = $4310
BBAD1     = $4311
A1T1L     = $4312
A1T1H     = $4313
A1B1      = $4314
DAS1L     = $4315
DAS1H     = $4316
