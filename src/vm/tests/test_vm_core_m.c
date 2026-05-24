/* Tests for RV32M multiply/divide instructions:
 *   MUL, MULH, MULHSU, MULHU
 *   DIV, DIVU, REM, REMU
 *
 * Particular attention to:
 *   - High-half products for the three MULH variants
 *   - Sign mixing in MULHSU
 *   - Divide-by-zero (returns -1 for DIV/DIVU, dividend for REM/REMU)
 *   - Signed-overflow INT32_MIN / -1 (returns INT32_MIN; REM returns 0) */

#include "test_runner.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  Encoding helpers
 * ============================================================ */

static uint32_t enc_r(uint32_t funct7, uint32_t rs2, uint32_t rs1,
                      uint32_t funct3, uint32_t rd, uint32_t opcode) {
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | (rd << 7) | opcode;
}

/* RV32M instructions all use funct7=0x01, opcode=0x33 */
#define ENC_M(f3, rs2, rs1, rd)  enc_r(0x01, (rs2), (rs1), (f3), (rd), 0x33)

/* ============================================================
 *  Fixture
 * ============================================================ */

typedef struct {
    uint8_t code[64];
    VmCpu   cpu;
} MFixture;

static void fixture_init(MFixture *f) {
    memset(f, 0, sizeof(*f));
    vm_init(&f->cpu, 0);
    f->cpu.regions[VM_REGION_CODE].base = f->code;
    f->cpu.regions[VM_REGION_CODE].length = sizeof(f->code);
    f->cpu.regions[VM_REGION_CODE].writable = false;
    f->cpu.pc = 0;
}

static void plant(MFixture *f, uint32_t insn) {
    f->code[f->cpu.pc + 0] = (uint8_t)(insn & 0xFF);
    f->code[f->cpu.pc + 1] = (uint8_t)((insn >> 8) & 0xFF);
    f->code[f->cpu.pc + 2] = (uint8_t)((insn >> 16) & 0xFF);
    f->code[f->cpu.pc + 3] = (uint8_t)((insn >> 24) & 0xFF);
}

static int run_one(MFixture *f) {
    uint32_t used = 0;
    VmStepResult r = vm_step(&f->cpu, 1, &used);
    return (r == VM_STEP_QUANTUM_EXPIRED && used == 1) ? 1 : 0;
}

/* ============================================================
 *  MUL — low 32 bits of product
 * ============================================================ */

static void test_mul_small(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 7;
    f.cpu.regs[2] = 6;
    plant(&f, ENC_M(0x0, 2, 1, 3));   /* mul x3, x1, x2 */
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(42, (int)f.cpu.regs[3]);
}

static void test_mul_negative(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = (uint32_t)-7;
    f.cpu.regs[2] = 6;
    plant(&f, ENC_M(0x0, 2, 1, 3));
    ASSERT(run_one(&f));
    /* -7 * 6 = -42 = 0xFFFFFFD6 */
    ASSERT_EQ_INT((int)(uint32_t)-42, (int)f.cpu.regs[3]);
}

