/* Tests for RV32I ALU instructions:
 *   I-type:   ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI
 *   R-type:   ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND
 *   U-type:   LUI, AUIPC
 *
 * Each test encodes one (or a few) instructions by hand, points
 * the VM's code region at them, sets up rs1/rs2 values, runs
 * vm_step, and checks rd. */

#include "test_runner.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  Encoding helpers — build instructions on the fly
 * ============================================================ */

static uint32_t enc_r(uint32_t funct7, uint32_t rs2, uint32_t rs1,
                      uint32_t funct3, uint32_t rd, uint32_t opcode) {
    return (funct7 << 25) | (rs2 << 20) | (rs1 << 15) |
           (funct3 << 12) | (rd << 7) | opcode;
}

static uint32_t enc_i(int32_t imm, uint32_t rs1,
                      uint32_t funct3, uint32_t rd, uint32_t opcode) {
    uint32_t uimm = ((uint32_t)imm) & 0xFFFu;
    return (uimm << 20) | (rs1 << 15) | (funct3 << 12) |
           (rd << 7) | opcode;
}

/* I-type for shifts: shamt in low 5 bits, funct7 in high 7 */
static uint32_t enc_i_shift(uint32_t funct7, uint32_t shamt, uint32_t rs1,
                            uint32_t funct3, uint32_t rd, uint32_t opcode) {
    return (funct7 << 25) | ((shamt & 0x1Fu) << 20) | (rs1 << 15) |
           (funct3 << 12) | (rd << 7) | opcode;
}

static uint32_t enc_u(uint32_t imm, uint32_t rd, uint32_t opcode) {
    return (imm & 0xFFFFF000u) | (rd << 7) | opcode;
}

/* ============================================================
 *  Test fixture: 64 bytes of code RAM, run one instruction,
 *  check register state.
 * ============================================================ */

typedef struct {
    uint8_t code[64];
    VmCpu   cpu;
} AluFixture;

static void fixture_init(AluFixture *f) {
    memset(f, 0, sizeof(*f));
    vm_init(&f->cpu, 0);
    f->cpu.regions[VM_REGION_CODE].base = f->code;
    f->cpu.regions[VM_REGION_CODE].length = sizeof(f->code);
    f->cpu.regions[VM_REGION_CODE].writable = false;
    f->cpu.pc = 0;
}

/* Plant one 32-bit instruction at the given offset in the code
 * buffer. The buffer is always the start of region 0 (vaddr 0). */
static void plant_at(AluFixture *f, uint32_t offset, uint32_t insn) {
    f->code[offset + 0] = (uint8_t)(insn & 0xFF);
    f->code[offset + 1] = (uint8_t)((insn >> 8) & 0xFF);
    f->code[offset + 2] = (uint8_t)((insn >> 16) & 0xFF);
    f->code[offset + 3] = (uint8_t)((insn >> 24) & 0xFF);
}

/* Plant at the current PC (assumes PC is a buffer offset). */
static void plant(AluFixture *f, uint32_t insn) {
    plant_at(f, f->cpu.pc, insn);
}

/* Run exactly one instruction. Asserts vm_step succeeded. */
static int run_one(AluFixture *f) {
    uint32_t used = 0;
    VmStepResult r = vm_step(&f->cpu, 1, &used);
    return (r == VM_STEP_QUANTUM_EXPIRED && used == 1) ? 1 : 0;
}

/* ============================================================
 *  I-type ALU
 * ============================================================ */

