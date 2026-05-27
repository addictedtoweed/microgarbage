/* Tests for RV32I system instructions:
 *   FENCE/FENCE.I
 *   ECALL
 *   EBREAK
 *   CSRRW, CSRRS, CSRRC (register-source variants)
 *   CSRRWI, CSRRSI, CSRRCI (immediate-source variants)
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  Encoding helpers
 * ============================================================ */

static uint32_t enc_i_raw(uint32_t imm12, uint32_t rs1,
                          uint32_t funct3, uint32_t rd, uint32_t opcode) {
    return ((imm12 & 0xFFFu) << 20) | (rs1 << 15) |
           (funct3 << 12) | (rd << 7) | opcode;
}

/* Specific encodings */
static uint32_t enc_ecall(void)  { return 0x00000073u; }
static uint32_t enc_ebreak(void) { return 0x00100073u; }
static uint32_t enc_fence(void)  { return 0x0000000Fu; }   /* nop fence */
static uint32_t enc_fence_i(void){ return 0x0000100Fu; }   /* funct3 = 1 */

static uint32_t enc_csr(uint32_t csr, uint32_t rs1_or_imm,
                        uint32_t funct3, uint32_t rd) {
    return enc_i_raw(csr, rs1_or_imm, funct3, rd, 0x73);
}

/* ============================================================
 *  Fixture
 * ============================================================ */

typedef struct {
    uint8_t code[64];
    VmCpu   cpu;
} SysFixture;

static void fixture_init(SysFixture *f) {
    memset(f, 0, sizeof(*f));
    vm_init(&f->cpu, 7);   /* non-zero vm_id so we can test mhartid */
    f->cpu.regions[VM_REGION_CODE].base = f->code;
    f->cpu.regions[VM_REGION_CODE].length = sizeof(f->code);
    f->cpu.regions[VM_REGION_CODE].writable = false;
    f->cpu.pc = 0;
}

static void plant_at(SysFixture *f, uint32_t offset, uint32_t insn) {
    f->code[offset + 0] = (uint8_t)(insn & 0xFF);
    f->code[offset + 1] = (uint8_t)((insn >> 8) & 0xFF);
    f->code[offset + 2] = (uint8_t)((insn >> 16) & 0xFF);
    f->code[offset + 3] = (uint8_t)((insn >> 24) & 0xFF);
}

static void plant(SysFixture *f, uint32_t insn) {
    plant_at(f, f->cpu.pc, insn);
}

static int run_one(SysFixture *f) {
    uint32_t used = 0;
    VmStepResult r = vm_step(&f->cpu, 1, &used);
    return (r == VM_STEP_QUANTUM_EXPIRED && used == 1) ? 1 : 0;
}

/* ============================================================
 *  FENCE / FENCE.I
 * ============================================================ */

static void test_fence_is_nop(void) {
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_fence());
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(4, (int)f.cpu.pc);
}

static void test_fence_i_is_nop(void) {
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_fence_i());
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(4, (int)f.cpu.pc);
}

static void test_fence_invalid_funct3_traps(void) {
    SysFixture f;
    fixture_init(&f);
    /* funct3 = 2 is not a valid FENCE variant */
    plant(&f, 0x0000200Fu);

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_ILLEGAL_INSTR, f.cpu.trap_cause);
}

/* ============================================================
 *  ECALL
 * ============================================================ */

static void test_ecall_returns_step_ecall(void) {
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_ecall());

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 5, &used);
    ASSERT_EQ_INT(VM_STEP_ECALL, r);
    ASSERT_EQ_INT(TRAP_ECALL, f.cpu.trap_cause);
}

static void test_ecall_advances_pc(void) {
    /* Per the header contract, ecall returns with PC already
     * advanced past the ecall instruction. */
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_ecall());

    vm_step(&f.cpu, 5, NULL);
    ASSERT_EQ_INT(4, (int)f.cpu.pc);
    /* trap_pc points at the ecall itself for diagnostics. */
    ASSERT_EQ_INT(0, (int)f.cpu.trap_pc);
}

static void test_ecall_bumps_ecall_count_not_trap_count(void) {
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_ecall());

    vm_step(&f.cpu, 5, NULL);
    ASSERT_EQ_INT(1, (int)f.cpu.ecall_count);
    ASSERT_EQ_INT(0, (int)f.cpu.trap_count);
}

static void test_ecall_does_not_retire_instruction(void) {
    /* The ecall didn't successfully complete (it exited the
     * dispatch loop with a trap), so it shouldn't be counted as
     * a retired instruction. */
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_ecall());

    vm_step(&f.cpu, 5, NULL);
    ASSERT_EQ_INT(0, (int)f.cpu.instructions_retired);
}

