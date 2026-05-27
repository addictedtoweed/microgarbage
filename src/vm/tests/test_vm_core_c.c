/* Tests for RV32C compressed instructions.
 *
 * Each test plants a 16-bit compressed instruction and verifies
 * that it produces the correct architectural effect after one
 * step. We don't separately verify the expansion shape — if the
 * effect is right, the expansion was right.
 *
 * The fixture uses PC increments of 2 so that step boundaries
 * line up with the compressed encoding.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    uint8_t code[128];
    uint8_t data[64];
    VmCpu   cpu;
} CFixture;

static void fixture_init(CFixture *f) {
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

static void plant16(CFixture *f, uint32_t offset, uint16_t c) {
    f->code[offset + 0] = (uint8_t)(c & 0xFF);
    f->code[offset + 1] = (uint8_t)((c >> 8) & 0xFF);
}

static int run_one(CFixture *f) {
    uint32_t used = 0;
    VmStepResult r = vm_step(&f->cpu, 1, &used);
    return (r == VM_STEP_QUANTUM_EXPIRED && used == 1) ? 1 : 0;
}

/* ============================================================
 *  Quadrant 0
 * ============================================================ */

static void test_c_addi4spn(void) {
    /* c.addi4spn x10 (creg=2 → x10), nzuimm=16
     * Encoding: 000 nzuimm[5:4|9:6|2|3] rd' 00
     *
     * nzuimm=16: bits [5:4]=00, [9:6]=0001, [3]=0, [2]=0 → no wait,
     * 16 = 0b10000, so bit 4 is set (nzuimm[4]=1).
     *   nzuimm[5:4]=01, nzuimm[9:6]=0000, nzuimm[3]=0, nzuimm[2]=0
     *
     * Layout: [15:13]=000, [12:11]=nzuimm[5:4]=01,
     *         [10:7]=nzuimm[9:6]=0000, [6]=nzuimm[2]=0,
     *         [5]=nzuimm[3]=0, [4:2]=rd'=010 (x10),
     *         [1:0]=00 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[2] = 1000;   /* x2 (sp) */
    /* Build the encoding for nzuimm=16, rd'=2:
     *   [12:11]=01 (nzuimm[5:4])
     *   [10:7]=0000, [6]=0, [5]=0 (other nzuimm bits zero)
     *   [4:2]=010 (rd')
     * → 0x0808 */
    plant16(&f, 0, 0x0808);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(1016, (int)f.cpu.regs[10]);
    ASSERT_EQ_INT(2, (int)f.cpu.pc);
}

static void test_c_addi4spn_zero_imm_illegal(void) {
    /* nzuimm = 0 is reserved/illegal. */
    CFixture f;
    fixture_init(&f);
    plant16(&f, 0, 0x0008);   /* funct3=0, all imm bits zero, rd'=010 */

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_ILLEGAL_INSTR, f.cpu.trap_cause);
}

static void test_c_lw(void) {
    /* c.lw x10 (rd'=010), 4(x9 (rs1'=001))
     *
     * imm = 4 → uimm[6]=0, uimm[5:3]=000, uimm[2]=1
     *
     * Layout: [15:13]=010 (funct3), [12:10]=uimm[5:3]=000,
     *         [9:7]=rs1'=001 (x9), [6]=uimm[2]=1, [5]=uimm[6]=0,
     *         [4:2]=rd'=010 (x10), [1:0]=00
     *
     * 0b 010 000 001 1 0 010 00 = 0x40C8 */
    CFixture f;
    fixture_init(&f);
    f.data[4] = 0xEF;
    f.data[5] = 0xBE;
    f.data[6] = 0xAD;
    f.data[7] = 0xDE;
    f.cpu.regs[9] = 0x80000000u;
    plant16(&f, 0, 0x40C8);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xDEADBEEFu, (int)f.cpu.regs[10]);
    ASSERT_EQ_INT(2, (int)f.cpu.pc);
}