static void test_mul_truncates(void) {
    /* 0x10000 * 0x10000 = 0x1_00000000; low 32 bits = 0 */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x10000;
    f.cpu.regs[2] = 0x10000;
    plant(&f, ENC_M(0x0, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[3]);
}

static void test_mul_by_zero(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xDEADBEEFu;
    f.cpu.regs[2] = 0;
    plant(&f, ENC_M(0x0, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[3]);
}

/* ============================================================
 *  MULH — high 32 bits, signed × signed
 * ============================================================ */

static void test_mulh_small_positive(void) {
    /* 100 * 100 = 10000 (well under 32 bits, high half = 0) */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 100;
    f.cpu.regs[2] = 100;
    plant(&f, ENC_M(0x1, 2, 1, 3));   /* mulh x3, x1, x2 */
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[3]);
}

static void test_mulh_overflow(void) {
    /* 0x10000 * 0x10000 = 0x100000000. High 32 bits = 1, low 32 = 0. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x10000;
    f.cpu.regs[2] = 0x10000;
    plant(&f, ENC_M(0x1, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(1, (int)f.cpu.regs[3]);
}

static void test_mulh_both_negative(void) {
    /* (-2) * (-3) = 6. Full 64-bit product = 0x0000_0000_0000_0006.
     * High 32 = 0. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = (uint32_t)-2;
    f.cpu.regs[2] = (uint32_t)-3;
    plant(&f, ENC_M(0x1, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[3]);
}

static void test_mulh_mixed_signs(void) {
    /* INT32_MIN * 2 = 0xFFFFFFFF_00000000 as signed 64-bit.
     * High 32 = 0xFFFFFFFF (sign-extended). */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;   /* INT32_MIN */
    f.cpu.regs[2] = 2;
    plant(&f, ENC_M(0x1, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFFu, (int)f.cpu.regs[3]);
}

/* ============================================================
 *  MULHU — high 32 bits, unsigned × unsigned
 * ============================================================ */

static void test_mulhu_basic(void) {
    /* 0xFFFFFFFF * 0xFFFFFFFF = 0xFFFFFFFE_00000001 (unsigned).
     * High 32 = 0xFFFFFFFE. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    f.cpu.regs[2] = 0xFFFFFFFFu;
    plant(&f, ENC_M(0x3, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFEu, (int)f.cpu.regs[3]);
}

static void test_mulhu_differs_from_mulh(void) {
    /* Same operands as test_mulh_both_negative, but unsigned.
     *   (uint32_t)-2 = 0xFFFFFFFE
     *   (uint32_t)-3 = 0xFFFFFFFD
     *   product = 0xFFFFFFFB_00000006 (unsigned)
     *   high 32 = 0xFFFFFFFB
     * Different from MULH (which gave 0). */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = (uint32_t)-2;
    f.cpu.regs[2] = (uint32_t)-3;
    plant(&f, ENC_M(0x3, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFBu, (int)f.cpu.regs[3]);
}

/* ============================================================
 *  MULHSU — high 32 bits, signed × unsigned
 * ============================================================ */

static void test_mulhsu_negative_signed(void) {
    /* rs1 = -1 (signed), rs2 = 1 (unsigned).
     * Product = (int64_t)(-1) * (uint32_t)1 = -1 in 64-bit signed.
     * High 32 = 0xFFFFFFFF (sign extension). */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    f.cpu.regs[2] = 1;
    plant(&f, ENC_M(0x2, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFFu, (int)f.cpu.regs[3]);
}

static void test_mulhsu_negative_signed_times_large_unsigned(void) {
    /* rs1 = -1 (signed), rs2 = 0xFFFFFFFF (unsigned).
     * Product = -1 * 4294967295 = -4294967295 = 0xFFFFFFFF_00000001
     * (as 64-bit signed two's complement).
     * High 32 = 0xFFFFFFFF. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    f.cpu.regs[2] = 0xFFFFFFFFu;
    plant(&f, ENC_M(0x2, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFFu, (int)f.cpu.regs[3]);
}

static void test_mulhsu_positive_signed(void) {
    /* rs1 = 2 (signed), rs2 = 0x80000000 (unsigned).
     * Product = 2 * 0x80000000 = 0x1_00000000.
     * High 32 = 1. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 2;
    f.cpu.regs[2] = 0x80000000u;
    plant(&f, ENC_M(0x2, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(1, (int)f.cpu.regs[3]);
}

/* ============================================================
 *  DIV — signed division
 * ============================================================ */

static void test_div_basic(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 100;
    f.cpu.regs[2] = 7;
    plant(&f, ENC_M(0x4, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(14, (int)f.cpu.regs[3]);
}

static void test_div_negative_dividend(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = (uint32_t)-100;
    f.cpu.regs[2] = 7;
    plant(&f, ENC_M(0x4, 2, 1, 3));
    ASSERT(run_one(&f));
    /* Truncating division: -100/7 = -14 (rounds toward zero) */
    ASSERT_EQ_INT(-14, (int32_t)f.cpu.regs[3]);
}

static void test_div_negative_divisor(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 100;
    f.cpu.regs[2] = (uint32_t)-7;
    plant(&f, ENC_M(0x4, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(-14, (int32_t)f.cpu.regs[3]);
}

static void test_div_by_zero_returns_minus_one(void) {
    /* RISC-V spec: DIV by zero returns -1 (all bits set) without trapping. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 100;
    f.cpu.regs[2] = 0;
    plant(&f, ENC_M(0x4, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFFu, (int)f.cpu.regs[3]);
}

static void test_div_intmin_by_minus_one_returns_intmin(void) {
    /* The signed overflow case. Mathematically INT32_MIN / -1 = 2^31,
     * which doesn't fit. Spec: result is INT32_MIN itself. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    f.cpu.regs[2] = 0xFFFFFFFFu;
    plant(&f, ENC_M(0x4, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x80000000u, (int)f.cpu.regs[3]);
}

/* ============================================================
 *  DIVU — unsigned division
 * ============================================================ */

static void test_divu_basic(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    f.cpu.regs[2] = 2;
    plant(&f, ENC_M(0x5, 2, 1, 3));
    ASSERT(run_one(&f));
    /* 0xFFFFFFFF / 2 = 0x7FFFFFFF unsigned */
    ASSERT_EQ_INT((int)0x7FFFFFFFu, (int)f.cpu.regs[3]);
}

static void test_divu_by_zero_returns_all_ones(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 42;
    f.cpu.regs[2] = 0;
    plant(&f, ENC_M(0x5, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFFu, (int)f.cpu.regs[3]);
}

static void test_divu_distinguishes_from_div(void) {
    /* DIV: -1 / 1 = -1 (which is signed -1).
     * DIVU: 0xFFFFFFFF / 1 = 0xFFFFFFFF.
     * Same bit pattern, but conceptually different operations. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    f.cpu.regs[2] = 1;
    plant(&f, ENC_M(0x5, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFFu, (int)f.cpu.regs[3]);
}

/* ============================================================
 *  REM — signed remainder
 * ============================================================ */

static void test_rem_basic(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 100;
    f.cpu.regs[2] = 7;
    plant(&f, ENC_M(0x6, 2, 1, 3));
    ASSERT(run_one(&f));
    /* 100 = 14*7 + 2 */
    ASSERT_EQ_INT(2, (int)f.cpu.regs[3]);
}

static void test_rem_negative_dividend(void) {
    /* C99 rule: sign of result follows sign of dividend. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = (uint32_t)-100;
    f.cpu.regs[2] = 7;
    plant(&f, ENC_M(0x6, 2, 1, 3));
    ASSERT(run_one(&f));
    /* -100 = (-14)*7 + (-2) */
    ASSERT_EQ_INT(-2, (int32_t)f.cpu.regs[3]);
}

static void test_rem_by_zero_returns_dividend(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 42;
    f.cpu.regs[2] = 0;
    plant(&f, ENC_M(0x6, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(42, (int)f.cpu.regs[3]);
}

static void test_rem_intmin_by_minus_one_returns_zero(void) {
    /* Mathematical INT32_MIN % -1 = 0 (since INT32_MIN is divisible).
     * Spec explicitly mandates 0 here to avoid the C UB. */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    f.cpu.regs[2] = 0xFFFFFFFFu;
    plant(&f, ENC_M(0x6, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[3]);
}

/* ============================================================
 *  REMU — unsigned remainder
 * ============================================================ */

static void test_remu_basic(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 100;
    f.cpu.regs[2] = 7;
    plant(&f, ENC_M(0x7, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(2, (int)f.cpu.regs[3]);
}

static void test_remu_by_zero_returns_dividend(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xDEADBEEFu;
    f.cpu.regs[2] = 0;
    plant(&f, ENC_M(0x7, 2, 1, 3));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xDEADBEEFu, (int)f.cpu.regs[3]);
}

static void test_remu_large_dividend(void) {
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    f.cpu.regs[2] = 3;
    plant(&f, ENC_M(0x7, 2, 1, 3));
    ASSERT(run_one(&f));
    /* 0xFFFFFFFF / 3 = 0x55555555, remainder = 0xFFFFFFFF - 3*0x55555555 = 0 */
    ASSERT_EQ_INT(0, (int)f.cpu.regs[3]);
}

/* ============================================================
 *  Combined: factorial via repeated multiplication
 * ============================================================ */

static void test_factorial_via_mul_loop(void) {
    /* x1 = result, starts at 1
     * x2 = counter, starts at 5
     * loop:
     *   mul x1, x1, x2
     *   addi x2, x2, -1
     *   bne x2, x0, loop
     *
     * Expected: 5! = 120 */
    MFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 1;
    f.cpu.regs[2] = 5;
    plant(&f, ENC_M(0x0, 2, 1, 1));   /* mul x1, x1, x2 */
    f.cpu.pc = 4;
    /* addi x2, x2, -1 */
    plant(&f, 0xFFF10113u);
    f.cpu.pc = 8;
    /* bne x2, x0, -8: branch back 8 bytes */
    /* Encode: imm=-8, rs2=0, rs1=2, funct3=1, opcode=0x63 */
    /* -8 in 13-bit signed = 0x1FF8 (sign-extended), encoded fields:
     * bit 12 = 1, bits 10:5 = 111100, bits 4:1 = 1100, bit 11 = 1, bit 0 = 0
     * Actually let me compute this by hand: imm = -8 = 0xFFFFFFF8.
     * imm[12]=1, imm[11]=1, imm[10:5]=111111, imm[4:1]=1100, imm[0]=0
     * Wait, imm here = 0xFFFFFFF8 — sign-extended -8. Bit 12 (the sign) is 1.
     * Actually, the B-type immediate is 13 bits (imm[12:0]) where bit 0 is always 0.
     * For imm = -8: low 13 bits = 1_1111_1111_1000.
     *   imm[12] = 1 (sign)
     *   imm[11] = 1
     *   imm[10:5] = 111111
     *   imm[4:1] = 1100
     *   imm[0] = 0
     * Instruction layout:
     *   bit 31 = imm[12] = 1
     *   bits 30:25 = imm[10:5] = 111111 = 0x3F
     *   bits 24:20 = rs2 = 0
     *   bits 19:15 = rs1 = 2
     *   bits 14:12 = funct3 = 1
     *   bits 11:8 = imm[4:1] = 1100 = 0xC
     *   bit 7 = imm[11] = 1
     *   bits 6:0 = opcode = 0x63
     */
    uint32_t bne_back8 =
        (1u << 31) | (0x3Fu << 25) | (0u << 20) | (2u << 15) |
        (1u << 12) | (0xCu << 8)   | (1u << 7)  | 0x63u;
    plant(&f, bne_back8);
    f.cpu.pc = 0;

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 100, &used);
    /* The loop will exit when x2 reaches 0, and the next fetch
     * (at PC=12, beyond the plantd code) will trap. So we expect
     * VM_STEP_TRAPPED. */
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(120, (int)f.cpu.regs[1]);
    /* 1 setup + 5 iterations of 3 instructions, but only 4 BNEs
     * are taken (the last one falls through):
     *   mul, addi, bne  (taken) — iter 1
     *   mul, addi, bne  (taken) — iter 2
     *   mul, addi, bne  (taken) — iter 3
     *   mul, addi, bne  (taken) — iter 4
     *   mul, addi, bne  (not taken) — iter 5
     * Total: 15 instructions. */
    ASSERT_EQ_INT(15, (int)used);
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_core_m");

    /* MUL */
    RUN(test_mul_small);
    RUN(test_mul_negative);
    RUN(test_mul_truncates);
    RUN(test_mul_by_zero);

    /* MULH */
    RUN(test_mulh_small_positive);
    RUN(test_mulh_overflow);
    RUN(test_mulh_both_negative);
    RUN(test_mulh_mixed_signs);

    /* MULHU */
    RUN(test_mulhu_basic);
    RUN(test_mulhu_differs_from_mulh);

    /* MULHSU */
    RUN(test_mulhsu_negative_signed);
    RUN(test_mulhsu_negative_signed_times_large_unsigned);
    RUN(test_mulhsu_positive_signed);

    /* DIV */
    RUN(test_div_basic);
    RUN(test_div_negative_dividend);
    RUN(test_div_negative_divisor);
    RUN(test_div_by_zero_returns_minus_one);
    RUN(test_div_intmin_by_minus_one_returns_intmin);

    /* DIVU */
    RUN(test_divu_basic);
    RUN(test_divu_by_zero_returns_all_ones);
    RUN(test_divu_distinguishes_from_div);

    /* REM */
    RUN(test_rem_basic);
    RUN(test_rem_negative_dividend);
    RUN(test_rem_by_zero_returns_dividend);
    RUN(test_rem_intmin_by_minus_one_returns_zero);

    /* REMU */
    RUN(test_remu_basic);
    RUN(test_remu_by_zero_returns_dividend);
    RUN(test_remu_large_dividend);

    /* Combined */
    RUN(test_factorial_via_mul_loop);

    return TEST_SUITE_RESULT();
}
