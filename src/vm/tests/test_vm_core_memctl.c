/* Tests for RV32I memory and control flow instructions:
 *   Loads:    LB, LH, LW, LBU, LHU
 *   Stores:   SB, SH, SW
 *   Branches: BEQ, BNE, BLT, BGE, BLTU, BGEU
 *   Jumps:    JAL, JALR
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  Encoding helpers
 * ============================================================ */

static uint32_t enc_i(int32_t imm, uint32_t rs1,
                      uint32_t funct3, uint32_t rd, uint32_t opcode) {
    uint32_t uimm = ((uint32_t)imm) & 0xFFFu;
    return (uimm << 20) | (rs1 << 15) | (funct3 << 12) |
           (rd << 7) | opcode;
}

static uint32_t enc_s(int32_t imm, uint32_t rs2, uint32_t rs1,
                      uint32_t funct3, uint32_t opcode) {
    uint32_t uimm = ((uint32_t)imm) & 0xFFFu;
    uint32_t hi = (uimm >> 5) & 0x7Fu;
    uint32_t lo = uimm & 0x1Fu;
    return (hi << 25) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | (lo << 7) | opcode;
}

/* B-type: imm[12|10:5|4:1|11] in bits [31|30:25|11:8|7].
 * imm is the byte offset to add to PC if the branch is taken;
 * imm[0] is implicitly 0 (must be even). */
static uint32_t enc_b(int32_t imm, uint32_t rs2, uint32_t rs1,
                      uint32_t funct3, uint32_t opcode) {
    uint32_t uimm = (uint32_t)imm;
    uint32_t b12   = (uimm >> 12) & 0x1u;
    uint32_t b11   = (uimm >> 11) & 0x1u;
    uint32_t b10_5 = (uimm >> 5)  & 0x3Fu;
    uint32_t b4_1  = (uimm >> 1)  & 0xFu;
    return (b12 << 31) | (b10_5 << 25) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | (b4_1 << 8) | (b11 << 7) | opcode;
}

/* J-type: imm[20|10:1|11|19:12] in bits [31|30:21|20|19:12]. */
static uint32_t enc_j(int32_t imm, uint32_t rd, uint32_t opcode) {
    uint32_t uimm = (uint32_t)imm;
    uint32_t b20    = (uimm >> 20) & 0x1u;
    uint32_t b19_12 = (uimm >> 12) & 0xFFu;
    uint32_t b11    = (uimm >> 11) & 0x1u;
    uint32_t b10_1  = (uimm >> 1)  & 0x3FFu;
    return (b20 << 31) | (b10_1 << 21) | (b11 << 20) | (b19_12 << 12) |
           (rd << 7) | opcode;
}

/* ============================================================
 *  Fixture with code + data regions
 * ============================================================ */

typedef struct {
    uint8_t code[128];
    uint8_t data[128];
    VmCpu   cpu;
} MemFixture;

static void fixture_init(MemFixture *f) {
    memset(f, 0, sizeof(*f));
    vm_init(&f->cpu, 0);
    f->cpu.regions[VM_REGION_CODE].base = f->code;
    f->cpu.regions[VM_REGION_CODE].length = sizeof(f->code);
    f->cpu.regions[VM_REGION_CODE].writable = false;
    f->cpu.regions[VM_REGION_DATA].base = f->data;
    f->cpu.regions[VM_REGION_DATA].length = sizeof(f->data);
    f->cpu.regions[VM_REGION_DATA].writable = true;
    f->cpu.pc = 0;
}

static void plant_at(MemFixture *f, uint32_t offset, uint32_t insn) {
    f->code[offset + 0] = (uint8_t)(insn & 0xFF);
    f->code[offset + 1] = (uint8_t)((insn >> 8) & 0xFF);
    f->code[offset + 2] = (uint8_t)((insn >> 16) & 0xFF);
    f->code[offset + 3] = (uint8_t)((insn >> 24) & 0xFF);
}

static void plant(MemFixture *f, uint32_t insn) {
    plant_at(f, f->cpu.pc, insn);
}

static int run_one(MemFixture *f) {
    uint32_t used = 0;
    VmStepResult r = vm_step(&f->cpu, 1, &used);
    return (r == VM_STEP_QUANTUM_EXPIRED && used == 1) ? 1 : 0;
}

/* ============================================================
 *  Loads
 * ============================================================ */

