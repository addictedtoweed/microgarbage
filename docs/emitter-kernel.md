# Full-emitter kernel — the SNES as pure DMA arbiter

Locked 2026-07-11 (John). Supersedes the descriptor-walk transport for the 3D /
dead-simple-kernel path. See memory `dead-simple-kernel`, `nmi-builder-architecture`.

## Principle
The RISC-V coprocessor emits the ENTIRE per-frame 65816 virtual-NMI routine and is
liable for its own DMA. The SNES kernel is boot + PPU baseline + joypad mailbox +
the guest-code install path + a safe IRQ stub. Nothing per-frame is kernel logic.
"The SNES exists only as a DMA arbiter." No state machine, no cycle chainer, no
descriptor slot walk, no kernel letterbox/siphon logic.

## Vectoring (no trampoline)
- v-blank NMI stays OFF; timing is the H/V counter IRQ (NMITIMEN=$30, HTIME=22 set
  once at boot). The guest ISR re-arms VTIME each fire for the next event line.
- Native IRQ vector → fixed WRAM entry `$0E00` (already proven executable via the
  install path). Entry holds a `JMP abs` ($4C lo hi). To swap routines the guest
  PATCHES the 2 target bytes at $0E01/$0E02 — one direct jump per IRQ, no `jmp (ptr)`.
- TRANSITION: may keep the kernel's one `jmp (RAMVEC_IRQ)` entry indirect and set
  RAMVEC_IRQ=$0E00, so state-machine demos still work. Once all demos migrate, point
  the ROM vector straight at $0E00 and delete RAMVEC_IRQ + the state machine.

## Kernel (final shape)
- boot: WRAM init, PPU force-blank + clear baseline, build ABI jump-table, arm H/V
  IRQ (NMITIMEN/HTIME/VTIME), seed a SAFE stub at $0E00 (`ack TIMEUP; rearm VTIME;
  rti`) so a pre-install IRQ can't run garbage.
- install poll (@loop): on COPRO_NMI_VERSION bump, copy COPRO_NMI_CODE → $0E00 (K_
  NMI_CODE_BASE) and, in ISR mode, point the vector at $0E00.
- joypad: bit-bang + mailbox as an ABI routine `K_ABI_JOYPAD` the guest ISR `jsr`s.
- ABI jump-table: K_ABI_JOYPAD (new) + keep whatever proven helpers stay useful.

## Guest emitter (mg_nmi, expanded)
Reuse the legacy full-ISR primitives (emit_prologue / emit_inidisp / emit_store_imm8
/ emit_store_imm16 / emit_epilogue-RTI). REPLACE emit_dma_list_walk with:
- `emit_dma_direct(bbus, dmap, src16, size16, prep16)` — bakes one channel-0 DMA as
  immediates (BBAD0/DMAP0/A1T0L/A1B0/DAS0L + prep→VMADDL/CGADD + MDMAEN). No cart
  descriptor read.