static void test_ecall_resumable(void) {
    /* After an ecall, the scheduler (or test) can call vm_step
     * again and execution resumes at the instruction after the
     * ecall (PC was already advanced). */
    SysFixture f;
    fixture_init(&f);
    plant_at(&f, 0, enc_ecall());
    /* addi x1, x0, 42 at offset 4 */
    plant_at(&f, 4, 0x02A00093u);

    /* First call: hit ecall */
    vm_step(&f.cpu, 5, NULL);
    ASSERT_EQ_INT(4, (int)f.cpu.pc);

    /* Resume: should run the ADDI */
    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 5, &used);
    /* No more instructions plantd at PC=8, will trap */
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(42, (int)f.cpu.regs[1]);
    ASSERT_EQ_INT(1, (int)used);
}

/* ============================================================
 *  EBREAK
 * ============================================================ */

static void test_ebreak_returns_trapped(void) {
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_ebreak());

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 5, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_BREAKPOINT, f.cpu.trap_cause);
}

static void test_ebreak_does_not_advance_pc(void) {
    /* The debugger needs PC pointing at the ebreak so it can
     * replace the instruction and re-execute. */
    SysFixture f;
    fixture_init(&f);
    plant_at(&f, 8, enc_ebreak());
    f.cpu.pc = 8;

    vm_step(&f.cpu, 5, NULL);
    ASSERT_EQ_INT(8, (int)f.cpu.pc);
    ASSERT_EQ_INT(8, (int)f.cpu.trap_pc);
}

static void test_ebreak_bumps_trap_count(void) {
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_ebreak());

    vm_step(&f.cpu, 5, NULL);
    ASSERT_EQ_INT(1, (int)f.cpu.trap_count);
    ASSERT_EQ_INT(0, (int)f.cpu.ecall_count);
}

/* ============================================================
 *  CSR reads
 * ============================================================ */

static void test_csrrs_reads_mhartid(void) {
    /* mhartid = vm_id = 7 (set by fixture). Use CSRRS rd, csr, x0
     * which is the canonical "read CSR" idiom. */
    SysFixture f;
    fixture_init(&f);
    /* csrrs x1, mhartid (0xF14), x0 — funct3=0x2 */
    plant(&f, enc_csr(0xF14, 0, 0x2, 1));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(7, (int)f.cpu.regs[1]);
}

static void test_csrrs_reads_cycle_zero_initially(void) {
    SysFixture f;
    fixture_init(&f);
    /* csrrs x1, cycle (0xC00), x0 — read cycle (instructions retired) */
    plant(&f, enc_csr(0xC00, 0, 0x2, 1));

    ASSERT(run_one(&f));
    /* This is the first retired instruction. The CSR is read
     * BEFORE the retirement counter ticks (see csr_read in
     * vm_core.c — it reads instructions_retired, which is bumped
     * by step_once only after execute_one returns true).
     *
     * So the value read here should be 0. */
    ASSERT_EQ_INT(0, (int)f.cpu.regs[1]);
    /* After this instruction retires, the counter is 1. */
    ASSERT_EQ_INT(1, (int)f.cpu.instructions_retired);
}

static void test_csrrs_reads_cycle_after_several_insns(void) {
    SysFixture f;
    fixture_init(&f);
    /* 3 NOPs, then csrrs x1, cycle, x0 */
    plant_at(&f, 0,  0x00000013u);   /* addi x0,x0,0 */
    plant_at(&f, 4,  0x00000013u);
    plant_at(&f, 8,  0x00000013u);
    plant_at(&f, 12, enc_csr(0xC00, 0, 0x2, 1));

    vm_step(&f.cpu, 4, NULL);
    /* After 3 retired NOPs, the cycle CSR reads 3 (the fourth
     * instruction's read happens before its own retirement). */
    ASSERT_EQ_INT(3, (int)f.cpu.regs[1]);
}

static void test_csrrs_reads_cycleh(void) {
    /* Force a high-half value */
    SysFixture f;
    fixture_init(&f);
    f.cpu.instructions_retired = (uint64_t)0x12345678u << 32 | 0xABCDEF00u;
    plant(&f, enc_csr(0xC80, 0, 0x2, 1));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x12345678, (int)f.cpu.regs[1]);
}

static void test_csrrs_unknown_csr_returns_zero(void) {
    SysFixture f;
    fixture_init(&f);
    /* Unknown CSR address 0x000 */
    plant(&f, enc_csr(0x000, 0, 0x2, 1));

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[1]);
}

/* ============================================================
 *  CSR writes — all no-ops in this shim, but the read-old-value
 *  side is observable
 * ============================================================ */

static void test_csrrw_reads_old_value(void) {
    /* CSRRW returns the OLD csr value in rd and replaces csr with rs1.
     * In our shim the replace is a no-op, but the read side works. */
    SysFixture f;
    fixture_init(&f);
    f.cpu.regs[2] = 0xDEADBEEFu;   /* value to "write" */
    /* csrrw x1, mhartid (=7), x2 */
    plant(&f, enc_csr(0xF14, 2, 0x1, 1));

    ASSERT(run_one(&f));
    /* rd gets the OLD csr value */
    ASSERT_EQ_INT(7, (int)f.cpu.regs[1]);
    /* CSR write was ignored; reading mhartid again still returns 7 */
    /* (We can't easily verify a second read in this single-instruction
     * test, but the shim's contract is that writes are no-ops.) */
}

