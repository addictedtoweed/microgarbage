/* Tests for vm_core (chunk 1: memory access + step skeleton).
 *
 * Instruction-semantics tests are added in subsequent chunks
 * as RV32I / RV32M / RVC support comes online. These tests
 * cover only the plumbing: region translation, fetch path,
 * step-loop control flow.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  Test fixtures
 *
 *  set_up_cpu_with_regions: helper that hands the cpu four
 *  caller-owned region buffers. We use static buffers per test
 *  to keep things simple. */

/* ============================================================
 *  Lifecycle
 * ============================================================ */

static void test_init_clears_state(void) {
    VmCpu cpu;
    /* Pre-fill with garbage to confirm init clears it. */
    memset(&cpu, 0xAB, sizeof(cpu));
    vm_init(&cpu, 7);

    ASSERT_EQ_INT(7, cpu.vm_id);
    ASSERT_EQ_INT(0, (int)cpu.pc);
    ASSERT_EQ_INT(0, (int)cpu.regs[0]);
    ASSERT_EQ_INT(0, (int)cpu.regs[1]);
    ASSERT_EQ_INT(0, (int)cpu.regs[31]);
    ASSERT_EQ_INT(TRAP_NONE, cpu.trap_cause);
    ASSERT(!cpu.halted);
    ASSERT(!cpu.in_critical);
    ASSERT_EQ_INT(BLOCK_NONE, cpu.block_reason);
    ASSERT_EQ_INT(0, (int)cpu.instructions_retired);
}

static void test_reset_preserves_regions_and_id(void) {
    VmCpu cpu;
    uint8_t code_buf[16] = {0};
    vm_init(&cpu, 3);

    /* Pretend a loader set up regions and ran some instructions. */
    cpu.regions[VM_REGION_CODE].base = code_buf;
    cpu.regions[VM_REGION_CODE].length = sizeof(code_buf);
    cpu.regions[VM_REGION_CODE].writable = false;
    cpu.pc = 0x100;
    cpu.regs[1] = 0xDEADBEEF;
    cpu.instructions_retired = 42;

    vm_reset(&cpu);

    /* Regions and identity survive */
    ASSERT_EQ_PTR(code_buf, cpu.regions[VM_REGION_CODE].base);
    ASSERT_EQ_INT(16, (int)cpu.regions[VM_REGION_CODE].length);
    ASSERT_EQ_INT(3, cpu.vm_id);

    /* Architectural state is wiped */
    ASSERT_EQ_INT(0, (int)cpu.pc);
    ASSERT_EQ_INT(0, (int)cpu.regs[1]);
    ASSERT_EQ_INT(0, (int)cpu.instructions_retired);
}

/* ============================================================
 *  Region translation
 * ============================================================ */

static void test_translate_read_valid(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16] = {0xAA, 0xBB, 0xCC, 0xDD};
    cpu.regions[VM_REGION_CODE].base = buf;
    cpu.regions[VM_REGION_CODE].length = 16;

    const void *p = vm_translate_read(&cpu, 0x00000000, 4);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_PTR(buf, p);
    ASSERT_EQ_INT(TRAP_NONE, cpu.trap_cause);
}

static void test_translate_read_offset(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16];
    for (int i = 0; i < 16; i++) buf[i] = (uint8_t)i;
    cpu.regions[VM_REGION_DATA].base = buf;
    cpu.regions[VM_REGION_DATA].length = 16;

    /* Read 4 bytes starting at offset 8 in region 2. */
    const uint8_t *p = vm_translate_read(&cpu, 0x80000000u + 8, 4);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(8, p[0]);
    ASSERT_EQ_INT(11, p[3]);
}

static void test_translate_read_out_of_bounds(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16];
    cpu.regions[VM_REGION_DATA].base = buf;
    cpu.regions[VM_REGION_DATA].length = 16;

    /* 4-byte read starting at offset 14 would extend past 16 */
    const void *p = vm_translate_read(&cpu, 0x80000000u + 14, 4);
    ASSERT_NULL(p);
    ASSERT_EQ_INT(TRAP_LOAD_FAULT, cpu.trap_cause);
    ASSERT_EQ_INT((int)(0x80000000u + 14), (int)cpu.trap_addr);
}

static void test_translate_read_empty_region(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    /* SHARED region intentionally not populated. */
    const void *p = vm_translate_read(&cpu, 0xC0000000u, 1);
    ASSERT_NULL(p);
    ASSERT_EQ_INT(TRAP_LOAD_FAULT, cpu.trap_cause);
}

static void test_translate_write_readonly_traps(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16] = {0};
    cpu.regions[VM_REGION_CODE].base = buf;
    cpu.regions[VM_REGION_CODE].length = 16;
    cpu.regions[VM_REGION_CODE].writable = false;

    void *p = vm_translate_write(&cpu, 0x00000000, 4);
    ASSERT_NULL(p);
    ASSERT_EQ_INT(TRAP_STORE_RO, cpu.trap_cause);
}

