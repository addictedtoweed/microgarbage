/* ============================================================
 *  mg_nmi.h — guest-side NMI handler builder (Phase 2)
 *
 *  Each .elf can install its own 65816 NMI handler tailored to
 *  its frame shape, replacing the generic kernel NMI. The guest
 *  composes the handler by calling emit_* primitives, then ships
 *  it to the host via mg_nmi_install which invokes the
 *  SYS_MG_NMI_INSTALL ecall. The host stages the bytes into the
 *  cart-window NMI region and bumps the version byte; the SNES
 *  kernel's @loop polls the version, MVN-copies the new code to
 *  K_NMI_CODE_BASE ($0E00), and rewrites RAMVEC_NMI to point at
 *  the freshly-copied handler.
 *
 *  Design point: emit primitives are mid-level coherent blocks
 *  (prologue, frame-gate, DMA-walk, etc.) rather than per-opcode
 *  helpers. This keeps cycle accounting tractable and the API
 *  ergonomic. Per-opcode emit_imm8/store/etc are provided for
 *  the cases where a specific inline register write is needed
 *  (e.g. BG12NBA flip for FMV double-buffer).
 *
 *  Cycle accounting is in 65816 CPU cycles (worst-case, native
 *  mode E=0). The validator multiplies by 8 master/cycle (slowrom
 *  conservative) and compares against vblank + force_blank budget.
 *
 *  USAGE:
 *      MgNmi b;
 *      mg_nmi_begin(&b);
 *      mg_nmi_emit_prologue(&b);
 *      mg_nmi_emit_frame_ready_gate(&b);
 *      mg_nmi_emit_inidisp(&b, 0x80);     // force blank
 *      mg_nmi_emit_dma_list_walk(&b);
 *      mg_nmi_emit_inidisp(&b, 0x0F);     // unblank
 *      mg_nmi_emit_out_label(&b);
 *      mg_nmi_emit_epilogue(&b);
 *      if (mg_nmi_finish(&b, 16) < 0) mg_panic("nmi overrun");
 *      mg_nmi_install(&b);
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef MG_NMI_H
#define MG_NMI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cart-window NMI region is 1 KB. Builder buffer is the same
 * size — emit beyond this sets MgNmi.err and subsequent emits
 * are no-ops. */
#define MG_NMI_BUF_SIZE        1024u

/* The SNES kernel copies installed code to K_NMI_CODE_BASE in WRAM. */
#define MG_NMI_LOAD_ADDR       0x0E00u

/* Errors (negative). */
#define MG_NMI_OK              0
#define MG_NMI_E_BUF_FULL     -1
#define MG_NMI_E_CYCLES       -2
#define MG_NMI_E_INSTALL      -3
#define MG_NMI_E_MISORDERED   -4   /* emit_out_label after epilogue, etc */

typedef struct {
    uint8_t  buf[MG_NMI_BUF_SIZE];
    uint16_t used;             /* bytes emitted */
    uint16_t cycles;           /* worst-case CPU cycles */
    uint16_t out_label_off;    /* offset of @out target (set by emit_out_label) */
    uint16_t out_fixup_off;    /* offset of jmp target byte to patch (set by emit_frame_ready_gate) */
    uint8_t  has_out_label;    /* set by emit_out_label */
    uint8_t  has_out_fixup;    /* set by emit_frame_ready_gate */
    int8_t   err;              /* first error (0 = ok) */
} MgNmi;

/* Reset builder. */
void mg_nmi_begin(MgNmi *b);

/* Emit standard prologue: rep #$30, pha, phx, phy, sep #$20,
 * lda f:$004210 (ack RDNMI). Caller arrives with M=1, X=1 per
 * SNES NMI convention; on return M=1, X=0, A and X/Y saved. */
void mg_nmi_emit_prologue(MgNmi *b);

/* Emit frame-ready gate: lda f:COPRO_FRAME_RDY_L; bne @do_frame;
 * jmp @out. The jmp target is patched when emit_out_label runs.
 * On fall-through path M=1, X=0. */
void mg_nmi_emit_frame_ready_gate(MgNmi *b);

/* Emit INIDISP write (lda #val; sta $2100). Use 0x80 for force
 * blank, 0x0F for unblank-full-brightness. M=1 required. */
void mg_nmi_emit_inidisp(MgNmi *b, uint8_t val);

/* Emit immediate 8-bit store to absolute address: lda #val; sta
 * $00:abs. M=1 required. Useful for inline PPU register writes
 * (BG12NBA, BG1SC, NMITIMEN, etc.). */