static void test_lw(void) {
    MemFixture f;
    fixture_init(&f);
    /* Put 0xCAFEBABE at data offset 0 (guest addr 0x80000000) */
    f.data[0] = 0xBE; f.data[1] = 0xBA;
    f.data[2] = 0xFE; f.data[3] = 0xCA;

    /* Set up rs1 to point at the data region base. */
    f.cpu.regs[1] = 0x80000000u;
    /* lw x2, 0(x1) — opcode 0x03, funct3 0x2 */
    plant(&f, enc_i(0, 1, 0x2, 2, 0x03));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xCAFEBABEu, (int)f.cpu.regs[2]);
}

static void test_lw_with_offset(void) {
    MemFixture f;
    fixture_init(&f);
    f.data[16] = 0x78; f.data[17] = 0x56;
    f.data[18] = 0x34; f.data[19] = 0x12;
    f.cpu.regs[1] = 0x80000000u;
    /* lw x2, 16(x1) */
    plant(&f, enc_i(16, 1, 0x2, 2, 0x03));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x12345678, (int)f.cpu.regs[2]);
}

static void test_lw_negative_offset(void) {
    MemFixture f;
    fixture_init(&f);
    f.data[0] = 0xEF; f.data[1] = 0xBE;
    f.data[2] = 0xAD; f.data[3] = 0xDE;
    f.cpu.regs[1] = 0x80000000u + 8;
    /* lw x2, -8(x1) — reads at base + 0 */
    plant(&f, enc_i(-8, 1, 0x2, 2, 0x03));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xDEADBEEFu, (int)f.cpu.regs[2]);
}

static void test_lb_sign_extends_negative(void) {
    MemFixture f;
    fixture_init(&f);
    f.data[0] = 0xFF;   /* -1 in signed 8-bit */
    f.cpu.regs[1] = 0x80000000u;
    plant(&f, enc_i(0, 1, 0x0, 2, 0x03));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFFu, (int)f.cpu.regs[2]);
}

static void test_lb_sign_extends_positive(void) {
    MemFixture f;
    fixture_init(&f);
    f.data[0] = 0x7F;   /* +127, high bit clear */
    f.cpu.regs[1] = 0x80000000u;
    plant(&f, enc_i(0, 1, 0x0, 2, 0x03));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x7F, (int)f.cpu.regs[2]);
}

static void test_lbu_zero_extends(void) {
    MemFixture f;
    fixture_init(&f);
    f.data[0] = 0xFF;
    f.cpu.regs[1] = 0x80000000u;
    /* lbu — funct3 = 0x4 */
    plant(&f, enc_i(0, 1, 0x4, 2, 0x03));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0xFF, (int)f.cpu.regs[2]);
}

static void test_lh_sign_extends_negative(void) {
    MemFixture f;
    fixture_init(&f);
    /* 0xFF80 = -128 as signed 16-bit */
    f.data[0] = 0x80; f.data[1] = 0xFF;
    f.cpu.regs[1] = 0x80000000u;
    /* lh — funct3 = 0x1 */
    plant(&f, enc_i(0, 1, 0x1, 2, 0x03));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFF80u, (int)f.cpu.regs[2]);
}

static void test_lhu_zero_extends(void) {
    MemFixture f;
    fixture_init(&f);
    f.data[0] = 0x80; f.data[1] = 0xFF;
    f.cpu.regs[1] = 0x80000000u;
    /* lhu — funct3 = 0x5 */
    plant(&f, enc_i(0, 1, 0x5, 2, 0x03));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0xFF80, (int)f.cpu.regs[2]);
}

static void test_lw_misaligned_traps(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    /* lw x2, 1(x1) — misaligned */
    plant(&f, enc_i(1, 1, 0x2, 2, 0x03));

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_LOAD_MISALIGNED, f.cpu.trap_cause);
}

static void test_lh_misaligned_traps(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    /* lh x2, 1(x1) — misaligned (odd byte) */
    plant(&f, enc_i(1, 1, 0x1, 2, 0x03));

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_LOAD_MISALIGNED, f.cpu.trap_cause);
}

static void test_lb_byte_alignment_ok(void) {
    /* LB has no alignment requirement. */
    MemFixture f;
    fixture_init(&f);
    f.data[3] = 0x42;
    f.cpu.regs[1] = 0x80000000u;
    plant(&f, enc_i(3, 1, 0x0, 2, 0x03));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x42, (int)f.cpu.regs[2]);
}

static void test_load_out_of_bounds_traps(void) {
    MemFixture f;
    fixture_init(&f);
    /* Region is 128 bytes; try to load past the end. */
    f.cpu.regs[1] = 0x80000000u + 124;
    plant(&f, enc_i(8, 1, 0x2, 2, 0x03));   /* lw at base+132 */

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_LOAD_FAULT, f.cpu.trap_cause);
}