- `emit_ack` (lda f:TIMEUP), `emit_arm_vtime(line)` (VTIMEL/H), `emit_patch_entry
  (target)` (write $0E01/2), `emit_strobe(abs)` (lda f:abs, e.g. FRAME_DONE),
  `emit_call_abi(addr)` (jsr), `emit_isr_end` (rep#$30; ply;plx;pla; rti).
Guest composes N routines into the $0E00 image; each routine ends by patching the
entry to its successor. Emitter tracks per-routine offsets so patch targets resolve
to absolute WRAM addresses ($0E00 + offset).

## cube3d slice (first proof)
Emit: entry `JMP` + 4 finish routines (one per band, baked BG1+BG3 src/size/VRAM
dest for that band's tile range) + 1 start routine.
- finish_bN (V=finish_line): force-blank; DMA band N (BG1 + BG3, baked); strobe
  FRAME_DONE; set WRAM NEXT_FINISH = finish_b{next in order 1,2,3,0}; patch entry →
  start; arm VTIME=top_lb; rti.
- start (V=top_lb): unblank; CGADD reset; jsr K_ABI_JOYPAD; patch entry → NEXT_FINISH;
  arm VTIME=finish_line; rti.
Copro (copro_r3d): stage the static PPU config (BGMODE/SC/NBA/TM/TS/colour-math) +
tilemap + palette ONCE (via a one-shot emitted init routine or the kernel ABI), then
each frame re-render + refill the 4 bands' CHR at their fixed cart-window offsets. The
emitted finish routines' baked src offsets point at those. Centre later via PB_SCROLLS
(-8) once displaying.

## Tear-free 240×200 20 fps (thirds double-buffer, overlapping bases)
Locked 2026-07-11 (John): full 240×200 60-colour, tear-free, commits ALL 64 KB VRAM
(OBJ off). Two SNES limits force the scheme: VRAM 32768 words AND the 10-bit tilemap
tile index (1024 tiles/BG). Thirds (3 bands, from 3 subframes @20 fps) need only
1.67 buffers; the tile index is beaten by OVERLAPPING the two CHR bases so the shared
band lives in the overlap, reachable from both frames' bases at different indices.

VRAM word map (0x0000-0x7FFF):
- BG1 4bpp CHR (base step 0x1000w, two bases 0x2000 apart):
  mid_A 0x0000 | bot_A 0x1000 | top 0x2000(SHARED) | mid_B 0x3000 | bot_B 0x4000
  base_A=0x0000 (BG12NBA=0)   base_B=0x2000 (BG12NBA=2)
- BG3 2bpp CHR (base step 0x1000w, two bases 0x1000 apart):
  mid3_A 0x5000 | bot3_A 0x5800 | top3 0x6000(SHARED) | mid3_B 0x6800 | bot3_B 0x7000
  base3_A=0x5000 (BG34NBA=5)  base3_B=0x6000 (BG34NBA=6)
- Tilemaps (SHARED by BG1+BG3, palette field 4): tilemap_A 0x7800, tilemap_B 0x7C00.
Tile indices (identical BG1 & BG3): frame A mid->0/bot->256/top->512; frame B
top->0/mid->256/bot->512. Full VRAM used; ~720w of alignment gaps at unused indices
(250-255 etc.) hold the reserved blank tile.

Shared tilemap keeps 60 colours: 4bpp scales palette ×16, 2bpp ×4, so palette field 4
lands them NON-overlapping — BG1 hues -> CGRAM 64-79, BG3 brightness -> CGRAM 16-19.

Reveal on subframe 3 (last/shared band, during force-blank) = 4 register writes:
  show A: BG12NBA=0 BG34NBA=5 BG1SC=BG3SC=0x7800(>>? SC field 0x400w units)
  show B: BG12NBA=2 BG34NBA=6 BG1SC=BG3SC=0x7C00
Delivery: bottom-2 thirds into the building frame's off-screen slots (subframes 1-2),
top/shared third into the shared slot during subframe-3 blank, then flip -> atomic.
ISR fits 1 KB via SELF-MODIFY: one set of 3 finish routines whose VMADDL dest
immediates + the reveal registers are PATCHED by the reveal each frame to ping-pong
A<->B (no 2nd baked routine set). ~4 B/line siphon tops off the 62-line burst per
third (11160 B burst, 12000 B/third). Renderer: NBANDS=3, thirds of 250 tiles.

## Integration (copro_r3d.c) — Stage A LANDED (green build), Stage B (siphon) TODO
DONE + correct: overlapping-base VRAM constants, palette-1 CGRAM (build_palette:
hues->16-31, brightness->4-7), two-parity shared tilemaps (fill_tilemaps: tmap_idx_A/B
with the overlap index math), cart-window layout (R3D_STAGE_BG1 0x0000 / R3D_STAGE_BG3
0x2000 / TMAP_A 0x3000 / TMAP_B 0x3800 / PAL 0x4000 / mailbox R3D_MB 0x4100:
bg1dst u16, bg3dst u16, reveal u8, nba1/nba3/sc u8).

DONE render (copro_r3d_render): s_build_parity(0=A,1=B) + s_deliver(0..2). Per subframe
third = s_deliver_order[sub] (2=bot,1=mid,0=top). Stages s_chr[third_t0]->R3D_STAGE_BG1
(THIRD_BG1_BYTES), s_chr2->R3D_STAGE_BG3 (THIRD_BG3_BYTES). Mailbox bg1dst/bg3dst =
slot_for(third, build_parity) [third0=BG1_TOP/BG3_TOP shared; mid/bot A/B]. reveal=1
only on third==0, nba1/nba3/sc = build_parity's regs. frame_ready. On wrap flips
s_build_parity + ahead-renders. init-once stages both tilemaps + palette + letterbox
12/12 + install_isr_image; seeds build_parity=1 (init displays A, first frame builds B
-> 1-frame startup glitch, accepted).

DONE Stage-A emitter (install_isr_image = entry JMP + init + ONE mailbox finish + start):
- init: force-blank; NMITIMEN=$20 (V-IRQ only, NMI off, no auto-joy); BGMODE1/TM1/TS4/
  CGWSEL2/CGADSUB$41; BGVOFS -8; display parity A; DMA palette->CGRAM0, both tilemaps
  ->0x7800/0x7C00; entry->start; VTIME=top_lb.
- finish (V=VIS_END): force-blank; e_dma_vram_mb BG1 (src R3D_STAGE_BG1, dest=[MB_BG1DST])
  + BG3 (src R3D_STAGE_BG3, dest=[MB_BG3DST]) BOTH burst in blank; e_reveal (beq on
  [MB_REVEAL], latch BG12NBA/BG34NBA/BG1SC/BG3SC from mailbox); strobe FRAME_DONE;
  entry->start; VTIME=top_lb.
- start (V=top_lb): unblank; stz CGADD; jsr K_ABI_JOYPAD; entry->finish; VTIME=VIS_END.
Helpers added: e_lda_long ($AF), e_dma_vram_mb (VMADDL from cart mailbox), e_reveal.
KNOWN Stage-A defect: 12 KB (BG1 8 KB + BG3 4 KB) burst overruns the ~10.5 KB blank
window by ~9 lines -> ~9-line black bar at the TOP each frame. Proves the architecture
(reveal / mailbox dest / palette-1 / ping-pong) before the siphon.

TODO Stage B (siphon) — reclaim the ~9 lines: split BG1 (finish burst, fits ~8 KB) from
BG3 (per-line H-blank siphon, 4000 B / 200 lines = 20 B/line). finish sets up the BG3
siphon channel ONCE (src R3D_STAGE_BG3, dest [MB_BG3DST]) + seeds count; start enables
per-line H-IRQ (HTIME ~260 right margin, NMITIMEN adds H); emitted siphon HIRQ per line
reloads DAS0, MDMAEN a ~20 B chunk (src/VMADDL auto-advance via WRAM state), dec remain,
on 0 disables H + restores V vector. Recipe: snes/siphon_hdot_test.s (clean at 20 B/line,
OBJ off). Working cube safe at git 59c668e; dbuf3_test proves reveal + palette-1 on ares.

## BANKED idea — per-region BG char-base switching (kills the shared slot)
John, 2026-07-12. The shared top third (overlapping CHR bases) is the root of the
whole tear/delayed-siphon dance: because both display parities read ONE physical
top slot, it can't be double-buffered, so its BG3 must be written after the beam
scans it (the delayed siphon + finish-tail split + hdot tuning). The overlap is NOT
the cause of the current siphon strip (that's H-blank timing/bandwidth), but it IS
why the top is special at all.

Alternative to explore: instead of overlapping bases, rewrite BG12NBA/BG34NBA
($210B/$210C) mid-frame — via HDMA or the per-line H-IRQ we already run — at each
third's top scanline boundary, so every third reads from its OWN non-overlapping,
fully double-buffered CHR base. No shared slot -> no tear -> no delayed siphon /
tail split; the siphon could then run plainly on the off-screen building parity.
Cost/risk: an HDMA channel (or a few H-IRQ writes) for the two base regs; mid-frame
char-base changes must be verified on ares + real HW (char base is latched per
tile fetch, so switching exactly at an 8-line tile-row boundary should be clean, but
this is unusual and untested here). VRAM math also changes (each third its own
0x1000-word BG1 slot x2 parities = 6 slots = 0x6000 words + BG3 + tilemaps — may not
fit 0x8000, so this likely pairs with a smaller letterbox or fewer tilemaps). See
[[3d-renderer-design]]. Bank for after the cube demo ships.

## VERIFIED on ares — snes/dogcat_test.s (standalone reference)
John + 2026-07-12. Bare-metal .sfc (no VM/mgapi, HiROM 128KB, tools/gen_dogcat.c makes
a dog + cat over a non-repeating 60-colour plasma) that runs the EXACT scheme: rolling
thirds, overlapping-base shared-top double-buffer, palette-1 shared tilemap, per-line
BG3 H-blank siphon + finish BG3-tail burst, atomic reveal, flipping dog<->cat. Build:
snes/build-dogcat.ps1. Result: FULL 240x200 60-colour, tear-free, flip-clean, CENTRED,
no top flicker. So the scheme is sound on accurate hardware — the mgapi copro flicker
is a PIPELINE artifact (kernel-trampoline latency / DMA-rate), not the scheme.

Two hard findings from the bring-up:
- TOP-EDGE FIX (the "1 stale pixel row"): force-blank freezes the PPU tile-fetch, so the
  first visible line after a full-line force-blank is stale. The fix is to UNBLANK LATE
  in the scanline (HTIME dot ~240, right margin) instead of at dot 22 — force-blank then
  still covers the left of that line while the fetch for the rows below primes, so line
  12 is clean. PORT: move copro_r3d.c's emitted `start` unblank to a late HTIME. (A
  transparent-tile letterbox — never force-blank near visible — is the other clean fix
  but needs zeroed blank tiles; the late-unblank is far simpler.)
- SIPHON hdot vs ISR latency: the siphon force-blank must land in the true H-blank; a
  heavier ISR (e.g. reading OPVCT) needs the hdot pushed later to compensate.

## Cleaner kernel direction — free-running H-IRQ + OPVCT + WRAM action table
The VTIME/H-V-counter MARCHING (re-arm VTIME every line) was the fragile part. Better:
fixed HTIME, NMITIMEN = H-IRQ every line, each ISR reads the scanline from OPVCT and
dispatches via a WRAM table (0=nothing / 1=turn-on / 2=siphon / 4=burst). No counters;
one table swap = dynamic letterbox (top+bottom bounds, which lines siphon, per-scanline
effects). The copro DMAs a fresh table in each frame during the force-blank window. In
dogcat_test this rendered + removed the top flicker but had a delivery bug traced to the
OPVCT high/low read-toggle protocol — nail it in an INSTRUMENTED env (mgapi side, with
logging/screenshots), not blind .sfc iteration. This is the target microgarbage kernel:
dynamic letterbox DMA budget + H-blank siphon + free per-scanline H-IRQ effects.

## Coprocessor time budget + cart-busy sentinel (worked out 2026-07-12)
For a 20 fps 240x200 60-colour frame the SNES pulls ~40 KB/logical-frame (BG1 24 KB +
BG3 12 KB + tilemaps/palette) at ~163 B/line GP-DMA = ~245 lines-equiv of cart reads
out of 786 lines (3 subframes) => cart demanded only ~31% of the logical frame, ~69%
FREE for the M7. So a 50% (131-line) CONTIGUOUS quiet block in the first subframe is a
scheduling problem, not a bandwidth one: keep the BG1 burst in the blank edges, defer
the BG3 siphon to the subframe's back half / subframes 2-3, and lines ~12-142 are
cart-quiet (~8.3 ms; ~2M M7 cycles even at a stalled ~240 MHz effective, ~4M at 480).
For guaranteed 20 fps, render one logical frame AHEAD so a compute overrun slips
delivery a subframe (acceptable dip) instead of tearing. Synergy: a WIDER siphon =>
fewer siphon lines => later SIP_FIRST => the siphon vacates the TOP of the subframe,
front-loading the quiet block. (We stay at 28 B/line — proven clean at the early
no-spin hdot 240 on ares + bsnes-plus; not widening, so no gamble.)

Cart-busy SENTINEL (robust to copro lockup): the ISR runs from WRAM (LowRAM `$0000-
$1FFF`, e.g. `$0E00` or `$1111`) so it's live even while the copro holds the cart bus.
It reads a 16-bit cart-window sentinel via `lda f:$C0xxxx` (24-bit long load); the copro
publishes a READY value with DISTINCT bytes and a bus read during lockup returns the
repeated-byte pattern `0xXYXY`. ISR check = read sentinel, compare hi vs lo; equal =>
BUSY => skip this frame's cart DMA and hold the last complete frame (rti), else pull
the fresh third. Graceful degradation, no tearing. NOTE: the ROM vectors are 16-bit
`.word`s and the CPU forces PB=`$00` on IRQ, so the vector points at a bank-`$00`
trampoline (`jmp (ramvec)` -> `$00:xxxx` WRAM); only the sentinel/cart reach is 24-bit.

Width-sweep test rigs: snes/dogcat_test.s is `-D SIP_BYTES` / `-D SIP_LINES` /
`-D NO_TAIL` / `-D HTIME_SIP` overridable (default build = the committed 28+tail
reference, unchanged). For a PURE siphon-ceiling test set SIP_LINES*SIP_BYTES >= 4000 +
`-D NO_TAIL` so the finish burst stays constant BG1-only (else a smaller width -> bigger
tail -> bigger finish burst is what flickers, NOT the siphon). siphon_hblank_test.s
(`-D BYTES_PER_LINE`) is the diagonal-pattern equivalent for real-hardware (sd2snes) checks.

## Real-hardware finding (2026-07-13) — the siphon fails where emulators are clean
Tested dogcat_test.sfc on a real SNES + CRT: the scheme WORKS INTERMITTENTLY ("worked
for a short time") but the per-line SIPHON corrupts where both ares + bsnes-plus were
clean. BG1 (hue, burst-delivered in the blank edges) lands fine — faces + colour survive.
BG3 (brightness, siphon-delivered) fails — the fine plasma scrambles to noise and a hard
BLACK rectangle appears lower-left. The glitch region == the siphon-active DISPLAY region
(lines ~100-213); the top third (scanned out before the siphon starts) stays clean.
Cause: the mid-line force-blank + 28B GP-DMA + unblank OVERRUNS the real H-blank window,
so the unblank slips into the next line (left force-blank strip) and DMA writes land
during active display (dropped/scrambled BG3). It is NOT IRQ jitter — siphon_isr is
already WAI-anchored (main loop = `wai`/`bra`, CPU halted when the H-IRQ fires) with a
branchless path to the force-blank, so IRQ->force-blank is already a constant cycle count.
It's purely the DMA overrunning the (stricter-than-emulated) real ceiling.

Two-axis hardware sweep built (snes/gen/dogcat_hsweep/): dogcat_h{240..200}.sfc (28B) +
dogcat_b24_h{240..200}.sfc (24B). Moving HTIME_SIP LEFT starts force-blank+DMA+unblank
earlier so it finishes before the next line (fixes the left strip); too far left blanks
the image's RIGHT edge instead — the fix is the sweet spot between. 24B shifts load off
the failing siphon onto the working finish burst (bigger tail, ends ~line 8). MASTER
MEASUREMENT still owed: run snes/dma_rate_test.s on silicon for the REAL per-line H-blank
byte ceiling (emulator claimed ~163 B/line; silicon is clearly stricter) — that number
forks the fix between hdot-nudge / line-redistribution / depth-reduction.

## Subframe release counter + vector-swap (per-TV-frame sequencing)
A logical (20 fps) frame = 3 TV frames = subframes 0/1/2. Only subframe 0 needs to touch
the RISC-V/M7 side; 1 and 2 are pure delivery. Encode that asymmetry so the M7's quiet
block on subframe 0 is as large as possible and 1-2 spend ~nothing on housekeeping.

Kernel state: a `subframe` counter (0->1->2->0) in WRAM, advanced once per TV frame at the
FRAME_DONE point. Per-subframe behaviour, gated on the counter:
- **Subframe 0 (full):** read joypad -> K_ABI_JOYPAD mailbox; check the cart-busy sentinel;
  release the cart so the M7 gets its contiguous quiet block (lines ~12-142); deliver the
  BOTTOM third (burst BG1 + siphon BG3).
- **Subframe 1 (delivery-only):** NO joypad, NO cart release, NO M7 signal. Deliver the MID
  third. The only kernel act is the vector swap.
- **Subframe 2 (delivery-only + reveal):** deliver the TOP third (delayed/constrained
  siphon — the shared VRAM) and REVEAL the built parity. Vector swap back to 0.

Vector swap: all three subframes' ISR images are precomputed/emitted, so switching frames
is just repointing `ramvec` (2 bytes in DP/WRAM) at the next pattern's entry — a handful of
instructions, not a re-setup. Budget ~10 lines max for the swap; everything else on
subframes 1-2 is idle -> DMA/quiet. microgarbage (RISC-V) resets `subframe`=0 on app-load
so a new app always starts clean at the full subframe.

Line-count lever (NOT bytes/line): the per-line byte ceiling is H-blank/PPU-gated, a
hardware constant — freeing the CPU on subframes 1-2 does NOT raise bytes/line, it raises
the number of usable siphon LINES. Subframes 0 & 1 deliver the bottom/mid thirds = the
building parity's PRIVATE, off-screen VRAM (no shared-top tear), so their siphon may span
the whole display (~200 lines) -> ~20 B/line, well under the real ceiling, with margin.
Only subframe 2 (the SHARED top third) needs the delayed window (line ~100+, 28 B/line +
tail). Caveat: spreading the siphon onto more display lines also spreads the per-line
force-blank artifact onto them, so widen ONLY after the per-line timing is proven robust
on silicon (the HTIME sweep). Net: the "24B for margin" happens for free on 2 of 3 frames.

## Sequencing
1. Emitter core (direct-DMA + ISR skeleton + patch-entry) + kernel ISR-mode + $0E00
   stub. 2. cube3d emitter → cube on screen. 3. FMV player (band + partial-OAM +
   mouse/crosshair as extra baked DMA). 4. Remaining demos + dev-kit skeleton emitter.
5. Kill the kernel entry indirect + state machine.