static void test_c_sw(void) {
    /* c.sw rs2'=x10, 4(rs1'=x9)
     *
     * imm=4: uimm[6]=0, uimm[5:3]=000, uimm[2]=1
     *
     * funct3=110, [12:10]=000, [9:7]=001 (rs1'=x9), [6]=1, [5]=0,
     * [4:2]=010 (rs2'=x10), [1:0]=00
     * 0b 110 000 001 1 0 010 00 = 0xC0C8 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[9] = 0x80000000u;
    f.cpu.regs[10] = 0x12345678u;
    plant16(&f, 0, 0xC0C8);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x78, f.data[4]);
    ASSERT_EQ_INT(0x56, f.data[5]);
    ASSERT_EQ_INT(0x34, f.data[6]);
    ASSERT_EQ_INT(0x12, f.data[7]);
}

/* ============================================================
 *  Quadrant 1
 * ============================================================ */

static void test_c_addi(void) {
    /* c.addi x5, 5
     * Encoding: 000 nzimm[5] rd nzimm[4:0] 01
     * imm=5: bit 5=0, bits 4:0=00101
     * funct3=000, [12]=0, [11:7]=00101 (x5), [6:2]=00101 (=5), [1:0]=01
     * 0b 000 0 00101 00101 01 = 0x0295 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 10;
    plant16(&f, 0, 0x0295);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(15, (int)f.cpu.regs[5]);
    ASSERT_EQ_INT(2, (int)f.cpu.pc);
}

static void test_c_addi_negative(void) {
    /* c.addi x5, -1
     * imm=-1: bit 5=1 (sign), bits 4:0=11111
     * [12]=1, [11:7]=00101, [6:2]=11111, [1:0]=01
     * 0b 000 1 00101 11111 01 = 0x12FD */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 10;
    plant16(&f, 0, 0x12FD);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(9, (int)f.cpu.regs[5]);
}

static void test_c_nop(void) {
    /* c.nop = c.addi x0, 0 = 0x0001 */
    CFixture f;
    fixture_init(&f);
    plant16(&f, 0, 0x0001);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0, (int)f.cpu.regs[0]);   /* still zero */
    ASSERT_EQ_INT(2, (int)f.cpu.pc);
}

static void test_c_li(void) {
    /* c.li x5, -7
     * funct3=010, [12]=imm[5]=1, [11:7]=rd=00101, [6:2]=imm[4:0]=11001, [1:0]=01
     * imm[5:0]=111001 = -7 in 6-bit signed.
     * 0b 010 1 00101 11001 01 = 0x52E5 */
    CFixture f;
    fixture_init(&f);
    plant16(&f, 0, 0x52E5);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(-7, (int32_t)f.cpu.regs[5]);
}

static void test_c_lui(void) {
    /* c.lui x5, 1
     * imm[17:12] = 1, so resulting LUI imm = 0x1000 << 12 = 0x1000000
     * Wait — let me re-read the spec.
     *
     * C.LUI: lui rd, nzimm. The encoded nzimm provides bits [17:12]
     * of the LUI immediate. The LUI's full immediate is then
     * sign-extended from bit 17.
     *
     * For nzimm bit-pattern 000001 (imm[17:12]=0b000001), the LUI
     * immediate after sign-extension is 0x00001000 << 12 wait no.
     *
     * Let me just be careful: LUI sets rd = imm[31:12] << 12.
     * C.LUI provides imm[17:12]. So with imm[17:12]=0b000001 and
     * imm[31:18] sign-extended from imm[17]=0:
     *   imm[31:12] = 0x00001
     *   rd = 0x00001000
     *
     * Encoding: 011 imm[17] rd imm[16:12] 01
     * imm[17]=0, rd=00101 (x5), imm[16:12]=00001
     * 0b 011 0 00101 00001 01 = 0x6285 */
    CFixture f;
    fixture_init(&f);
    plant16(&f, 0, 0x6285);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x00001000, (int)f.cpu.regs[5]);
}

static void test_c_lui_negative(void) {
    /* c.lui x5, -1 (i.e., imm[17:12] = 0b111111)
     * Sign-extended: imm[31:12] = 0xFFFFF, so rd = 0xFFFFF000 */
    CFixture f;
    fixture_init(&f);
    /* [12]=imm[17]=1, [11:7]=00101, [6:2]=imm[16:12]=11111
     * 0b 011 1 00101 11111 01 = 0x72FD */
    plant16(&f, 0, 0x72FD);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xFFFFF000u, (int)f.cpu.regs[5]);
}