/* ============================================================
 *  Stores
 * ============================================================ */

static void test_sw(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    f.cpu.regs[2] = 0x11223344u;
    /* sw x2, 0(x1) — funct3 0x2 */
    plant(&f, enc_s(0, 2, 1, 0x2, 0x23));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x44, f.data[0]);
    ASSERT_EQ_INT(0x33, f.data[1]);
    ASSERT_EQ_INT(0x22, f.data[2]);
    ASSERT_EQ_INT(0x11, f.data[3]);
}

static void test_sb(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    f.cpu.regs[2] = 0xAABBCCDDu;
    /* sb x2, 0(x1) — funct3 0x0; only low byte stored */
    plant(&f, enc_s(0, 2, 1, 0x0, 0x23));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0xDD, f.data[0]);
    /* No write to surrounding bytes */
    ASSERT_EQ_INT(0x00, f.data[1]);
}

static void test_sh(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    f.cpu.regs[2] = 0x11223344u;
    /* sh x2, 0(x1) — funct3 0x1; low 16 bits stored */
    plant(&f, enc_s(0, 2, 1, 0x1, 0x23));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x44, f.data[0]);
    ASSERT_EQ_INT(0x33, f.data[1]);
    ASSERT_EQ_INT(0x00, f.data[2]);
}

static void test_sw_with_negative_offset(void) {
    /* S-type immediate is split across the instruction; this
     * tests that negative offsets encode correctly. */
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u + 16;
    f.cpu.regs[2] = 0xCAFEBABEu;
    plant(&f, enc_s(-16, 2, 1, 0x2, 0x23));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0xBE, f.data[0]);
    ASSERT_EQ_INT(0xBA, f.data[1]);
    ASSERT_EQ_INT(0xFE, f.data[2]);
    ASSERT_EQ_INT(0xCA, f.data[3]);
}

static void test_sw_misaligned_traps(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u + 1;   /* odd byte */
    plant(&f, enc_s(0, 2, 1, 0x2, 0x23));

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_STORE_MISALIGNED, f.cpu.trap_cause);
}

static void test_store_to_readonly_traps(void) {
    MemFixture f;
    fixture_init(&f);
    /* Write into CODE region (RO) */
    f.cpu.regs[1] = 0x00000010u;   /* code region, valid offset */
    plant(&f, enc_s(0, 2, 1, 0x2, 0x23));

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_STORE_RO, f.cpu.trap_cause);
}

static void test_load_store_round_trip(void) {
    /* Write then read back. */
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u + 32;
    f.cpu.regs[2] = 0xDEADBEEFu;
    /* sw x2, 0(x1) */
    plant_at(&f, 0, enc_s(0, 2, 1, 0x2, 0x23));
    /* lw x3, 0(x1) */
    plant_at(&f, 4, enc_i(0, 1, 0x2, 3, 0x03));

    uint32_t used = 0;
    ASSERT_EQ_INT(VM_STEP_QUANTUM_EXPIRED, vm_step(&f.cpu, 2, &used));
    ASSERT_EQ_INT(2, (int)used);
    ASSERT_EQ_INT((int)0xDEADBEEFu, (int)f.cpu.regs[3]);
}

/* ============================================================
 *  Branches
 * ============================================================ */

static void test_beq_taken(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 42;
    f.cpu.regs[2] = 42;
    /* beq x1, x2, 16 — funct3 0x0 */
    plant(&f, enc_b(16, 2, 1, 0x0, 0x63));
    /* Plant the target so the fetcher doesn't trap if it advances. */
    plant_at(&f, 16, enc_i(0, 0, 0x0, 0, 0x13));   /* nop */

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(16, (int)f.cpu.pc);
}

static void test_beq_not_taken(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 42;
    f.cpu.regs[2] = 43;
    plant(&f, enc_b(16, 2, 1, 0x0, 0x63));

    ASSERT(run_one(&f));
    /* Branch not taken: PC advances by 4 */
    ASSERT_EQ_INT(4, (int)f.cpu.pc);
}

static void test_bne(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 42;
    f.cpu.regs[2] = 43;
    plant(&f, enc_b(16, 2, 1, 0x1, 0x63));
    plant_at(&f, 16, enc_i(0, 0, 0x0, 0, 0x13));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(16, (int)f.cpu.pc);
}

