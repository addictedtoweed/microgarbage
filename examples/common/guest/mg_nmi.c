/* ============================================================
 *  mg_nmi.c — see mg_nmi.h
 *
 *  65816 opcode notes:
 *    M=0 means A is 16-bit; M=1 means A is 8-bit.
 *    X=0 means X/Y are 16-bit; X=1 means 8-bit.
 *    Cycle counts assume native mode (E=0). Conservative —
 *    counts always charge the taken-branch cost where it would
 *    increase cycles.
 *
 *  Cart-window symbols mirrored from snes/copro.inc:
 *    COPRO_FRAME_RDY_L = $C07800   (1 byte, non-zero => process)
 *    COPRO_DMA_LIST_L  = $C07808   (8 slots * 8 bytes)
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "mg_nmi.h"

#include "vm_runtime.h"

/* ---------- Constants ---------- */

/* Hardware registers (bank $00, 16-bit absolute). */
#define HW_INIDISP     0x2100
#define HW_OAMADDL     0x2102
#define HW_OAMDATA     0x2104   /* bbus byte */
#define HW_BGMODE      0x2105
#define HW_VMAIN       0x2115
#define HW_VMADDL      0x2116
#define HW_VMDATAL     0x2118   /* bbus byte */
#define HW_CGADD       0x2121
#define HW_CGDATA      0x2122   /* bbus byte */
#define HW_RDNMI       0x4210
#define HW_VTIMEL      0x4209   /* v2.46 H/V-timer IRQ V target low */
#define HW_VTIMEH      0x420A
#define HW_TIMEUP      0x004211u /* read (long) to ack an H/V-timer IRQ */
#define HW_MDMAEN      0x420B
#define HW_DMAP0       0x4300
#define HW_BBAD0       0x4301
#define HW_A1T0L       0x4302
#define HW_A1B0        0x4304
#define HW_DAS0L       0x4305

/* Cart window addresses (24-bit; long-mode lda f:). */
#define CART_FRAME_RDY 0xC07800u
#define CART_DMA_LIST  0xC07808u   /* +0 bbus, +1 dmap, +2-3 src, +4-5 size, +6-7 prep */

/* DMA slot constants. */
#define DMA_LIST_ENTRY_BYTES   8u
#define DMA_LIST_SLOTS         8u
#define DMA_LIST_END_X         (DMA_LIST_ENTRY_BYTES * DMA_LIST_SLOTS)   /* 64 */

/* ---------- Low-level emit helpers ---------- */

static inline void emit_byte(MgNmi *b, uint8_t v) {
    if (b->err) return;
    if (b->used >= MG_NMI_BUF_SIZE) {
        b->err = MG_NMI_E_BUF_FULL;
        return;
    }
    b->buf[b->used++] = v;
}

static inline void emit_word_le(MgNmi *b, uint16_t v) {
    emit_byte(b, (uint8_t)(v & 0xFFu));
    emit_byte(b, (uint8_t)((v >> 8) & 0xFFu));
}

static inline void emit_long_le(MgNmi *b, uint32_t v) {
    emit_byte(b, (uint8_t)(v & 0xFFu));
    emit_byte(b, (uint8_t)((v >> 8) & 0xFFu));
    emit_byte(b, (uint8_t)((v >> 16) & 0xFFu));
}

static inline void add_cycles(MgNmi *b, uint16_t n) {
    if (b->err) return;
    uint32_t sum = (uint32_t)b->cycles + n;
    if (sum > 0xFFFFu) sum = 0xFFFFu;
    b->cycles = (uint16_t)sum;
}

/* Emit jmp $xxxx (absolute, bank $00) targeting buffer offset. */
static void emit_jmp_to_offset(MgNmi *b, uint16_t target_offset) {
    uint16_t target_addr = (uint16_t)(MG_NMI_LOAD_ADDR + target_offset);
    emit_byte(b, 0x4C);
    emit_word_le(b, target_addr);
    add_cycles(b, 3);
}

/* Patch a previously-emitted jmp absolute at the given offset. The 2
 * bytes at offset+1, offset+2 are overwritten with the target's
 * absolute address. */