static void test_translate_write_writable_ok(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16] = {0};
    cpu.regions[VM_REGION_DATA].base = buf;
    cpu.regions[VM_REGION_DATA].length = 16;
    cpu.regions[VM_REGION_DATA].writable = true;

    void *p = vm_translate_write(&cpu, 0x80000000u, 4);
    ASSERT_NOT_NULL(p);
    ASSERT_EQ_INT(TRAP_NONE, cpu.trap_cause);
}

/* ============================================================
 *  vm_read_u32 / vm_write_u32
 * ============================================================ */

static void test_read_u32(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    /* Little-endian: bytes 0x44 0x33 0x22 0x11 == 0x11223344 */
    uint8_t buf[16] = {0x44, 0x33, 0x22, 0x11};
    cpu.regions[VM_REGION_DATA].base = buf;
    cpu.regions[VM_REGION_DATA].length = 16;

    uint32_t v = vm_read_u32(&cpu, 0x80000000u);
    ASSERT_EQ_INT((int)0x11223344, (int)v);
    ASSERT_EQ_INT(TRAP_NONE, cpu.trap_cause);
}

static void test_read_u32_misaligned_traps(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16] = {0};
    cpu.regions[VM_REGION_DATA].base = buf;
    cpu.regions[VM_REGION_DATA].length = 16;

    vm_read_u32(&cpu, 0x80000001u);
    ASSERT_EQ_INT(TRAP_LOAD_MISALIGNED, cpu.trap_cause);
}

static void test_write_u32(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16] = {0};
    cpu.regions[VM_REGION_DATA].base = buf;
    cpu.regions[VM_REGION_DATA].length = 16;
    cpu.regions[VM_REGION_DATA].writable = true;

    ASSERT(vm_write_u32(&cpu, 0x80000000u + 4, 0xCAFEBABEu));
    /* Little-endian bytes: 0xBE 0xBA 0xFE 0xCA */
    ASSERT_EQ_INT(0xBE, buf[4]);
    ASSERT_EQ_INT(0xBA, buf[5]);
    ASSERT_EQ_INT(0xFE, buf[6]);
    ASSERT_EQ_INT(0xCA, buf[7]);
}

static void test_write_u32_to_readonly_traps(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16] = {0};
    cpu.regions[VM_REGION_CODE].base = buf;
    cpu.regions[VM_REGION_CODE].length = 16;
    cpu.regions[VM_REGION_CODE].writable = false;

    ASSERT(!vm_write_u32(&cpu, 0x00000000, 0xDEADBEEF));
    ASSERT_EQ_INT(TRAP_STORE_RO, cpu.trap_cause);
}

/* ============================================================
 *  vm_copy_*_guest
 * ============================================================ */

static void test_copy_from_guest(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16] = {0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE};
    cpu.regions[VM_REGION_DATA].base = buf;
    cpu.regions[VM_REGION_DATA].length = 16;

    uint8_t out[4] = {0};
    ASSERT(vm_copy_from_guest(&cpu, out, 0x80000000u, 4));
    ASSERT_EQ_INT(0xDE, out[0]);
    ASSERT_EQ_INT(0xAD, out[1]);
    ASSERT_EQ_INT(0xBE, out[2]);
    ASSERT_EQ_INT(0xEF, out[3]);
}

static void test_copy_to_guest(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t buf[16] = {0};
    cpu.regions[VM_REGION_DATA].base = buf;
    cpu.regions[VM_REGION_DATA].length = 16;
    cpu.regions[VM_REGION_DATA].writable = true;

    const uint8_t in[4] = {0x11, 0x22, 0x33, 0x44};
    ASSERT(vm_copy_to_guest(&cpu, 0x80000000u + 8, in, 4));
    ASSERT_EQ_INT(0x11, buf[8]);
    ASSERT_EQ_INT(0x22, buf[9]);
    ASSERT_EQ_INT(0x33, buf[10]);
    ASSERT_EQ_INT(0x44, buf[11]);
}

/* ============================================================
 *  vm_step — structural behavior (no instructions yet)
 * ============================================================ */

static void test_step_halted_returns_immediately(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.halted = true;
    uint32_t used = 99;
    ASSERT_EQ_INT(VM_STEP_HALTED, vm_step(&cpu, 1000, &used));
    ASSERT_EQ_INT(0, (int)used);
}

static void test_step_zero_budget(void) {
    /* Budget of 0 should return immediately with no work done. */
    VmCpu cpu;
    vm_init(&cpu, 0);
    /* Even with no code populated, budget=0 means we don't fetch. */
    uint32_t used = 99;
    ASSERT_EQ_INT(VM_STEP_QUANTUM_EXPIRED, vm_step(&cpu, 0, &used));
    ASSERT_EQ_INT(0, (int)used);
}