static void test_csrrwi_uses_immediate(void) {
    SysFixture f;
    fixture_init(&f);
    /* csrrwi x1, mhartid, 0x15 — funct3=0x5, rs1 field IS the immediate */
    plant(&f, enc_csr(0xF14, 0x15, 0x5, 1));

    ASSERT(run_one(&f));
    /* Reads old CSR value (mhartid = 7) */
    ASSERT_EQ_INT(7, (int)f.cpu.regs[1]);
}

static void test_csrrs_zero_source_does_not_write(void) {
    /* CSRRS with rs1=x0 is the canonical "read-only access".
     * Our csr_write is a no-op anyway, but the logic shouldn't
     * even invoke it. We can't directly observe this, but we
     * can confirm the instruction completes normally. */
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_csr(0xC00, 0, 0x2, 1));   /* csrrs x1, cycle, x0 */

    ASSERT(run_one(&f));
    /* Sanity check: read returned a value, PC advanced */
    ASSERT_EQ_INT(0, (int)f.cpu.regs[1]);
    ASSERT_EQ_INT(4, (int)f.cpu.pc);
}

static void test_csrrc_basic(void) {
    /* CSRRC: rd = csr; csr &= ~rs1. */
    SysFixture f;
    fixture_init(&f);
    f.cpu.regs[2] = 0xFFFFFFFFu;
    plant(&f, enc_csr(0xF14, 2, 0x3, 1));   /* csrrc x1, mhartid, x2 */

    ASSERT(run_one(&f));
    /* Old value */
    ASSERT_EQ_INT(7, (int)f.cpu.regs[1]);
}

static void test_csr_invalid_funct3_traps(void) {
    /* funct3 = 0x4 is unassigned in SYSTEM. */
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_csr(0xC00, 0, 0x4, 1));

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_ILLEGAL_INSTR, f.cpu.trap_cause);
}

static void test_csr_write_to_x0_squashed(void) {
    /* The dispatcher already squashes writes to x0, but let's
     * verify CSR reads composed with rd=x0 don't leave garbage. */
    SysFixture f;
    fixture_init(&f);
    plant(&f, enc_csr(0xF14, 0, 0x2, 0));   /* csrrs x0, mhartid, x0 */

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[0]);
}

/* ============================================================
 *  Sequencing
 * ============================================================ */

static void test_ecall_in_middle_of_quantum(void) {
    /* A few ALU instructions then an ECALL. The dispatcher should
     * report VM_STEP_ECALL with used = count up to the ecall. */
    SysFixture f;
    fixture_init(&f);
    plant_at(&f, 0, 0x00100093u);   /* addi x1, x0, 1 */
    plant_at(&f, 4, 0x00200113u);   /* addi x2, x0, 2 */
    plant_at(&f, 8, enc_ecall());

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 10, &used);
    ASSERT_EQ_INT(VM_STEP_ECALL, r);
    /* Two ADDIs retired, then ECALL exited. The ECALL itself
     * doesn't count as retired. */
    ASSERT_EQ_INT(2, (int)used);
    ASSERT_EQ_INT(1, (int)f.cpu.regs[1]);
    ASSERT_EQ_INT(2, (int)f.cpu.regs[2]);
    /* PC advanced past the ecall */
    ASSERT_EQ_INT(12, (int)f.cpu.pc);
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_core_system");

    /* FENCE */
    RUN(test_fence_is_nop);
    RUN(test_fence_i_is_nop);
    RUN(test_fence_invalid_funct3_traps);

    /* ECALL */
    RUN(test_ecall_returns_step_ecall);
    RUN(test_ecall_advances_pc);
    RUN(test_ecall_bumps_ecall_count_not_trap_count);
    RUN(test_ecall_does_not_retire_instruction);
    RUN(test_ecall_resumable);

    /* EBREAK */
    RUN(test_ebreak_returns_trapped);
    RUN(test_ebreak_does_not_advance_pc);
    RUN(test_ebreak_bumps_trap_count);

    /* CSR reads */
    RUN(test_csrrs_reads_mhartid);
    RUN(test_csrrs_reads_cycle_zero_initially);
    RUN(test_csrrs_reads_cycle_after_several_insns);
    RUN(test_csrrs_reads_cycleh);
    RUN(test_csrrs_unknown_csr_returns_zero);

    /* CSR writes / immediates / variants */
    RUN(test_csrrw_reads_old_value);
    RUN(test_csrrwi_uses_immediate);
    RUN(test_csrrs_zero_source_does_not_write);
    RUN(test_csrrc_basic);
    RUN(test_csr_invalid_funct3_traps);
    RUN(test_csr_write_to_x0_squashed);

    /* Combined */
    RUN(test_ecall_in_middle_of_quantum);

    return TEST_SUITE_RESULT();
}