static void patch_jmp_at(MgNmi *b, uint16_t jmp_op_offset, uint16_t target_offset) {
    if (b->err) return;
    if ((uint32_t)jmp_op_offset + 3u > b->used) {
        b->err = MG_NMI_E_MISORDERED;
        return;
    }
    uint16_t target_addr = (uint16_t)(MG_NMI_LOAD_ADDR + target_offset);
    b->buf[jmp_op_offset + 1] = (uint8_t)(target_addr & 0xFFu);
    b->buf[jmp_op_offset + 2] = (uint8_t)((target_addr >> 8) & 0xFFu);
}

/* ---------- Public API ---------- */

void mg_nmi_begin(MgNmi *b) {
    if (!b) return;
    b->used = 0;
    b->cycles = 0;
    b->out_label_off = 0;
    b->out_fixup_off = 0;
    b->has_out_label = 0;
    b->has_out_fixup = 0;
    b->err = MG_NMI_OK;
}

/* ---------- Body model (v2.44) ---------- */

void mg_nmi_emit_call_default(MgNmi *b) {
    if (!b || b->err) return;
    /* jsr K_ABI_FRAME_DMA   20 lo hi   6 cyc (callee cost accounted per-frame) */
    emit_byte(b, 0x20); emit_word_le(b, (uint16_t)MG_ABI_FRAME_DMA); add_cycles(b, 6);
}

void mg_nmi_emit_call_calc_budget(MgNmi *b) {
    if (!b || b->err) return;
    /* jsr K_ABI_CALC_BYTES_REM   20 lo hi   6 cyc */
    emit_byte(b, 0x20); emit_word_le(b, (uint16_t)MG_ABI_CALC_BYTES_REM); add_cycles(b, 6);
}

void mg_nmi_emit_body_end(MgNmi *b) {
    if (!b || b->err) return;
    /* rts   60   6 cyc */
    emit_byte(b, 0x60); add_cycles(b, 6);
}

void mg_nmi_build_default(MgNmi *b) {
    if (!b) return;
    mg_nmi_begin(b);
    mg_nmi_emit_call_default(b);
    mg_nmi_emit_body_end(b);
}

/* ---------- Legacy full-ISR primitives (pre-v2.44) ---------- */

void mg_nmi_emit_prologue(MgNmi *b) {
    if (!b || b->err) return;
    /* rep #$30        C2 30        3 cyc  M=0, X=0 */
    emit_byte(b, 0xC2); emit_byte(b, 0x30); add_cycles(b, 3);
    /* pha             48           4 cyc  (M=0) */
    emit_byte(b, 0x48); add_cycles(b, 4);
    /* phx             DA           4 cyc  (X=0) */
    emit_byte(b, 0xDA); add_cycles(b, 4);
    /* phy             5A           4 cyc  (X=0) */
    emit_byte(b, 0x5A); add_cycles(b, 4);
    /* sep #$20        E2 20        3 cyc  -> M=1 */
    emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);
    /* lda f:$004210   AF 10 42 00  5 cyc  ack RDNMI */
    emit_byte(b, 0xAF); emit_long_le(b, HW_RDNMI); add_cycles(b, 5);
}

void mg_nmi_emit_frame_ready_gate(MgNmi *b) {
    if (!b || b->err) return;
    /* lda f:$C07800   AF 00 78 C0  5 cyc */
    emit_byte(b, 0xAF); emit_long_le(b, CART_FRAME_RDY); add_cycles(b, 5);
    /* bne +3 (skip jmp)  D0 03    2/3 cyc */
    emit_byte(b, 0xD0); emit_byte(b, 0x03); add_cycles(b, 3);
    /* jmp @out (16-bit abs, patched by emit_out_label)   4C ?? ?? */
    b->out_fixup_off = b->used;
    b->has_out_fixup = 1;
    emit_byte(b, 0x4C); emit_word_le(b, 0x0000); add_cycles(b, 3);
}

void mg_nmi_emit_inidisp(MgNmi *b, uint8_t val) {
    mg_nmi_emit_store_imm8(b, HW_INIDISP, val);
}

void mg_nmi_emit_store_imm8(MgNmi *b, uint16_t abs_addr, uint8_t val) {
    if (!b || b->err) return;
    /* lda #vv         A9 vv        2 cyc  (M=1) */
    emit_byte(b, 0xA9); emit_byte(b, val); add_cycles(b, 2);
    /* sta $xxxx       8D lo hi     4 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, abs_addr); add_cycles(b, 4);
}

void mg_nmi_emit_store_imm16(MgNmi *b, uint16_t abs_addr, uint16_t val) {
    if (!b || b->err) return;
    /* rep #$20        C2 20        3 cyc  -> M=0 */
    emit_byte(b, 0xC2); emit_byte(b, 0x20); add_cycles(b, 3);
    /* lda #$wwww      A9 lo hi     3 cyc  (M=0) */
    emit_byte(b, 0xA9); emit_word_le(b, val); add_cycles(b, 3);
    /* sta $xxxx       8D lo hi     5 cyc  (M=0) */
    emit_byte(b, 0x8D); emit_word_le(b, abs_addr); add_cycles(b, 5);
    /* sep #$20        E2 20        3 cyc  -> M=1 */
    emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);
}

