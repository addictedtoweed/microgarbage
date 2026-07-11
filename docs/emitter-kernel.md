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

## Sequencing
1. Emitter core (direct-DMA + ISR skeleton + patch-entry) + kernel ISR-mode + $0E00
   stub. 2. cube3d emitter → cube on screen. 3. FMV player (band + partial-OAM +
   mouse/crosshair as extra baked DMA). 4. Remaining demos + dev-kit skeleton emitter.
5. Kill the kernel entry indirect + state machine.