static void test_blt_signed(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = (uint32_t)-5;   /* -5 as signed */
    f.cpu.regs[2] = 1;
    /* blt: branch if rs1 < rs2 (signed). -5 < 1 = true. */
    plant(&f, enc_b(16, 2, 1, 0x4, 0x63));
    plant_at(&f, 16, enc_i(0, 0, 0x0, 0, 0x13));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(16, (int)f.cpu.pc);
}

static void test_bge_signed(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 5;
    f.cpu.regs[2] = 5;
    /* bge: branch if rs1 >= rs2 (signed). 5 >= 5 = true. */
    plant(&f, enc_b(16, 2, 1, 0x5, 0x63));
    plant_at(&f, 16, enc_i(0, 0, 0x0, 0, 0x13));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(16, (int)f.cpu.pc);
}

static void test_bltu(void) {
    /* Unsigned: 0xFFFFFFFF < 1 is FALSE (it's the max value). */
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    f.cpu.regs[2] = 1;
    plant(&f, enc_b(16, 2, 1, 0x6, 0x63));

    ASSERT(run_one(&f));
    /* Not taken — PC advanced by 4 */
    ASSERT_EQ_INT(4, (int)f.cpu.pc);
}

static void test_bgeu(void) {
    /* Unsigned: 0xFFFFFFFF >= 1 is TRUE. */
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    f.cpu.regs[2] = 1;
    plant(&f, enc_b(16, 2, 1, 0x7, 0x63));
    plant_at(&f, 16, enc_i(0, 0, 0x0, 0, 0x13));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(16, (int)f.cpu.pc);
}

static void test_branch_backward(void) {
    /* Negative branch offset. Plant branch at offset 32, target
     * at offset 8. */
    MemFixture f;
    fixture_init(&f);
    plant_at(&f, 8, enc_i(0, 0, 0x0, 0, 0x13));    /* nop at target */
    plant_at(&f, 32, enc_b(-24, 2, 1, 0x0, 0x63)); /* beq x1, x2, -24 */
    f.cpu.regs[1] = 0;
    f.cpu.regs[2] = 0;
    f.cpu.pc = 32;

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(8, (int)f.cpu.pc);
}

/* ============================================================
 *  JAL
 * ============================================================ */

static void test_jal(void) {
    MemFixture f;
    fixture_init(&f);
    /* jal x1, 16 — link in x1, jump to pc+16 */
    plant(&f, enc_j(16, 1, 0x6F));
    plant_at(&f, 16, enc_i(0, 0, 0x0, 0, 0x13));

    ASSERT(run_one(&f));
    /* Link address is pc+4 (the instruction after JAL) */
    ASSERT_EQ_INT(4, (int)f.cpu.regs[1]);
    ASSERT_EQ_INT(16, (int)f.cpu.pc);
}

static void test_jal_backward(void) {
    MemFixture f;
    fixture_init(&f);
    plant_at(&f, 16, enc_i(0, 0, 0x0, 0, 0x13));   /* target */
    plant_at(&f, 64, enc_j(-48, 1, 0x6F));         /* jal x1, -48 */
    f.cpu.pc = 64;

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(68, (int)f.cpu.regs[1]);   /* link = 64+4 */
    ASSERT_EQ_INT(16, (int)f.cpu.pc);
}

static void test_jal_to_x0_is_unconditional_jump(void) {
    /* jal x0, target — discards link, just jumps. */
    MemFixture f;
    fixture_init(&f);
    plant(&f, enc_j(20, 0, 0x6F));
    plant_at(&f, 20, enc_i(0, 0, 0x0, 0, 0x13));

    ASSERT(run_one(&f));
    /* x0 should still be 0 (squashed by dispatcher) */
    ASSERT_EQ_INT(0, (int)f.cpu.regs[0]);
    ASSERT_EQ_INT(20, (int)f.cpu.pc);
}

/* ============================================================
 *  JALR
 * ============================================================ */

static void test_jalr(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 32;
    /* jalr x1, 4(x5) — link in x1, target = (32+4) & ~1 = 36 */
    plant(&f, enc_i(4, 5, 0x0, 1, 0x67));
    plant_at(&f, 36, enc_i(0, 0, 0x0, 0, 0x13));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(4, (int)f.cpu.regs[1]);
    ASSERT_EQ_INT(36, (int)f.cpu.pc);
}