/* ---------- DMA list walk ---------- *
 *
 * Mirrors the kernel.s nmi @slot..@done loop. Per slot:
 *   read bbus byte (skip if 0)
 *   program BBAD0, DMAP0, A1T0L (16-bit src), DAS0L (16-bit size)
 *   dispatch prep:  CGDATA -> CGADD (low byte)
 *                   VMDATAL -> VMAIN $80 + VMADDL (16-bit prep)
 *                   OAMDATA -> OAMADDL (16-bit prep)
 *   fire MDMAEN
 *
 * Internal labels (offsets relative to start of emit):
 *   off_slot     = 5     after rep#$10/ldx#$0000
 *   off_check_v  = 70
 *   off_check_o  = 96
 *   off_fire     = 114
 *   off_next     = 119
 *   off_done     = 130
 *
 * Total: 130 bytes. Worst-case cycles per used slot ~80; 8 slots
 * full ~640 + epilogue branch costs.
 *
 * Pre-conditions:  M=1, X=0 (16-bit)
 * Post-conditions: M=1, X=0
 */
void mg_nmi_emit_dma_list_walk(MgNmi *b) {
    if (!b || b->err) return;

    const uint16_t base = b->used;
    const uint16_t off_slot    = (uint16_t)(base + 5u);
    const uint16_t off_check_v = (uint16_t)(base + 70u);
    const uint16_t off_check_o = (uint16_t)(base + 96u);
    const uint16_t off_fire    = (uint16_t)(base + 114u);
    const uint16_t off_next    = (uint16_t)(base + 119u);
    const uint16_t off_done    = (uint16_t)(base + 130u);

    /* +0  rep #$10              C2 10        3 cyc  -> X=0 (already) */
    emit_byte(b, 0xC2); emit_byte(b, 0x10); add_cycles(b, 3);
    /* +2  ldx #$0000            A2 00 00     3 cyc  (X=0) */
    emit_byte(b, 0xA2); emit_word_le(b, 0x0000); add_cycles(b, 3);
    /* +5  @slot: cpx #DMA_LIST_END_X   E0 40 00    3 cyc */
    emit_byte(b, 0xE0); emit_word_le(b, DMA_LIST_END_X); add_cycles(b, 3);
    /* +8  bne +3                D0 03        3 cyc */
    emit_byte(b, 0xD0); emit_byte(b, 0x03); add_cycles(b, 3);
    /* +10 jmp @done             4C ?? ??     3 cyc */
    emit_jmp_to_offset(b, off_done);
    /* +13 lda f:$C07808,x       BF 08 78 C0  6 cyc  bbus */
    emit_byte(b, 0xBF); emit_long_le(b, CART_DMA_LIST + 0u); add_cycles(b, 6);
    /* +17 bne +3                D0 03        3 cyc */
    emit_byte(b, 0xD0); emit_byte(b, 0x03); add_cycles(b, 3);
    /* +19 jmp @next             4C ?? ??     3 cyc */
    emit_jmp_to_offset(b, off_next);
    /* +22 sta $4301 (BBAD0)     8D 01 43     4 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, HW_BBAD0); add_cycles(b, 4);
    /* +25 lda f:$C07809,x       BF 09 78 C0  6 cyc  dmap */
    emit_byte(b, 0xBF); emit_long_le(b, CART_DMA_LIST + 1u); add_cycles(b, 6);
    /* +29 sta $4300 (DMAP0)     8D 00 43     4 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, HW_DMAP0); add_cycles(b, 4);
    /* +32 rep #$20              C2 20        3 cyc  -> M=0 */
    emit_byte(b, 0xC2); emit_byte(b, 0x20); add_cycles(b, 3);
    /* +34 lda f:$C0780A,x       BF 0A 78 C0  6 cyc  src */
    emit_byte(b, 0xBF); emit_long_le(b, CART_DMA_LIST + 2u); add_cycles(b, 6);
    /* +38 sta $4302 (A1T0L)     8D 02 43     5 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, HW_A1T0L); add_cycles(b, 5);
    /* +41 lda f:$C0780C,x       BF 0C 78 C0  6 cyc  size */
    emit_byte(b, 0xBF); emit_long_le(b, CART_DMA_LIST + 4u); add_cycles(b, 6);
    /* +45 sta $4305 (DAS0L)     8D 05 43     5 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, HW_DAS0L); add_cycles(b, 5);
    /* +48 sep #$20              E2 20        3 cyc  -> M=1 */
    emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);

    /* +50 lda $4301 (BBAD0)     AD 01 43     4 cyc */
    emit_byte(b, 0xAD); emit_word_le(b, HW_BBAD0); add_cycles(b, 4);
    /* +53 cmp #$22 (CGDATA)     C9 22        2 cyc */
    emit_byte(b, 0xC9); emit_byte(b, (uint8_t)(HW_CGDATA & 0xFFu)); add_cycles(b, 2);
    /* +55 beq +3                F0 03        3 cyc */
    emit_byte(b, 0xF0); emit_byte(b, 0x03); add_cycles(b, 3);
    /* +57 jmp @check_v          4C ?? ??     3 cyc */
    emit_jmp_to_offset(b, off_check_v);
    /* +60 lda f:$C0780E,x       BF 0E 78 C0  6 cyc  prep (CGADD low byte) */
    emit_byte(b, 0xBF); emit_long_le(b, CART_DMA_LIST + 6u); add_cycles(b, 6);
    /* +64 sta $2121 (CGADD)     8D 21 21     4 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, HW_CGADD); add_cycles(b, 4);
    /* +67 jmp @fire             4C ?? ??     3 cyc */
    emit_jmp_to_offset(b, off_fire);

    /* @check_v: +70 */
    /* +70 cmp #$18 (VMDATAL)    C9 18        2 cyc */
    emit_byte(b, 0xC9); emit_byte(b, (uint8_t)(HW_VMDATAL & 0xFFu)); add_cycles(b, 2);
    /* +72 beq +3                F0 03        3 cyc */
    emit_byte(b, 0xF0); emit_byte(b, 0x03); add_cycles(b, 3);
    /* +74 jmp @check_o          4C ?? ??     3 cyc */
    emit_jmp_to_offset(b, off_check_o);
    /* +77 lda #$80              A9 80        2 cyc */
    emit_byte(b, 0xA9); emit_byte(b, 0x80); add_cycles(b, 2);
    /* +79 sta $2115 (VMAIN)     8D 15 21     4 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, HW_VMAIN); add_cycles(b, 4);
    /* +82 rep #$20              C2 20        3 cyc  -> M=0 */
    emit_byte(b, 0xC2); emit_byte(b, 0x20); add_cycles(b, 3);
    /* +84 lda f:$C0780E,x       BF 0E 78 C0  6 cyc  prep (16-bit) */
    emit_byte(b, 0xBF); emit_long_le(b, CART_DMA_LIST + 6u); add_cycles(b, 6);
    /* +88 sta $2116 (VMADDL)    8D 16 21     5 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, HW_VMADDL); add_cycles(b, 5);
    /* +91 sep #$20              E2 20        3 cyc  -> M=1 */
    emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);
    /* +93 jmp @fire             4C ?? ??     3 cyc */
    emit_jmp_to_offset(b, off_fire);

    /* @check_o: +96 */
    /* +96 cmp #$04 (OAMDATA)    C9 04        2 cyc */
    emit_byte(b, 0xC9); emit_byte(b, (uint8_t)(HW_OAMDATA & 0xFFu)); add_cycles(b, 2);
    /* +98 beq +3                F0 03        3 cyc */
    emit_byte(b, 0xF0); emit_byte(b, 0x03); add_cycles(b, 3);
    /* +100 jmp @fire            4C ?? ??     3 cyc */
    emit_jmp_to_offset(b, off_fire);
    /* +103 rep #$20             C2 20        3 cyc  -> M=0 */
    emit_byte(b, 0xC2); emit_byte(b, 0x20); add_cycles(b, 3);
    /* +105 lda f:$C0780E,x      BF 0E 78 C0  6 cyc  prep */
    emit_byte(b, 0xBF); emit_long_le(b, CART_DMA_LIST + 6u); add_cycles(b, 6);
    /* +109 sta $2102 (OAMADDL)  8D 02 21     5 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, HW_OAMADDL); add_cycles(b, 5);
    /* +112 sep #$20             E2 20        3 cyc  -> M=1 */
    emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);

    /* @fire: +114 */
    /* +114 lda #$01             A9 01        2 cyc */
    emit_byte(b, 0xA9); emit_byte(b, 0x01); add_cycles(b, 2);
    /* +116 sta $420B (MDMAEN)   8D 0B 42     4 cyc */
    emit_byte(b, 0x8D); emit_word_le(b, HW_MDMAEN); add_cycles(b, 4);

    /* @next: +119 */
    /* +119 inx ×8 (X=0, 2 cyc each = 16 cyc, 8 bytes) */
    for (int i = 0; i < 8; ++i) {
        emit_byte(b, 0xE8); add_cycles(b, 2);
    }
    /* +127 jmp @slot            4C ?? ??     3 cyc */
    emit_jmp_to_offset(b, off_slot);

    /* @done: +130 (no opcode, just a label) */
    (void)off_check_v; (void)off_check_o; (void)off_fire;
    (void)off_next; (void)off_done; (void)off_slot;
}