static void test_step_traps_on_illegal_instr(void) {
    /* Plant an instruction with an opcode that's not in our
     * decode table. 0x0000007F has opcode bits 7'b1111111 which
     * is currently unassigned in RV32IMC. */
    VmCpu cpu;
    vm_init(&cpu, 0);
    static uint8_t code[] = { 0x7F, 0x00, 0x00, 0x00 };
    cpu.regions[VM_REGION_CODE].base = code;
    cpu.regions[VM_REGION_CODE].length = sizeof(code);
    cpu.pc = 0;

    uint32_t used = 0;
    VmStepResult r = vm_step(&cpu, 10, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_ILLEGAL_INSTR, cpu.trap_cause);
    ASSERT_EQ_INT(0, (int)cpu.trap_pc);
    ASSERT_EQ_INT(0x0000007F, (int)cpu.trap_insn);
    ASSERT_EQ_INT(0, (int)used);
    ASSERT_EQ_INT(1, (int)cpu.trap_count);
}

static void test_step_traps_on_bad_pc_fetch(void) {
    /* PC points at an address with no backing region. */
    VmCpu cpu;
    vm_init(&cpu, 0);
    /* Don't populate any region. PC defaults to 0 in region 0
     * which has length 0 — fetch should fail with
     * TRAP_INSTR_FETCH_FAULT. */
    uint32_t used = 0;
    VmStepResult r = vm_step(&cpu, 10, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_INSTR_FETCH_FAULT, cpu.trap_cause);
}

static void test_step_traps_on_misaligned_pc(void) {
    VmCpu cpu;
    vm_init(&cpu, 0);
    uint8_t code[8] = {0};
    cpu.regions[VM_REGION_CODE].base = code;
    cpu.regions[VM_REGION_CODE].length = sizeof(code);
    cpu.pc = 1;   /* odd byte */

    uint32_t used = 0;
    VmStepResult r = vm_step(&cpu, 10, &used);
    ASSERT_EQ_INT(VM_STEP_TRAPPED, r);
    ASSERT_EQ_INT(TRAP_INSTR_MISALIGNED, cpu.trap_cause);
}

static void test_step_fetch_distinguishes_16_vs_32(void) {
    /* This is a fetch-path test: confirm the fetcher reads either
     * 2 or 4 bytes depending on the low 2 bits.
     *
     * A 32-bit instruction has low 2 bits == 0b11; everything else
     * is compressed. We use trap_insn — the dispatcher stores the
     * raw instruction bits into trap_insn when it traps as illegal.
     *
     * Plant a reserved compressed encoding (quadrant 0 with funct3=7,
     * which is C.SD on RV64C but reserved on RV32C). The dispatcher
     * should fetch only 2 bytes and trap with trap_insn holding the
     * 16-bit value zero-extended. */
    VmCpu cpu;
    vm_init(&cpu, 0);
    /* funct3=7, quadrant=0 → 0xE000. Reserved on RV32C. */
    static uint8_t code[] = { 0x00, 0xE0 };
    cpu.regions[VM_REGION_CODE].base = code;
    cpu.regions[VM_REGION_CODE].length = sizeof(code);
    cpu.pc = 0;

    uint32_t used = 0;
    vm_step(&cpu, 10, &used);
    ASSERT_EQ_INT(TRAP_ILLEGAL_INSTR, cpu.trap_cause);
    /* trap_insn should carry just the 16-bit value, zero-extended */
    ASSERT_EQ_INT(0x0000E000, (int)cpu.trap_insn);
}

/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_core");

    /* Lifecycle */
    RUN(test_init_clears_state);
    RUN(test_reset_preserves_regions_and_id);

    /* Translate */
    RUN(test_translate_read_valid);
    RUN(test_translate_read_offset);
    RUN(test_translate_read_out_of_bounds);
    RUN(test_translate_read_empty_region);
    RUN(test_translate_write_readonly_traps);
    RUN(test_translate_write_writable_ok);

    /* u32 access */
    RUN(test_read_u32);
    RUN(test_read_u32_misaligned_traps);
    RUN(test_write_u32);
    RUN(test_write_u32_to_readonly_traps);

    /* copy_*_guest */
    RUN(test_copy_from_guest);
    RUN(test_copy_to_guest);

    /* Step skeleton */
    RUN(test_step_halted_returns_immediately);
    RUN(test_step_zero_budget);
    RUN(test_step_traps_on_illegal_instr);
    RUN(test_step_traps_on_bad_pc_fetch);
    RUN(test_step_traps_on_misaligned_pc);
    RUN(test_step_fetch_distinguishes_16_vs_32);

    return TEST_SUITE_RESULT();
}