static void test_c_addi16sp(void) {
    /* c.addi16sp 32
     * nzimm = 32, bits [9:0]: 0000100000
     *   imm[9]=0 (sign), imm[8:7]=00, imm[6]=0, imm[5]=1, imm[4]=0
     *
     * Encoding: 011 nzimm[9] rd=00010 nzimm[4|6|8:7|5] 01
     *   [12]=0, [11:7]=00010, [6]=0, [5]=0, [4:3]=00, [2]=1, [1:0]=01
     * 0b 011 0 00010 0 0 00 1 01 = 0x6105 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[2] = 1000;
    plant16(&f, 0, 0x6105);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(1032, (int)f.cpu.regs[2]);
}

static void test_c_srli(void) {
    /* c.srli x8 (rd'=000), 4
     * funct3=100, [12]=shamt[5]=0, [11:10]=00, [9:7]=rd'=000 (x8),
     * [6:2]=shamt=00100, [1:0]=01
     * 0b 100 0 00 000 00100 01 = 0x8011 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 0xFF000000u;
    plant16(&f, 0, 0x8011);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x0FF00000u, (int)f.cpu.regs[8]);
}

static void test_c_srai(void) {
    /* c.srai x8, 4 — like c.srli but funct[11:10] = 01 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 0x80000000u;
    /* 0b 100 0 01 000 00100 01 = 0x8411 */
    plant16(&f, 0, 0x8411);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xF8000000u, (int)f.cpu.regs[8]);
}

static void test_c_andi(void) {
    /* c.andi x8, 0x0F
     * funct3=100, [12]=imm[5]=0, [11:10]=10, [9:7]=rd'=000,
     * [6:2]=imm[4:0]=01111, [1:0]=01
     * 0b 100 0 10 000 01111 01 = 0x883D */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 0xFFFFFFFFu;
    plant16(&f, 0, 0x883D);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x0F, (int)f.cpu.regs[8]);
}

static void test_c_sub(void) {
    /* c.sub x8 (rd'=000), x9 (rs2'=001)
     * funct3=100, [12:10]=011, [9:7]=rd'=000, [6:5]=00 (SUB),
     * [4:2]=rs2'=001, [1:0]=01
     * 0b 100 0 11 000 00 001 01 = 0x8C05 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 100;
    f.cpu.regs[9] = 30;
    plant16(&f, 0, 0x8C05);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(70, (int)f.cpu.regs[8]);
}

static void test_c_xor(void) {
    /* c.xor x8, x9: [6:5]=01 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 0xF0F0;
    f.cpu.regs[9] = 0x0FFF;
    plant16(&f, 0, 0x8C25);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0xFF0F, (int)f.cpu.regs[8]);
}

static void test_c_or(void) {
    /* c.or x8, x9: [6:5]=10 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 0xF000;
    f.cpu.regs[9] = 0x0F00;
    plant16(&f, 0, 0x8C45);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0xFF00, (int)f.cpu.regs[8]);
}

static void test_c_and(void) {
    /* c.and x8, x9: [6:5]=11 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 0xFFFF;
    f.cpu.regs[9] = 0x00FF;
    plant16(&f, 0, 0x8C65);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0x00FF, (int)f.cpu.regs[8]);
}

static void test_c_j(void) {
    /* c.j +8 — funct3=101, offset bits encode +8.
     * Easier: hand-assemble a known-good sequence.
     *
     * Actually let me compute carefully.
     * imm=8 (decimal), bits [11:1] = 00000000100 (bit 3 set).
     * Per the encoding:
     *   c[12]=imm[11]=0
     *   c[11]=imm[4]=0
     *   c[10:9]=imm[9:8]=00
     *   c[8]=imm[10]=0
     *   c[7]=imm[6]=0
     *   c[6]=imm[7]=0
     *   c[5:3]=imm[3:1]=100 (imm[3]=1)
     *   c[2]=imm[5]=0
     *   c[1:0]=01
     *
     * 0b 101 0 0 00 0 0 0 100 0 01 = 0xA021 */
    CFixture f;
    fixture_init(&f);
    plant16(&f, 0, 0xA021);
    /* Plant a nop at the target so we know it's reachable */
    plant16(&f, 8, 0x0001);

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(8, (int)f.cpu.pc);
    /* x1 was NOT linked (C.J doesn't link) */
    ASSERT_EQ_INT(0, (int)f.cpu.regs[1]);
}

static void test_c_jal(void) {
    /* c.jal +8 — same encoding as c.j but funct3=001, links x1 */
    CFixture f;
    fixture_init(&f);
    /* 0b 001 0 0 00 0 0 0 100 0 01 = 0x2021 */
    plant16(&f, 0, 0x2021);
    plant16(&f, 8, 0x0001);

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(8, (int)f.cpu.pc);
    /* Link address is pc + 2 (size of c.jal) */
    ASSERT_EQ_INT(2, (int)f.cpu.regs[1]);
}