void mg_nmi_emit_out_label(MgNmi *b) {
    if (!b || b->err) return;
    if (b->has_out_label) {
        b->err = MG_NMI_E_MISORDERED;
        return;
    }
    b->out_label_off = b->used;
    b->has_out_label = 1;
    if (b->has_out_fixup) {
        patch_jmp_at(b, b->out_fixup_off, b->out_label_off);
    }
}

void mg_nmi_emit_epilogue(MgNmi *b) {
    if (!b || b->err) return;
    if (!b->has_out_label) {
        /* Auto-label: gate jumps here. */
        mg_nmi_emit_out_label(b);
        if (b->err) return;
    }
    /* rep #$30        C2 30        3 cyc  -> M=0, X=0 */
    emit_byte(b, 0xC2); emit_byte(b, 0x30); add_cycles(b, 3);
    /* ply             7A           4 cyc */
    emit_byte(b, 0x7A); add_cycles(b, 4);
    /* plx             FA           4 cyc */
    emit_byte(b, 0xFA); add_cycles(b, 4);
    /* pla             68           4 cyc */
    emit_byte(b, 0x68); add_cycles(b, 4);
    /* rti             40           6 cyc */
    emit_byte(b, 0x40); add_cycles(b, 6);
}

/* ---------- Full-emitter ISR primitives (v2.46) ---------- *
 * Build the ENTIRE H/V-counter virtual-NMI: guest owns timing, bars, DMA, vector.
 * All DMA is baked (mg_nmi_emit_dma_direct) — no descriptor read. See mg_nmi.h /
 * docs/emitter-kernel.md. */