void mg_nmi_emit_store_imm8(MgNmi *b, uint16_t abs_addr, uint8_t val);

/* Emit immediate 16-bit store to absolute address: rep#$20; lda
 * #val16; sta $00:abs; sep#$20. Useful for VMADDL/H pair, BG
 * scrolls, etc. */
void mg_nmi_emit_store_imm16(MgNmi *b, uint16_t abs_addr, uint16_t val);

/* Emit the 8-slot DMA-list walker. Mirrors kernel.s nmi @slot
 * loop: per slot, programs channel 0 from COPRO_DMA_LIST_L+slot*8,
 * dispatches prep value to CGADD/VMADDL/OAMADDL based on bbus byte,
 * fires MDMAEN. ~110 bytes, ~640 worst-case cycles for 8 used slots
 * (most demos use 3-4, so typical ~250 cycles). M=1, X=0 required. */
void mg_nmi_emit_dma_list_walk(MgNmi *b);

/* Mark current emit position as @out (the address the frame-ready
 * gate jumps to). Patches the gate's jmp target placeholder. Must
 * be called BEFORE emit_epilogue if frame_ready_gate was emitted. */
void mg_nmi_emit_out_label(MgNmi *b);

/* Emit epilogue: rep#$30; ply; plx; pla; rti. If emit_out_label
 * was not called and emit_frame_ready_gate was, auto-calls
 * emit_out_label here (gate jumps directly to epilogue). */
void mg_nmi_emit_epilogue(MgNmi *b);

/* Validate. force_blank_lines is the per-frame force-blank window
 * (mg_force_blank top+bottom total). Returns MG_NMI_OK or negative
 * MG_NMI_E_*. */
int  mg_nmi_finish(MgNmi *b, uint8_t force_blank_lines);

/* Ship the assembled handler to the host. Returns MG_NMI_OK or
 * MG_NMI_E_INSTALL. After install, the SNES kernel's @loop will
 * notice the version bump within one frame and switch over. */
int  mg_nmi_install(const MgNmi *b);

/* Introspection (for self-tests + cycle-tuning). */
uint16_t mg_nmi_used_bytes(const MgNmi *b);
uint16_t mg_nmi_used_cycles(const MgNmi *b);

/* ============================================================
 *  HIRQ builder (Phase 2.5)
 *
 *  An HIRQ handler fires per-scanline during the configured siphon
 *  window. ISR budget is ~340 master cycles (one HBLANK) including
 *  interrupt entry, ISR body, and RTI. Typical ISR body fires one
 *  small CPU DMA from cart-window source bytes to WRAM staging.
 *
 *  Phase 2.5a ships only the install plumbing — guest stages raw
 *  bytes via mg_hirq_install and the kernel @loop poll copies them
 *  into WRAM at $1200 and rewrites the IRQ vector. The cycle-counted
 *  emit primitives + HBLANK-budget validator + HIRQ scheduler config
 *  land in Phase 2.5b.
 * ============================================================ */

#define MG_HIRQ_BUF_SIZE       1024u
#define MG_HIRQ_LOAD_ADDR      0x1200u

typedef struct {
    uint8_t  buf[MG_HIRQ_BUF_SIZE];
    uint16_t used;
    uint16_t cycles;           /* worst-case CPU cycles per fire */
    int8_t   err;
} MgHirq;

/* Reset builder. */
void mg_hirq_begin(MgHirq *b);

/* Emit raw bytes — Phase 2.5a back-door for testing the install path
 * with hand-assembled handlers. Returns the buffer offset of the
 * first emitted byte, or UINT16_MAX on overflow. */
uint16_t mg_hirq_emit_raw(MgHirq *b, const uint8_t *bytes, uint16_t n);

/* Validate. Returns MG_NMI_OK or negative MG_NMI_E_*. */
int  mg_hirq_finish(MgHirq *b);

/* Ship the assembled handler to the host via SYS_MG_HIRQ_INSTALL.
 * Returns MG_NMI_OK or MG_NMI_E_INSTALL. */
int  mg_hirq_install(const MgHirq *b);

uint16_t mg_hirq_used_bytes(const MgHirq *b);
uint16_t mg_hirq_used_cycles(const MgHirq *b);

/* mg_hirq_configure / mg_hirq_disable were removed in v2.29 — the
 * Phase 2.5b cart_window-driven HIRQ schedule was superseded by
 * Phase 3a's NMI-driven unified ISR. Use mg_kernel_layout for
 * letterbox / mg_siphon_configure for per-scanline DMA siphon. */

#ifdef __cplusplus
}
#endif

#endif /* MG_NMI_H */