static void test_c_beqz_taken(void) {
    /* c.beqz x8 (rs1'=000), +6 — funct3=110
     * imm=6 (bits [8:1]=00000011): bit 0=0, bit 1=1, bit 2=1
     *   c[12]=imm[8]=0
     *   c[11:10]=imm[4:3]=00
     *   c[6:5]=imm[7:6]=00
     *   c[4:3]=imm[2:1]=11
     *   c[2]=imm[5]=0
     *   [9:7]=rs1'=000
     * 0b 110 0 00 000 00 11 0 01 = 0xC019 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 0;
    plant16(&f, 0, 0xC019);
    plant16(&f, 6, 0x0001);

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(6, (int)f.cpu.pc);
}

static void test_c_beqz_not_taken(void) {
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 1;   /* non-zero, won't branch */
    plant16(&f, 0, 0xC019);

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(2, (int)f.cpu.pc);
}

static void test_c_bnez(void) {
    /* c.bnez x8, +6: funct3=111, same imm layout as c.beqz */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[8] = 1;
    /* 0b 111 0 00 000 00 11 0 01 = 0xE019 */
    plant16(&f, 0, 0xE019);
    plant16(&f, 6, 0x0001);

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(6, (int)f.cpu.pc);
}

/* ============================================================
 *  Quadrant 2
 * ============================================================ */

static void test_c_slli(void) {
    /* c.slli x5, 4
     * funct3=000, [12]=shamt[5]=0, [11:7]=rd=00101, [6:2]=shamt=00100, [1:0]=10
     * 0b 000 0 00101 00100 10 = 0x0292 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 1;
    plant16(&f, 0, 0x0292);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(16, (int)f.cpu.regs[5]);
}

static void test_c_lwsp(void) {
    /* c.lwsp x5, 4(x2)
     * uimm=4: uimm[5]=0, uimm[4:2]=001, uimm[7:6]=00
     * funct3=010, [12]=0, [11:7]=00101, [6:4]=001, [3:2]=00, [1:0]=10
     * 0b 010 0 00101 001 00 10 = 0x4292 */
    CFixture f;
    fixture_init(&f);
    f.data[4] = 0xCD;
    f.data[5] = 0xAB;
    f.data[6] = 0x89;
    f.data[7] = 0x67;
    f.cpu.regs[2] = 0x80000000u;
    plant16(&f, 0, 0x4292);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0x6789ABCDu, (int)f.cpu.regs[5]);
}

static void test_c_swsp(void) {
    /* c.swsp x5, 4(x2)
     * uimm=4: uimm[5:2]=0001, uimm[7:6]=00
     * funct3=110, [12:9]=0001, [8:7]=00, [6:2]=00101 (rs2=x5), [1:0]=10
     * 0b 110 0001 00 00101 10 = 0xC216 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[2] = 0x80000000u;
    f.cpu.regs[5] = 0xCAFEBABEu;
    plant16(&f, 0, 0xC216);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(0xBE, f.data[4]);
    ASSERT_EQ_INT(0xBA, f.data[5]);
    ASSERT_EQ_INT(0xFE, f.data[6]);
    ASSERT_EQ_INT(0xCA, f.data[7]);
}

static void test_c_jr(void) {
    /* c.jr x5 — bit12=0, rs1=x5, rs2=0
     * funct3=100, [12]=0, [11:7]=00101, [6:2]=00000, [1:0]=10
     * 0b 100 0 00101 00000 10 = 0x8282 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 16;
    plant16(&f, 0, 0x8282);
    plant16(&f, 16, 0x0001);

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(16, (int)f.cpu.pc);
    /* Did not link */
    ASSERT_EQ_INT(0, (int)f.cpu.regs[1]);
}

static void test_c_mv(void) {
    /* c.mv x5, x6 — bit12=0, rs2!=0
     * funct3=100, [12]=0, [11:7]=00101 (rd=x5), [6:2]=00110 (rs2=x6), [1:0]=10
     * 0b 100 0 00101 00110 10 = 0x829A */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[6] = 0xDEADBEEFu;
    plant16(&f, 0, 0x829A);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT((int)0xDEADBEEFu, (int)f.cpu.regs[5]);
}