void mg_nmi_emit_isr_prologue(MgNmi *b) {
    if (!b || b->err) return;
    emit_byte(b, 0xC2); emit_byte(b, 0x30); add_cycles(b, 3);   /* rep #$30   -> M=0,X=0 */
    emit_byte(b, 0x48); add_cycles(b, 4);                       /* pha */
    emit_byte(b, 0xDA); add_cycles(b, 4);                       /* phx */
    emit_byte(b, 0x5A); add_cycles(b, 4);                       /* phy */
    emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);   /* sep #$20   -> M=1 */
    emit_byte(b, 0xAF); emit_long_le(b, HW_TIMEUP); add_cycles(b, 5); /* lda f:$004211  ack */
}

void mg_nmi_emit_isr_end(MgNmi *b) {
    if (!b || b->err) return;
    emit_byte(b, 0xC2); emit_byte(b, 0x30); add_cycles(b, 3);   /* rep #$30 */
    emit_byte(b, 0x7A); add_cycles(b, 4);                       /* ply */
    emit_byte(b, 0xFA); add_cycles(b, 4);                       /* plx */
    emit_byte(b, 0x68); add_cycles(b, 4);                       /* pla */
    emit_byte(b, 0x40); add_cycles(b, 6);                       /* rti */
}

void mg_nmi_emit_arm_vtime(MgNmi *b, uint8_t line) {
    if (!b || b->err) return;
    emit_byte(b, 0xA9); emit_byte(b, line); add_cycles(b, 2);         /* lda #line */
    emit_byte(b, 0x8D); emit_word_le(b, HW_VTIMEL); add_cycles(b, 4); /* sta $4209 VTIMEL */
    emit_byte(b, 0x9C); emit_word_le(b, HW_VTIMEH); add_cycles(b, 4); /* stz $420A VTIMEH */
}