static void test_addi_positive(void) {
    AluFixture f;
    fixture_init(&f);
    /* addi x1, x0, 42 */
    plant(&f, enc_i(42, /*rs1=*/0, /*f3=*/0x0, /*rd=*/1, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(42, (int)f.cpu.regs[1]);
    ASSERT_EQ_INT(4, (int)f.cpu.pc);
}

static void test_addi_negative_sign_extends(void) {
    AluFixture f;
    fixture_init(&f);
    /* addi x1, x0, -5 — immediate is sign-extended to 0xFFFFFFFB */
    plant(&f, enc_i(-5, 0, 0x0, 1, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFFFFBu, (int)f.cpu.regs[1]);
}

static void test_addi_max_negative(void) {
    AluFixture f;
    fixture_init(&f);
    /* imm = -2048 (0x800) is the minimum 12-bit signed value */
    plant(&f, enc_i(-2048, 0, 0x0, 1, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFF800u, (int)f.cpu.regs[1]);
}

static void test_addi_uses_rs1(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 100;
    /* addi x6, x5, 7 */
    plant(&f, enc_i(7, 5, 0x0, 6, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(107, (int)f.cpu.regs[6]);
}

static void test_addi_overflow_wraps(void) {
    /* Overflow is silent — RISC-V doesn't trap on signed overflow. */
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x7FFFFFFFu;   /* INT32_MAX */
    plant(&f, enc_i(1, 1, 0x0, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x80000000u, (int)f.cpu.regs[2]);
}

static void test_addi_to_x0_squashed(void) {
    /* Writes to x0 are squashed by the dispatcher. */
    AluFixture f;
    fixture_init(&f);
    plant(&f, enc_i(42, 0, 0x0, /*rd=x0*/0, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[0]);
}

static void test_slti_signed(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = (uint32_t)-1;   /* -1 as signed */
    /* slti x2, x1, 0 — is -1 < 0? Yes. */
    plant(&f, enc_i(0, 1, 0x2, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(1, (int)f.cpu.regs[2]);
}

static void test_slti_signed_false(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 5;
    /* slti x2, x1, 5 — is 5 < 5? No. */
    plant(&f, enc_i(5, 1, 0x2, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[2]);
}

static void test_sltiu_unsigned(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;   /* all bits set */
    /* sltiu x2, x1, 1 — is 0xFFFFFFFF < 1 (unsigned)? No. */
    plant(&f, enc_i(1, 1, 0x3, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[2]);
}

static void test_sltiu_zero_idiom(void) {
    /* sltiu rd, rs1, 1 is the canonical "set if zero" idiom:
     * result is 1 iff rs1 == 0. */
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0;
    plant(&f, enc_i(1, 1, 0x3, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(1, (int)f.cpu.regs[2]);
}

static void test_xori(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xF0F0F0F0u;
    /* xori x2, x1, 0x0FF (sign-extended to 0x000000FF) */
    plant(&f, enc_i(0x0FF, 1, 0x4, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xF0F0F00Fu, (int)f.cpu.regs[2]);
}

static void test_xori_minus_one_inverts(void) {
    /* xori rs, -1 is bitwise NOT. */
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x12345678u;
    plant(&f, enc_i(-1, 1, 0x4, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)~0x12345678u, (int)f.cpu.regs[2]);
}

static void test_ori(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x12345600u;
    plant(&f, enc_i(0x078, 1, 0x6, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x12345678u, (int)f.cpu.regs[2]);
}

static void test_andi(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    plant(&f, enc_i(0x0FF, 1, 0x7, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x000000FFu, (int)f.cpu.regs[2]);
}

static void test_slli(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 1;
    /* slli x2, x1, 4 — shamt=4, funct7=0, funct3=1 */
    plant(&f, enc_i_shift(0x00, 4, 1, 0x1, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(16, (int)f.cpu.regs[2]);
}

static void test_slli_max_shift(void) {
    /* shamt 31 — biggest legal shift */
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 1;
    plant(&f, enc_i_shift(0x00, 31, 1, 0x1, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x80000000u, (int)f.cpu.regs[2]);
}

static void test_srli(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    plant(&f, enc_i_shift(0x00, 4, 1, 0x5, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x08000000u, (int)f.cpu.regs[2]);
}

static void test_srai_negative(void) {
    /* Arithmetic shift right should sign-extend. */
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;   /* INT32_MIN */
    plant(&f, enc_i_shift(0x20, 4, 1, 0x5, 2, 0x13));
    ASSERT(run_one(&f));
    /* 0x80000000 >> 4 with sign extension = 0xF8000000 */
    ASSERT_EQ_INT((int)0xF8000000u, (int)f.cpu.regs[2]);
}

static void test_srai_positive(void) {
    /* For positive values, SRAI == SRLI. */
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x40000000u;
    plant(&f, enc_i_shift(0x20, 4, 1, 0x5, 2, 0x13));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x04000000u, (int)f.cpu.regs[2]);
}

/* ============================================================
 *  R-type ALU
 * ============================================================ */

static void test_add(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 100;
    f.cpu.regs[2] = 250;
    /* add x3, x1, x2 — funct7=0, funct3=0, opcode=0x33 */
    plant(&f, enc_r(0x00, 2, 1, 0x0, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(350, (int)f.cpu.regs[3]);
}

static void test_sub(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 100;
    f.cpu.regs[2] = 30;
    /* sub x3, x1, x2 — funct7=0x20 distinguishes from ADD */
    plant(&f, enc_r(0x20, 2, 1, 0x0, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(70, (int)f.cpu.regs[3]);
}

static void test_sub_underflow(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 10;
    f.cpu.regs[2] = 20;
    plant(&f, enc_r(0x20, 2, 1, 0x0, 3, 0x33));
    ASSERT(run_one(&f));
    /* 10 - 20 = -10 as int32, 0xFFFFFFF6 as uint32 */
    ASSERT_EQ_INT((int)0xFFFFFFF6u, (int)f.cpu.regs[3]);
}

static void test_sll(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 1;
    f.cpu.regs[2] = 8;
    plant(&f, enc_r(0x00, 2, 1, 0x1, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(256, (int)f.cpu.regs[3]);
}

static void test_sll_masks_shift(void) {
    /* Only low 5 bits of rs2 used as shift amount.
     * Shift by 33 is the same as shift by 1. */
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 1;
    f.cpu.regs[2] = 33;
    plant(&f, enc_r(0x00, 2, 1, 0x1, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(2, (int)f.cpu.regs[3]);
}

static void test_slt_signed_negative(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = (uint32_t)-1;
    f.cpu.regs[2] = 1;
    plant(&f, enc_r(0x00, 2, 1, 0x2, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(1, (int)f.cpu.regs[3]);
}

static void test_sltu_unsigned(void) {
    /* In unsigned compare, 0xFFFFFFFF > 1, so sltu returns 0. */
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFFFFFFu;
    f.cpu.regs[2] = 1;
    plant(&f, enc_r(0x00, 2, 1, 0x3, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[3]);
}

static void test_xor(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFF0000u;
    f.cpu.regs[2] = 0x00FFFF00u;
    plant(&f, enc_r(0x00, 2, 1, 0x4, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFF00FF00u, (int)f.cpu.regs[3]);
}

static void test_srl(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    f.cpu.regs[2] = 4;
    plant(&f, enc_r(0x00, 2, 1, 0x5, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x08000000u, (int)f.cpu.regs[3]);
}

static void test_sra_negative(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x80000000u;
    f.cpu.regs[2] = 4;
    plant(&f, enc_r(0x20, 2, 1, 0x5, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xF8000000u, (int)f.cpu.regs[3]);
}

static void test_or(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0x12000000u;
    f.cpu.regs[2] = 0x00345678u;
    plant(&f, enc_r(0x00, 2, 1, 0x6, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x12345678u, (int)f.cpu.regs[3]);
}

static void test_and(void) {
    AluFixture f;
    fixture_init(&f);
    f.cpu.regs[1] = 0xFFFF00FFu;
    f.cpu.regs[2] = 0x00FFFFFFu;
    plant(&f, enc_r(0x00, 2, 1, 0x7, 3, 0x33));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x00FF00FFu, (int)f.cpu.regs[3]);
}

static void test_op_rejects_bad_funct7(void) {
    /* funct7 = 0x40 (not 0x00 or 0x20) for an AND-like op
     * should be illegal. */
    AluFixture f;
    fixture_init(&f);
    plant(&f, enc_r(0x40, 2, 1, 0x7, 3, 0x33));
    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_ILLEGAL_INSTR, f.cpu.trap_cause);
}

/* ============================================================
 *  U-type — LUI, AUIPC
 * ============================================================ */

static void test_lui(void) {
    AluFixture f;
    fixture_init(&f);
    /* lui x1, 0xABCDE — places 0xABCDE in upper 20 bits */
    plant(&f, enc_u(0xABCDE000u, 1, 0x37));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xABCDE000u, (int)f.cpu.regs[1]);
}

static void test_auipc(void) {
    AluFixture f;
    fixture_init(&f);
    /* PC starts at 0. auipc x1, 0x10000 — rd = pc + 0x10000000 */
    plant(&f, enc_u(0x10000000u, 1, 0x17));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x10000000u, (int)f.cpu.regs[1]);
    ASSERT_EQ_INT(4, (int)f.cpu.pc);
}

static void test_auipc_at_offset(void) {
    /* Same instruction at a non-zero PC. Plant two NOPs first,
     * then the AUIPC at offset 8, then jump PC to 8 to execute
     * just the AUIPC. */
    AluFixture f;
    fixture_init(&f);
    plant_at(&f, 0, enc_i(0, 0, 0x0, 0, 0x13));   /* nop */
    plant_at(&f, 4, enc_i(0, 0, 0x0, 0, 0x13));   /* nop */
    plant_at(&f, 8, enc_u(0x10000000u, 1, 0x17)); /* auipc x1, 0x10000 */
    f.cpu.pc = 8;
    ASSERT(run_one(&f));
    /* rd = pc + 0x10000000 = 8 + 0x10000000 = 0x10000008 */
    ASSERT_EQ_INT((int)0x10000008u, (int)f.cpu.regs[1]);
    ASSERT_EQ_INT(12, (int)f.cpu.pc);
}

static void test_auipc_zero_imm(void) {
    /* auipc x1, 0 — reads current PC */
    AluFixture f;
    fixture_init(&f);
    plant_at(&f, 0, enc_u(0, 1, 0x17));
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[1]);
}

/* ============================================================
 *  Sequencing — run several instructions in a row
 * ============================================================ */

static void test_sequence_addi_chain(void) {
    AluFixture f;
    fixture_init(&f);
    /* addi x1, x0, 10  */
    plant_at(&f, 0, enc_i(10, 0, 0x0, 1, 0x13));
    /* addi x2, x1, 20  */
    plant_at(&f, 4, enc_i(20, 1, 0x0, 2, 0x13));
    /* add  x3, x1, x2  */
    plant_at(&f, 8, enc_r(0x00, 2, 1, 0x0, 3, 0x33));

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 3, &used);
    ASSERT_EQ_INT(VM_STEP_QUANTUM_EXPIRED, r);
    ASSERT_EQ_INT(3, (int)used);
    ASSERT_EQ_INT(10, (int)f.cpu.regs[1]);
    ASSERT_EQ_INT(30, (int)f.cpu.regs[2]);
    ASSERT_EQ_INT(40, (int)f.cpu.regs[3]);
    ASSERT_EQ_INT(12, (int)f.cpu.pc);
}

static void test_instructions_retired_counter(void) {
    AluFixture f;
    fixture_init(&f);
    /* Three NOPs (addi x0, x0, 0) */
    plant_at(&f, 0, enc_i(0, 0, 0x0, 0, 0x13));
    plant_at(&f, 4, enc_i(0, 0, 0x0, 0, 0x13));
    plant_at(&f, 8, enc_i(0, 0, 0x0, 0, 0x13));

    vm_step(&f.cpu, 3, NULL);
    ASSERT_EQ_INT(3, (int)f.cpu.instructions_retired);
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_core_alu");

    /* I-type ADDI */
    RUN(test_addi_positive);
    RUN(test_addi_negative_sign_extends);
    RUN(test_addi_max_negative);
    RUN(test_addi_uses_rs1);
    RUN(test_addi_overflow_wraps);
    RUN(test_addi_to_x0_squashed);

    /* I-type compares */
    RUN(test_slti_signed);
    RUN(test_slti_signed_false);
    RUN(test_sltiu_unsigned);
    RUN(test_sltiu_zero_idiom);

    /* I-type logic */
    RUN(test_xori);
    RUN(test_xori_minus_one_inverts);
    RUN(test_ori);
    RUN(test_andi);

    /* I-type shifts */
    RUN(test_slli);
    RUN(test_slli_max_shift);
    RUN(test_srli);
    RUN(test_srai_negative);
    RUN(test_srai_positive);

    /* R-type arithmetic */
    RUN(test_add);
    RUN(test_sub);
    RUN(test_sub_underflow);

    /* R-type shifts */
    RUN(test_sll);
    RUN(test_sll_masks_shift);

    /* R-type compares */
    RUN(test_slt_signed_negative);
    RUN(test_sltu_unsigned);

    /* R-type logic */
    RUN(test_xor);
    RUN(test_srl);
    RUN(test_sra_negative);
    RUN(test_or);
    RUN(test_and);

    /* Validation */
    RUN(test_op_rejects_bad_funct7);

    /* U-type */
    RUN(test_lui);
    RUN(test_auipc);
    RUN(test_auipc_at_offset);
    RUN(test_auipc_zero_imm);

    /* Sequencing */
    RUN(test_sequence_addi_chain);
    RUN(test_instructions_retired_counter);

    return TEST_SUITE_RESULT();
}