static void test_c_jalr(void) {
    /* c.jalr x5 — bit12=1, rs1=x5, rs2=0
     * 0b 100 1 00101 00000 10 = 0x9282 */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 16;
    plant16(&f, 0, 0x9282);
    plant16(&f, 16, 0x0001);

    ASSERT(run_one(&f));
    ASSERT_EQ_INT(16, (int)f.cpu.pc);
    ASSERT_EQ_INT(2, (int)f.cpu.regs[1]);   /* linked to x1, pc+2 */
}

static void test_c_add(void) {
    /* c.add x5, x6 — bit12=1, rs2!=0
     * 0b 100 1 00101 00110 10 = 0x929A */
    CFixture f;
    fixture_init(&f);
    f.cpu.regs[5] = 100;
    f.cpu.regs[6] = 25;
    plant16(&f, 0, 0x929A);
    ASSERT(run_one(&f));
    ASSERT_EQ_INT(125, (int)f.cpu.regs[5]);
}

static void test_c_ebreak(void) {
    /* c.ebreak — 0x9002 (bit12=1, rs1=0, rs2=0) */
    CFixture f;
    fixture_init(&f);
    plant16(&f, 0, 0x9002);

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 1, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_BREAKPOINT, f.cpu.trap_cause);
}

/* ============================================================
 *  Mixed 16-bit / 32-bit instruction streams
 * ============================================================ */

static void test_mixed_compressed_and_32bit(void) {
    /* Plant: c.li x5, 10  (16-bit)
     *        addi x5, x5, 5 (32-bit)
     *        c.nop          (16-bit)
     *
     * Verify PC advances by 2, 4, 2. */
    CFixture f;
    fixture_init(&f);
    /* c.li x5, 10 — funct3=010, [12]=0, [11:7]=00101, [6:2]=01010, [1:0]=01
     * 0b 010 0 00101 01010 01 = 0x42A9 */
    plant16(&f, 0, 0x42A9);
    /* 32-bit addi x5, x5, 5: imm=5, rs1=5, funct3=0, rd=5, op=0x13 */
    uint32_t addi32 = (5u << 20) | (5u << 15) | (0u << 12) | (5u << 7) | 0x13u;
    f.code[2] = (uint8_t)(addi32 & 0xFF);
    f.code[3] = (uint8_t)((addi32 >> 8) & 0xFF);
    f.code[4] = (uint8_t)((addi32 >> 16) & 0xFF);
    f.code[5] = (uint8_t)((addi32 >> 24) & 0xFF);
    /* c.nop */
    plant16(&f, 6, 0x0001);

    uint32_t used = 0;
    VmStepResult r = vm_step(&f.cpu, 3, &used);
    ASSERT_EQ_INT(VM_STEP_QUANTUM_EXPIRED, r);
    ASSERT_EQ_INT(3, (int)used);
    ASSERT_EQ_INT(15, (int)f.cpu.regs[5]);   /* 10 + 5 */
    ASSERT_EQ_INT(8, (int)f.cpu.pc);          /* 2 + 4 + 2 */
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_core_c");

    /* Quadrant 0 */
    RUN(test_c_addi4spn);
    RUN(test_c_addi4spn_zero_imm_illegal);
    RUN(test_c_lw);
    RUN(test_c_sw);

    /* Quadrant 1 */
    RUN(test_c_addi);
    RUN(test_c_addi_negative);
    RUN(test_c_nop);
    RUN(test_c_li);
    RUN(test_c_lui);
    RUN(test_c_lui_negative);
    RUN(test_c_addi16sp);
    RUN(test_c_srli);
    RUN(test_c_srai);
    RUN(test_c_andi);
    RUN(test_c_sub);
    RUN(test_c_xor);
    RUN(test_c_or);
    RUN(test_c_and);
    RUN(test_c_j);
    RUN(test_c_jal);
    RUN(test_c_beqz_taken);
    RUN(test_c_beqz_not_taken);
    RUN(test_c_bnez);

    /* Quadrant 2 */
    RUN(test_c_slli);
    RUN(test_c_lwsp);
    RUN(test_c_swsp);
    RUN(test_c_jr);
    RUN(test_c_mv);
    RUN(test_c_jalr);
    RUN(test_c_add);
    RUN(test_c_ebreak);

    /* Mixed streams */
    RUN(test_mixed_compressed_and_32bit);

    return TEST_SUITE_RESULT();
}