void mg_nmi_emit_patch_entry(MgNmi *b, uint16_t target_abs) {
    if (!b || b->err) return;
    emit_byte(b, 0xC2); emit_byte(b, 0x20); add_cycles(b, 3);            /* rep #$20 -> M=0 */
    emit_byte(b, 0xA9); emit_word_le(b, target_abs); add_cycles(b, 3);   /* lda #target */
    emit_byte(b, 0x8D); emit_word_le(b, MG_NMI_ENTRY_JMP_OPND); add_cycles(b, 5); /* sta $0E01 */
    emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);            /* sep #$20 -> M=1 */
}

void mg_nmi_emit_call_abi(MgNmi *b, uint16_t abs_addr) {
    if (!b || b->err) return;
    emit_byte(b, 0x20); emit_word_le(b, abs_addr); add_cycles(b, 6);     /* jsr abs */
}

void mg_nmi_emit_strobe(MgNmi *b, uint32_t abs_long) {
    if (!b || b->err) return;
    emit_byte(b, 0xAF); emit_long_le(b, abs_long); add_cycles(b, 5);     /* lda f:abs */
}

void mg_nmi_emit_dma_direct(MgNmi *b, uint8_t bbus, uint8_t dmap,
                            uint16_t src, uint16_t size, uint16_t prep) {
    if (!b || b->err) return;
    /* lda #bbus / sta BBAD0 */
    emit_byte(b, 0xA9); emit_byte(b, bbus); add_cycles(b, 2);
    emit_byte(b, 0x8D); emit_word_le(b, HW_BBAD0); add_cycles(b, 4);
    /* lda #dmap / sta DMAP0 */
    emit_byte(b, 0xA9); emit_byte(b, dmap); add_cycles(b, 2);
    emit_byte(b, 0x8D); emit_word_le(b, HW_DMAP0); add_cycles(b, 4);
    /* rep#$20; lda #src; sta A1T0L; lda #size; sta DAS0L; sep#$20 */
    emit_byte(b, 0xC2); emit_byte(b, 0x20); add_cycles(b, 3);
    emit_byte(b, 0xA9); emit_word_le(b, src); add_cycles(b, 3);
    emit_byte(b, 0x8D); emit_word_le(b, HW_A1T0L); add_cycles(b, 5);
    emit_byte(b, 0xA9); emit_word_le(b, size); add_cycles(b, 3);
    emit_byte(b, 0x8D); emit_word_le(b, HW_DAS0L); add_cycles(b, 5);
    emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);
    /* compile-time prep dispatch — only the path for this bbus is emitted */
    if (bbus == MG_DMA_TO_VRAM) {
        emit_byte(b, 0xA9); emit_byte(b, 0x80); add_cycles(b, 2);         /* lda #$80 */
        emit_byte(b, 0x8D); emit_word_le(b, HW_VMAIN); add_cycles(b, 4);  /* sta VMAIN */
        emit_byte(b, 0xC2); emit_byte(b, 0x20); add_cycles(b, 3);         /* rep #$20 */
        emit_byte(b, 0xA9); emit_word_le(b, prep); add_cycles(b, 3);      /* lda #prep */
        emit_byte(b, 0x8D); emit_word_le(b, HW_VMADDL); add_cycles(b, 5); /* sta VMADDL */
        emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);         /* sep #$20 */
    } else if (bbus == MG_DMA_TO_CGRAM) {
        emit_byte(b, 0xA9); emit_byte(b, (uint8_t)(prep & 0xFFu)); add_cycles(b, 2); /* lda #prep.lo */
        emit_byte(b, 0x8D); emit_word_le(b, HW_CGADD); add_cycles(b, 4);  /* sta CGADD */
    } else if (bbus == MG_DMA_TO_OAM) {
        emit_byte(b, 0xC2); emit_byte(b, 0x20); add_cycles(b, 3);
        emit_byte(b, 0xA9); emit_word_le(b, prep); add_cycles(b, 3);
        emit_byte(b, 0x8D); emit_word_le(b, HW_OAMADDL); add_cycles(b, 5);
        emit_byte(b, 0xE2); emit_byte(b, 0x20); add_cycles(b, 3);
    }
    /* lda #$01 / sta MDMAEN */
    emit_byte(b, 0xA9); emit_byte(b, 0x01); add_cycles(b, 2);
    emit_byte(b, 0x8D); emit_word_le(b, HW_MDMAEN); add_cycles(b, 4);
}