static void test_jalr_masks_low_bit(void) {
    /* JALR clears the low bit of the computed target. */
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 33;   /* odd */
    /* jalr x1, 0(x5) — target = 33 & ~1 = 32 */
    plant(&f, enc_i(0, 5, 0x0, 1, 0x67));
    plant_at(&f, 32, enc_i(0, 0, 0x0, 0, 0x13));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(32, (int)f.cpu.pc);
}

static void test_jalr_negative_offset(void) {
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 100;
    /* jalr x1, -40(x5) at PC=16; target = (100 + -40) & ~1 = 60 */
    plant_at(&f, 16, enc_i(-40, 5, 0x0, 1, 0x67));
    plant_at(&f, 60, enc_i(0, 0, 0x0, 0, 0x13));   /* nop at target */
    f.cpu.pc = 16;

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(20, (int)f.cpu.regs[1]);   /* link = 16+4 */
    ASSERT_EQ_INT(60, (int)f.cpu.pc);        /* target */
}

static void test_jalr_rd_equals_rs1(void) {
    /* The spec is explicit: rs1 is read before rd is written.
     * This matters for code like `jalr x1, 0(x1)` which uses the
     * return address as the target. */
    MemFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 48;
    /* jalr x1, 0(x1) — target should be 48, link should be 4 */
    plant(&f, enc_i(0, 1, 0x0, 1, 0x67));
    plant_at(&f, 48, enc_i(0, 0, 0x0, 0, 0x13));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(4, (int)f.cpu.regs[1]);    /* link overwrote rs1 */
    ASSERT_EQ_INT(48, (int)f.cpu.pc);        /* target used the OLD rs1 */
}

/* ============================================================
 *  Combined: simple loop
 * ============================================================ */

static void test_loop_decrement_to_zero(void) {
    /* Run this code:
     *   addi x1, x0, 5      # x1 = 5
     *   addi x1, x1, -1     # x1 -= 1
     *   bne  x1, x0, -4     # if x1 != 0, loop back
     *
     * After the loop, x1 should be 0 and we should have done
     * 1 setup + 5 decrements + 5 branches = 11 instructions
     * (last branch falls through). */
    MemFixture f;
    fixture_init(&f);
    plant_at(&f, 0, enc_i(5, 0, 0x0, 1, 0x13));
    plant_at(&f, 4, enc_i(-1, 1, 0x0, 1, 0x13));
    plant_at(&f, 8, enc_b(-4, 0, 1, 0x1, 0x63));   /* bne x1, x0, -4 */

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 100, &used);
    /* Either ran out of budget or fell off the end (no instruction
     * at PC=12) — both would be acceptable, but actually we'll
     * hit an illegal-instruction trap because code[12..15] is zero
     * which is opcode 0 (illegal). So we expect VM_STEP_TRAPPED. */
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    /* x1 should be 0 after the loop */
    ASSERT_EQ_INT(0, (int)f.cpu.regs[1]);
    /* We should have run 1 + 5 + 5 = 11 instructions */
    ASSERT_EQ_INT(11, (int)used);
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_core_memctl");

    /* Loads */
    RUN(test_lw);
    RUN(test_lw_with_offset);
    RUN(test_lw_negative_offset);
    RUN(test_lb_sign_extends_negative);
    RUN(test_lb_sign_extends_positive);
    RUN(test_lbu_zero_extends);
    RUN(test_lh_sign_extends_negative);
    RUN(test_lhu_zero_extends);
    RUN(test_lw_misaligned_traps);
    RUN(test_lh_misaligned_traps);
    RUN(test_lb_byte_alignment_ok);
    RUN(test_load_out_of_bounds_traps);

    /* Stores */
    RUN(test_sw);
    RUN(test_sb);
    RUN(test_sh);
    RUN(test_sw_with_negative_offset);
    RUN(test_sw_misaligned_traps);
    RUN(test_store_to_readonly_traps);
    RUN(test_load_store_round_trip);

    /* Branches */
    RUN(test_beq_taken);
    RUN(test_beq_not_taken);
    RUN(test_bne);
    RUN(test_blt_signed);
    RUN(test_bge_signed);
    RUN(test_bltu);
    RUN(test_bgeu);
    RUN(test_branch_backward);

    /* JAL */
    RUN(test_jal);
    RUN(test_jal_backward);
    RUN(test_jal_to_x0_is_unconditional_jump);

    /* JALR */
    RUN(test_jalr);
    RUN(test_jalr_masks_low_bit);
    RUN(test_jalr_negative_offset);
    RUN(test_jalr_rd_equals_rs1);

    /* Combined */
    RUN(test_loop_decrement_to_zero);

    return TEST_SUITE_RESULT();
}