int mg_nmi_finish(MgNmi *b, uint8_t force_blank_lines) {
    if (!b) return MG_NMI_E_INSTALL;
    if (b->err) return b->err;
    if (b->used == 0) return MG_NMI_E_BUF_FULL;

    /* Cycle budget check. Vblank is 38 scanlines, force-blank adds
     * `force_blank_lines` more. One scanline = 1364 master / 8
     * master per CPU cycle = ~170 CPU cycles. We only check that
     * the HANDLER's instruction cycles don't themselves consume
     * the full budget — DMA cycles are accounted for separately
     * per frame by the runtime. Cap handler cycles at HEADROOM. */
    const uint16_t vblank_cyc = 38u * 170u;                   /* ~6460 */
    const uint16_t fb_cyc     = (uint16_t)force_blank_lines * 170u;
    const uint32_t total_cyc  = (uint32_t)vblank_cyc + fb_cyc;
    /* The handler itself runs serially within vblank before DMA
     * transfers start. Pessimistically reserve at most 1/4 of
     * vblank for handler overhead — anything beyond that and the
     * DMA budget shrinks too much. */
    const uint32_t headroom   = total_cyc / 4u;
    if ((uint32_t)b->cycles > headroom) {
        b->err = MG_NMI_E_CYCLES;
        return MG_NMI_E_CYCLES;
    }
    return MG_NMI_OK;
}

int mg_nmi_install(const MgNmi *b) {
    if (!b) return MG_NMI_E_INSTALL;
    if (b->err) return b->err;
    if (b->used == 0) return MG_NMI_E_BUF_FULL;

    int r = (int)_vm_sys2(SYS_MG_NMI_INSTALL,
                          (uint32_t)(unsigned long)b->buf,
                          (uint32_t)b->used);
    if (r < 0) return MG_NMI_E_INSTALL;
    return MG_NMI_OK;
}

uint16_t mg_nmi_used_bytes(const MgNmi *b)  { return b ? b->used : 0u; }
uint16_t mg_nmi_used_cycles(const MgNmi *b) { return b ? b->cycles : 0u; }

/* ============================================================
 *  HIRQ builder (Phase 2.5a — install plumbing only)
 * ============================================================ */

void mg_hirq_begin(MgHirq *b) {
    if (!b) return;
    b->used   = 0;
    b->cycles = 0;
    b->err    = MG_NMI_OK;
}

uint16_t mg_hirq_emit_raw(MgHirq *b, const uint8_t *bytes, uint16_t n) {
    if (!b || b->err) return (uint16_t)-1;
    if (!bytes || n == 0) return b->used;
    if ((uint32_t)b->used + n > MG_HIRQ_BUF_SIZE) {
        b->err = MG_NMI_E_BUF_FULL;
        return (uint16_t)-1;
    }
    uint16_t start = b->used;
    for (uint16_t i = 0; i < n; i++) {
        b->buf[b->used++] = bytes[i];
    }
    return start;
}

int mg_hirq_finish(MgHirq *b) {
    if (!b) return MG_NMI_E_INSTALL;
    if (b->err) return b->err;
    if (b->used == 0) return MG_NMI_E_BUF_FULL;
    /* HBLANK budget validation lands in Phase 2.5b once emit primitives
     * with known cycle costs exist. For now any buffer that fits is OK. */
    return MG_NMI_OK;
}

int mg_hirq_install(const MgHirq *b) {
    if (!b) return MG_NMI_E_INSTALL;
    if (b->err) return b->err;
    if (b->used == 0) return MG_NMI_E_BUF_FULL;

    int r = (int)_vm_sys2(SYS_MG_HIRQ_INSTALL,
                          (uint32_t)(unsigned long)b->buf,
                          (uint32_t)b->used);
    if (r < 0) return MG_NMI_E_INSTALL;
    return MG_NMI_OK;
}

uint16_t mg_hirq_used_bytes(const MgHirq *b)  { return b ? b->used : 0u; }
uint16_t mg_hirq_used_cycles(const MgHirq *b) { return b ? b->cycles : 0u; }

/* mg_hirq_configure / mg_hirq_disable removed in v2.29 — see header. */
