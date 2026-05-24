/* Tests for vm_system.
 *
 * Most tests build a small ELF in memory, load it, run it, and
 * check the result. The ELF synthesizer is the same pattern as
 * test_vm_loader.c. Programs are hand-assembled small enough to
 * fit a few syscalls and check a register state. */

#include "test_runner.h"
#include "vm/vm_system.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>

/* ============================================================
 *  Storage pools — generous enough for several VMs
 * ============================================================ */

#define SHARED_BYTES  (64 * 1024)
/* Sized for max_vms=2 spawn_data_kb=4 (the per-test config below).
 * The slab allocator carves bins for each VM's allocations plus
 * headroom; this comfortably covers it. */
#define LOCAL_BYTES   (128 * 1024)

static uint8_t g_shared_storage[SHARED_BYTES];
static uint8_t g_local_storage[LOCAL_BYTES];

/* ============================================================
 *  ELF synthesizer — minimal, just code + entry point
 *
 *  Builds an RV32 ELF with a single PT_LOAD covering a code
 *  region populated with caller-supplied bytes. No rodata, no
 *  data segment in the ELF (so the loader supplies data_region
 *  fresh from the arena). entry = 0.
 * ============================================================ */

#define EHDR_SIZE  52u
#define PHDR_SIZE  32u

static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* Builds a minimal ELF into buf with one PT_LOAD for code.
 * code is placed at file offset 84 (EHDR + 1 PHDR + 0 pad).
 * Returns total ELF size. */
static size_t build_code_only_elf(uint8_t *buf, size_t cap,
                                  const uint8_t *code, size_t code_size) {
    if (cap < EHDR_SIZE + PHDR_SIZE + code_size) return 0;

    memset(buf, 0, cap);

    /* ELF header */
    buf[0] = 0x7F; buf[1] = 'E'; buf[2] = 'L'; buf[3] = 'F';
    buf[4] = 1;    /* ELFCLASS32 */
    buf[5] = 1;    /* ELFDATA2LSB */
    buf[6] = 1;    /* EV_CURRENT */
    wr16(buf + 16, 2);       /* e_type = ET_EXEC */
    wr16(buf + 18, 0xF3);    /* e_machine = EM_RISCV */
    wr32(buf + 20, 1);       /* e_version */
    wr32(buf + 24, 0);       /* e_entry = 0 (region 0 / CODE) */
    wr32(buf + 28, EHDR_SIZE);  /* e_phoff */
    wr32(buf + 32, 0);       /* e_shoff */
    wr32(buf + 36, 0);       /* e_flags */
    wr16(buf + 40, EHDR_SIZE);  /* e_ehsize */
    wr16(buf + 42, PHDR_SIZE);  /* e_phentsize */
    wr16(buf + 44, 1);       /* e_phnum */

    /* PT_LOAD program header — places code at vaddr 0 (region 0). */
    uint8_t *ph = buf + EHDR_SIZE;
    uint32_t code_offset = EHDR_SIZE + PHDR_SIZE;
    wr32(ph + 0,  1);                /* PT_LOAD */
    wr32(ph + 4,  code_offset);      /* p_offset */
    wr32(ph + 8,  0);                /* p_vaddr = 0 */
    wr32(ph + 12, 0);                /* p_paddr */
    wr32(ph + 16, (uint32_t)code_size);  /* p_filesz */
    wr32(ph + 20, (uint32_t)code_size);  /* p_memsz */
    wr32(ph + 24, 0x5);              /* p_flags = R | X */
    wr32(ph + 28, 4);                /* p_align */

    memcpy(buf + code_offset, code, code_size);
    return code_offset + code_size;
}

/* ============================================================
 *  Small RISC-V code helpers
 *
 *  Hand-assemble specific instructions we need.
 * ============================================================ */

/* addi rd, rs1, imm */
static uint32_t addi(uint32_t rd, uint32_t rs1, int32_t imm) {
    return ((((uint32_t)imm) & 0xFFFu) << 20) | (rs1 << 15) |
           (0u << 12) | (rd << 7) | 0x13u;
}

/* lui rd, imm (imm in upper 20 bits as-is) */
static uint32_t lui(uint32_t rd, uint32_t imm20) {
    return ((imm20 & 0xFFFFFu) << 12) | (rd << 7) | 0x37u;
}

/* ecall */
static uint32_t ecall(void) { return 0x00000073u; }

/* ebreak — used to stop execution cleanly (the trap handler
 * terminates the VM). */
static uint32_t ebreak(void) { return 0x00100073u; }

/* sw rs2, imm(rs1) */
static uint32_t sw_(uint32_t rs2, uint32_t rs1, int32_t imm) {
    uint32_t uimm = ((uint32_t)imm) & 0xFFFu;
    uint32_t hi = (uimm >> 5) & 0x7Fu;
    uint32_t lo = uimm & 0x1Fu;
    return (hi << 25) | (rs2 << 20) | (rs1 << 15) |
           (2u << 12) | (lo << 7) | 0x23u;
}

/* lw rd, imm(rs1) */
static uint32_t lw_(uint32_t rd, uint32_t rs1, int32_t imm) {
    return ((((uint32_t)imm) & 0xFFFu) << 20) | (rs1 << 15) |
           (2u << 12) | (rd << 7) | 0x03u;
}

#define REG_A0  10
#define REG_A1  11
#define REG_A2  12
#define REG_A7  17

/* Load a 32-bit constant into rd: lui then addi.
 * Writes 2 instructions to buf and returns 8 (bytes written). */
static size_t load_imm32(uint8_t *buf, uint32_t rd, uint32_t value) {
    /* Sign-extend addi part: if low 12 bits have bit 11 set, the
     * lui needs to compensate by adding 1 to the upper 20 bits. */
    uint32_t lo = value & 0xFFFu;
    uint32_t hi = (value - (int32_t)((lo & 0x800u) ? (lo | 0xFFFFF000u) : lo)) >> 12;
    hi &= 0xFFFFFu;

    uint32_t lui_insn = lui(rd, hi);
    uint32_t addi_insn = addi(rd, rd, (int32_t)((lo & 0x800u) ? (lo | 0xFFFFF000u) : lo));
    wr32(buf + 0, lui_insn);
    wr32(buf + 4, addi_insn);
    return 8;
}

/* ============================================================
 *  Init / destroy
 * ============================================================ */

static void test_init_succeeds(void) {
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    ASSERT(sys.ecall_router != NULL);
    ASSERT(sys.sched != NULL);
    ASSERT(sys.shared_slab != NULL);
    ASSERT(sys.local_slab != NULL);
    /* Defaults filled in */
    ASSERT_EQ_INT(5000, (int)sys.config.baseline_quantum);

    vm_system_destroy(&sys);
}

static void test_init_rejects_missing_storage(void) {
    VmSystem sys;
    VmSystemConfig cfg = {0};   /* no storage pointers */
    ASSERT(!vm_system_init(&sys, &cfg));
}

static void test_init_rejects_null_args(void) {
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
    };
    ASSERT(!vm_system_init(NULL, &cfg));

    VmSystem sys;
    ASSERT(!vm_system_init(&sys, NULL));
}

/* ============================================================
 *  Load a tiny VM and run it
 * ============================================================ */

static void test_load_and_run_exit_vm(void) {
    /* Program: set a7=SYS_EXIT, ecall.
     *   addi a7, x0, 93
     *   ecall
     *   ebreak  ; safety, won't be reached
     */
    uint8_t code[64];
    size_t pos = 0;
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;
    wr32(code + pos, ebreak()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);
    ASSERT(elf_size > 0);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size,
                                           /*data_region=*/4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);
    ASSERT_EQ_INT(0, lr.assigned_vm_id);

    /* Run the system; should complete (all halted) very quickly. */
    bool done = vm_system_run(&sys, 100);
    ASSERT(done);

    vm_system_destroy(&sys);
}

static void test_load_and_run_self_vm(void) {
    /* Program: a7 = SYS_SELF, ecall, then a7 = SYS_EXIT, ecall.
     * After exit, the VM should have its vm_id in a0 (from SELF). */
    uint8_t code[64];
    size_t pos = 0;
    wr32(code + pos, addi(REG_A7, 0, 1024)); pos += 4;  /* SYS_SELF */
    wr32(code + pos, ecall()); pos += 4;
    /* Save a0 to a1 so SYS_EXIT's a0 (exit code) doesn't clobber it */
    wr32(code + pos, addi(REG_A1, REG_A0, 0)); pos += 4;  /* mv a1, a0 */
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);

    bool done = vm_system_run(&sys, 100);
    ASSERT(done);

    /* a1 should hold the vm_id (=0) */
    VmCpu *cpu = vm_sched_get(sys.sched, (uint16_t)lr.assigned_vm_id);
    ASSERT(cpu != NULL);
    ASSERT_EQ_INT(0, (int)cpu->regs[REG_A1]);

    vm_system_destroy(&sys);
}

/* ============================================================
 *  Alloc and free
 * ============================================================ */

static void test_alloc_and_free(void) {
    /* Program: SYS_ALLOC 64 bytes, save pointer, SYS_FREE, exit.
     *
     *   addi a0, x0, 64
     *   addi a7, x0, 1056  ; SYS_ALLOC
     *   ecall
     *   addi a1, a0, 0     ; save pointer
     *   ; check that pointer is in shared region (not negative)
     *   addi a2, a0, 0     ; save again before free
     *   addi a7, x0, 1057  ; SYS_FREE
     *   ecall              ; a0 still has the pointer
     *   addi a7, x0, 93
     *   ecall
     */
    uint8_t code[128];
    size_t pos = 0;
    wr32(code + pos, addi(REG_A0, 0, 64)); pos += 4;
    pos += load_imm32(code + pos, REG_A7, 1056);  /* SYS_ALLOC */
    wr32(code + pos, ecall()); pos += 4;
    wr32(code + pos, addi(REG_A1, REG_A0, 0)); pos += 4;  /* save ptr */
    /* a0 already has the pointer; SYS_FREE wants it in a0 */
    pos += load_imm32(code + pos, REG_A7, 1057);  /* SYS_FREE */
    wr32(code + pos, ecall()); pos += 4;
    wr32(code + pos, addi(REG_A2, REG_A0, 0)); pos += 4;  /* save free result */
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);

    bool done = vm_system_run(&sys, 200);
    ASSERT(done);

    VmCpu *cpu = vm_sched_get(sys.sched, (uint16_t)lr.assigned_vm_id);
    ASSERT(cpu != NULL);
    /* a1 saved the alloc result; should be a valid shared-region address
     * (≥ 0xC0000000) */
    ASSERT((cpu->regs[REG_A1] & 0xC0000000u) == 0xC0000000u);
    /* a2 saved the free result; should be 0 */
    ASSERT_EQ_INT(0, (int)cpu->regs[REG_A2]);

    vm_system_destroy(&sys);
}

static void test_alloc_zero_returns_einval(void) {
    /* SYS_ALLOC with size=0 should return -EINVAL */
    uint8_t code[128];
    size_t pos = 0;
    /* a0 = 0 already (registers init to 0) */
    pos += load_imm32(code + pos, REG_A7, 1056);
    wr32(code + pos, ecall()); pos += 4;
    wr32(code + pos, addi(REG_A1, REG_A0, 0)); pos += 4;
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);
    vm_system_run(&sys, 100);

    VmCpu *cpu = vm_sched_get(sys.sched, (uint16_t)lr.assigned_vm_id);
    /* a1 saved the alloc result; should be -EINVAL = -22 = 0xFFFFFFEA */
    ASSERT_EQ_INT(-22, (int32_t)cpu->regs[REG_A1]);

    vm_system_destroy(&sys);
}

/* ============================================================
 *  Unload / auto-cleanup
 * ============================================================ */

static void test_unload_releases_local_slab(void) {
    /* A trivial guest that immediately calls SYS_EXIT. We're testing
     * the host's reclaim path, not guest behavior. */
    uint8_t code[32];
    size_t pos = 0;
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    size_t baseline = sys.local_slab->total_bytes_in_use;

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);

    /* After load: usage should be above baseline (VmCpu + mailbox +
     * text + data). */
    ASSERT(sys.local_slab->total_bytes_in_use > baseline);

    vm_system_run(&sys, 200);

    /* After unload: should return exactly to baseline. */
    ASSERT(vm_system_unload_vm(&sys, (uint16_t)lr.assigned_vm_id));
    ASSERT_EQ_INT((int)baseline, (int)sys.local_slab->total_bytes_in_use);

    /* Slot should now be free — sys.vms[id] cleared. */
    ASSERT(sys.vms[lr.assigned_vm_id] == NULL);

    vm_system_destroy(&sys);
}

static void test_unload_auto_cleans_leaked_sys_alloc(void) {
    /* Guest:
     *   SYS_ALLOC(64) → ptr in shared region
     *   (deliberately don't free it)
     *   SYS_EXIT
     *
     * After unload, the shared slab should report the block freed. */
    uint8_t code[128];
    size_t pos = 0;
    wr32(code + pos, addi(REG_A0, 0, 64)); pos += 4;
    pos += load_imm32(code + pos, REG_A7, 1056);   /* SYS_ALLOC */
    wr32(code + pos, ecall()); pos += 4;
    /* leak it. Exit without freeing. */
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    size_t shared_baseline = sys.shared_slab->total_bytes_in_use;

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);
    vm_system_run(&sys, 200);

    /* While loaded with the leak: shared slab usage above baseline. */
    ASSERT(sys.shared_slab->total_bytes_in_use > shared_baseline);

    /* Verify the tracking recorded the leak. */
    ASSERT_EQ_INT(1, (int)sys.alloc_tracking[lr.assigned_vm_id].count);

    /* Unload — auto-cleanup should free the leaked block. */
    ASSERT(vm_system_unload_vm(&sys, (uint16_t)lr.assigned_vm_id));
    ASSERT_EQ_INT((int)shared_baseline,
                  (int)sys.shared_slab->total_bytes_in_use);
    /* Tracking cleared. */
    ASSERT_EQ_INT(0, (int)sys.alloc_tracking[lr.assigned_vm_id].count);

    vm_system_destroy(&sys);
}

static void test_unload_concurrent_non_lifo(void) {
    /* Load two VMs, unload them in NON-LIFO order (newer first).
     * The slab makes this trivially correct (any order works);
     * the bump arena couldn't have. */
    uint8_t code[32];
    size_t pos = 0;
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    size_t baseline = sys.local_slab->total_bytes_in_use;

    VmLoadVmResult lr0 = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                            VM_BACKING_COPY_RAM,
                                            VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr0.code);

    VmLoadVmResult lr1 = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                            VM_BACKING_COPY_RAM,
                                            VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr1.code);

    size_t loaded = sys.local_slab->total_bytes_in_use;
    ASSERT(loaded > baseline);

    /* Run both to exit. */
    vm_system_run(&sys, 1000);

    /* Unload OLDER first (non-LIFO) — bump couldn't do this. */
    ASSERT(vm_system_unload_vm(&sys, (uint16_t)lr0.assigned_vm_id));
    /* Half-way: less usage than before, more than baseline. */
    ASSERT(sys.local_slab->total_bytes_in_use < loaded);
    ASSERT(sys.local_slab->total_bytes_in_use > baseline);

    /* Unload the other. */
    ASSERT(vm_system_unload_vm(&sys, (uint16_t)lr1.assigned_vm_id));
    /* All the way back to baseline. */
    ASSERT_EQ_INT((int)baseline, (int)sys.local_slab->total_bytes_in_use);

    vm_system_destroy(&sys);
}

static void test_unload_invalid_args(void) {
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 1,
        .spawn_data_kb = 4,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    /* NULL sys */
    ASSERT(!vm_system_unload_vm(NULL, 0));
    /* Invalid vm_id (out of range) */
    ASSERT(!vm_system_unload_vm(&sys, VM_SCHED_MAX_VMS));
    ASSERT(!vm_system_unload_vm(&sys, VM_SCHED_MAX_VMS + 5));
    /* vm_id in range but not loaded */
    ASSERT(!vm_system_unload_vm(&sys, 0));

    vm_system_destroy(&sys);
}

/* ============================================================
 *  Mailbox info
 * ============================================================ */

static void test_mailbox_info_on_self(void) {
    /* Program: add x0 to whitelist (self-whitelist), SYS_MAILBOX_INFO on self,
     * save result in a2 (so SYS_EXIT's a0 doesn't clobber).
     *
     * We need to first whitelist ourselves so MAILBOX_INFO doesn't return -EPERM.
     */
    uint8_t code[256];
    size_t pos = 0;
    /* SYS_WHITELIST_ADD a0=0 (self) */
    wr32(code + pos, addi(REG_A0, 0, 0)); pos += 4;
    pos += load_imm32(code + pos, REG_A7, 1075);
    wr32(code + pos, ecall()); pos += 4;
    /* SYS_MAILBOX_INFO a0=0 */
    wr32(code + pos, addi(REG_A0, 0, 0)); pos += 4;
    pos += load_imm32(code + pos, REG_A7, 1074);
    wr32(code + pos, ecall()); pos += 4;
    wr32(code + pos, addi(REG_A2, REG_A0, 0)); pos += 4;  /* save slot_size to a2 */
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[512];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);
    vm_system_run(&sys, 200);

    VmCpu *cpu = vm_sched_get(sys.sched, (uint16_t)lr.assigned_vm_id);
    /* a2 should hold slot_size (the default = 32) */
    ASSERT_EQ_INT(32, (int)cpu->regs[REG_A2]);

    vm_system_destroy(&sys);
}

static void test_mailbox_info_without_whitelist_returns_eperm(void) {
    /* MAILBOX_INFO on a VM that hasn't whitelisted us */
    uint8_t code[128];
    size_t pos = 0;
    /* Don't whitelist self — call MAILBOX_INFO directly */
    wr32(code + pos, addi(REG_A0, 0, 0)); pos += 4;
    pos += load_imm32(code + pos, REG_A7, 1074);
    wr32(code + pos, ecall()); pos += 4;
    wr32(code + pos, addi(REG_A2, REG_A0, 0)); pos += 4;
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);
    vm_system_run(&sys, 200);

    VmCpu *cpu = vm_sched_get(sys.sched, (uint16_t)lr.assigned_vm_id);
    /* a2 should be -EPERM = -1 = 0xFFFFFFFF */
    ASSERT_EQ_INT(-1, (int32_t)cpu->regs[REG_A2]);

    vm_system_destroy(&sys);
}

/* ============================================================
 *  Multi-VM: load several VMs, verify they all run
 * ============================================================ */

static void test_three_vms_all_exit(void) {
    /* Three VMs, each just SYS_EXIT */
    uint8_t code[64];
    size_t pos = 0;
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    for (int i = 0; i < 3; i++) {
        VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                               VM_BACKING_COPY_RAM,
                                               VM_BACKING_COPY_RAM);
        ASSERT_EQ_INT(VM_SYS_OK, lr.code);
        ASSERT_EQ_INT(i, lr.assigned_vm_id);
    }

    ASSERT_EQ_INT(3, (int)vm_system_ready_count(&sys));

    bool done = vm_system_run(&sys, 100);
    ASSERT(done);

    /* All VMs halted */
    ASSERT_EQ_INT(0, (int)vm_system_ready_count(&sys));

    vm_system_destroy(&sys);
}

/* ============================================================
 *  Send/recv between two VMs
 *
 *  VM 0: whitelists VM 1, calls SYS_RECV with timeout 0 (non-blocking),
 *  saves result to a2, exits. Expected: -EAGAIN because no message yet.
 *
 *  Actually let me do a more interesting test:
 *
 *  VM 0: whitelists VM 1, polls recv until success, saves payload, exits
 *  VM 1: writes 0x12345678 to its data region, sends it to VM 0, exits
 *
 *  This requires both VMs to make progress, exercising the scheduler's
 *  round-robin and the send/recv handlers.
 *
 *  Simpler version: just verify a single send-then-recv pair via direct
 *  manipulation, since hand-assembling a full poll loop is tedious.
 * ============================================================ */

static void test_send_and_recv_direct(void) {
    /* Exercise the per-VM mailbox API directly (bypasses ECALL).
     * The full ECALL round-trip is tested below in
     * test_send_and_recv_blocking. */
    uint8_t code[64];
    size_t pos = 0;
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    vm_system_load_vm(&sys, elf, elf_size, 4096,
                       VM_BACKING_COPY_RAM, VM_BACKING_COPY_RAM);
    vm_system_load_vm(&sys, elf, elf_size, 4096,
                       VM_BACKING_COPY_RAM, VM_BACKING_COPY_RAM);

    VmMailbox *mbox_a = vm_system_get_mailbox(&sys, 0);
    VmMailbox *mbox_b = vm_system_get_mailbox(&sys, 1);
    ASSERT(mbox_a != NULL);
    ASSERT(mbox_b != NULL);
    ASSERT(mbox_a != mbox_b);

    /* B allows A as a sender */
    vm_mailbox_whitelist_set(mbox_b, 0);

    /* A sends "hello" to B (32 bytes payload — the default slot size) */
    uint8_t payload[32];
    memset(payload, 0xAB, sizeof(payload));
    payload[0] = 'H'; payload[1] = 'i';
    VmMailboxResult r = vm_mailbox_send(mbox_b, /*sender=*/0,
                                          payload, 32);
    ASSERT_EQ_INT(VM_MBOX_OK, r);
    ASSERT_EQ_INT(1, (int)vm_mailbox_count(mbox_b));

    /* B receives it */
    uint8_t received[32];
    uint16_t sender = 0xFFFF;
    r = vm_mailbox_recv(mbox_b, received, &sender);
    ASSERT_EQ_INT(VM_MBOX_OK, r);
    ASSERT_EQ_INT(0, (int)sender);
    ASSERT_EQ_INT('H', received[0]);
    ASSERT_EQ_INT('i', received[1]);
    ASSERT_EQ_INT(0xAB, received[31]);

    vm_system_destroy(&sys);
}

/* ============================================================
 *  End-to-end send/recv through ECALL — synchronous wake
 *
 *  VM 0 (receiver): whitelists VM 1, calls SYS_RECV with timeout,
 *  blocks. On wake, saves the sender id to s0 (x8) and the first
 *  byte of the received payload to s1 (x9). Then exits.
 *
 *  VM 1 (sender): writes a known payload into its data region,
 *  calls SYS_SEND targeting VM 0. The synchronous-delivery path
 *  in handle_send writes the payload directly to VM 0's dest
 *  buffer and unblocks VM 0. Then VM 1 exits.
 * ============================================================ */

static void test_send_and_recv_blocking(void) {
    /* ===== Receiver program (VM 0) ===== */
    /* Layout (data region starts at 0x80000000):
     *   We use vaddr 0x80000000 as our recv dest buffer (top of
     *   region 2, well below the stack which grows down from
     *   region_size). */
    uint8_t recv_code[256];
    size_t pos = 0;
    /* SYS_WHITELIST_ADD: a0=1 (VM 1), a7=1075 */
    wr32(recv_code + pos, addi(REG_A0, 0, 1)); pos += 4;
    pos += load_imm32(recv_code + pos, REG_A7, 1075);
    wr32(recv_code + pos, ecall()); pos += 4;
    /* SYS_RECV: a0 = dest = 0x80000000, a1 = timeout = 10000, a7 = 1073 */
    pos += load_imm32(recv_code + pos, REG_A0, 0x80000000u);
    pos += load_imm32(recv_code + pos, REG_A1, 10000);
    pos += load_imm32(recv_code + pos, REG_A7, 1073);
    wr32(recv_code + pos, ecall()); pos += 4;
    /* Save sender id (a0) into s0 (x8) */
    wr32(recv_code + pos, addi(/*x8*/ 8, REG_A0, 0)); pos += 4;
    /* Load the first byte of dest into s1 (x9): lbu x9, 0(t0)
     * where t0 = 0x80000000. Need to load the constant first. */
    pos += load_imm32(recv_code + pos, /*x5 (t0)*/ 5, 0x80000000u);
    /* lbu x9, 0(x5): opcode 0x03, funct3 0x4 (LBU), rd=9, rs1=5, imm=0 */
    wr32(recv_code + pos,
         (0u << 20) | (5u << 15) | (4u << 12) | (9u << 7) | 0x03u);
    pos += 4;
    /* SYS_EXIT */
    wr32(recv_code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(recv_code + pos, ecall()); pos += 4;

    uint8_t recv_elf[512];
    size_t recv_elf_size = build_code_only_elf(recv_elf, sizeof(recv_elf),
                                                recv_code, pos);
    ASSERT(recv_elf_size > 0);

    /* ===== Sender program (VM 1) ===== */
    uint8_t send_code[256];
    pos = 0;
    /* Write a payload byte (0x5A) into our data region at vaddr 0x80000010.
     * SB x10, 0(x5) where x5 = 0x80000010, x10 = 0x5A. */
    pos += load_imm32(send_code + pos, /*x5*/ 5, 0x80000010u);
    wr32(send_code + pos, addi(/*x10*/ 10, 0, 0x5A)); pos += 4;
    /* sb x10, 0(x5): opcode 0x23, funct3 0x0 (SB) */
    wr32(send_code + pos,
         (0u << 25) | (10u << 20) | (5u << 15) | (0u << 12) | (0u << 7) | 0x23u);
    pos += 4;
    /* SYS_SEND: a0 = target = 0, a1 = payload addr = 0x80000010, a2 = 32 */
    wr32(send_code + pos, addi(REG_A0, 0, 0)); pos += 4;
    pos += load_imm32(send_code + pos, REG_A1, 0x80000010u);
    wr32(send_code + pos, addi(REG_A2, 0, 32)); pos += 4;
    pos += load_imm32(send_code + pos, REG_A7, 1072);
    wr32(send_code + pos, ecall()); pos += 4;
    /* Save send result to s0 */
    wr32(send_code + pos, addi(/*x8*/ 8, REG_A0, 0)); pos += 4;
    /* SYS_EXIT */
    wr32(send_code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(send_code + pos, ecall()); pos += 4;

    uint8_t send_elf[512];
    size_t send_elf_size = build_code_only_elf(send_elf, sizeof(send_elf),
                                                send_code, pos);
    ASSERT(send_elf_size > 0);

    /* ===== Set up system and load both VMs ===== */
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 200,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    VmLoadVmResult lr0 = vm_system_load_vm(&sys, recv_elf, recv_elf_size,
                                            4096, VM_BACKING_COPY_RAM,
                                            VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr0.code);
    ASSERT_EQ_INT(0, lr0.assigned_vm_id);

    VmLoadVmResult lr1 = vm_system_load_vm(&sys, send_elf, send_elf_size,
                                            4096, VM_BACKING_COPY_RAM,
                                            VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr1.code);
    ASSERT_EQ_INT(1, lr1.assigned_vm_id);

    /* Run until everyone halts (or cap). */
    bool done = vm_system_run(&sys, 500);
    ASSERT(done);

    /* ===== Verify results ===== */
    VmCpu *recv_cpu = vm_sched_get(sys.sched, 0);
    VmCpu *send_cpu = vm_sched_get(sys.sched, 1);

    /* Receiver: s0 should hold sender id (= 1). */
    ASSERT_EQ_INT(1, (int)recv_cpu->regs[8]);
    /* Receiver: s1 should hold the first payload byte (0x5A). */
    ASSERT_EQ_INT(0x5A, (int)recv_cpu->regs[9]);
    /* Sender: s0 should hold the send result (0 = success). */
    ASSERT_EQ_INT(0, (int)send_cpu->regs[8]);
}

/* ============================================================
 *  Bump arena bookkeeping
 * ============================================================ */

static void test_local_bytes_used_grows(void) {
    /* Loading VMs should consume bump arena space proportionally. */
    uint8_t code[64];
    size_t pos = 0;
    wr32(code + pos, addi(REG_A7, 0, 93)); pos += 4;
    wr32(code + pos, ecall()); pos += 4;

    uint8_t elf[256];
    size_t elf_size = build_code_only_elf(elf, sizeof(elf), code, pos);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    size_t used0 = vm_system_local_bytes_used(&sys);
    vm_system_load_vm(&sys, elf, elf_size, 4096,
                       VM_BACKING_COPY_RAM, VM_BACKING_COPY_RAM);
    size_t used1 = vm_system_local_bytes_used(&sys);
    ASSERT(used1 > used0);

    vm_system_load_vm(&sys, elf, elf_size, 4096,
                       VM_BACKING_COPY_RAM, VM_BACKING_COPY_RAM);
    size_t used2 = vm_system_local_bytes_used(&sys);
    ASSERT(used2 > used1);

    /* Each load consumes a similar amount (VmCpu + mailbox storage
     * + data region of 4096) */
    size_t per_vm = used1 - used0;
    size_t second_vm = used2 - used1;
    /* Should be the same size for both VMs */
    ASSERT_EQ_INT((int)per_vm, (int)second_vm);

    vm_system_destroy(&sys);
}

/* ============================================================
 *  Timer / clock syscalls
 *
 *  Tests SYS_TICKS_NOW, SYS_TICK_HZ, SYS_SLEEP_TICKS, SYS_SLEEP_UNTIL.
 *  We test the syscall handlers directly (driving cpu state + dispatch)
 *  rather than building a guest ELF; cleaner since these handlers
 *  primarily mutate CPU/scheduler state rather than producing a
 *  computed value that propagates through registers across many
 *  instructions.
 * ============================================================ */

/* Test tick_source: a captured value the host increments manually
 * between steps. */
static uint32_t g_fake_ticks = 0;
static uint32_t fake_tick_source(void *userdata) {
    (void)userdata;
    return g_fake_ticks;
}

static void test_tick_hz_zero_without_source(void) {
    /* No tick_source configured → SYS_TICK_HZ returns 0. */
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[REG_A7] = 1044;  /* SYS_TICK_HZ */
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    ASSERT_EQ_INT(0, (int)cpu.regs[REG_A0]);

    vm_system_destroy(&sys);
}

static void test_tick_hz_returns_configured_value(void) {
    g_fake_ticks = 0;
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .tick_source = fake_tick_source,
        .ticks_per_second = 1000,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[REG_A7] = 1044;  /* SYS_TICK_HZ */
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    ASSERT_EQ_INT(1000, (int)cpu.regs[REG_A0]);

    vm_system_destroy(&sys);
}

static void test_ticks_now_reads_scheduler_tick(void) {
    g_fake_ticks = 0;
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .tick_source = fake_tick_source,
        .ticks_per_second = 1000,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    /* Seed scheduler's global_tick (would normally be set on next
     * scheduler step). For this direct-handler test we set it
     * explicitly. */
    sys.sched->global_tick = 12345;

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[REG_A7] = 1043;  /* SYS_TICKS_NOW */
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    ASSERT_EQ_INT(12345, (int)cpu.regs[REG_A0]);

    vm_system_destroy(&sys);
}

static void test_sleep_ticks_sets_block_sleep(void) {
    g_fake_ticks = 100;
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .tick_source = fake_tick_source,
        .ticks_per_second = 1000,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    sys.sched->global_tick = 100;

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[REG_A0] = 50;
    cpu.regs[REG_A7] = 1045;  /* SYS_SLEEP_TICKS */
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    /* Should be set to BLOCK_SLEEP with deadline = now + 50. */
    ASSERT_EQ_INT((int)BLOCK_SLEEP, (int)cpu.block_reason);
    ASSERT_EQ_INT(150, (int)cpu.block_deadline);

    vm_system_destroy(&sys);
}

static void test_sleep_ticks_zero_yields(void) {
    g_fake_ticks = 100;
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .tick_source = fake_tick_source,
        .ticks_per_second = 1000,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    sys.sched->global_tick = 100;

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[REG_A0] = 0;
    cpu.regs[REG_A7] = 1045;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    /* n=0 is YIELD. */
    ASSERT_EQ_INT((int)BLOCK_YIELDED, (int)cpu.block_reason);
    ASSERT_EQ_INT(0, (int)cpu.regs[REG_A0]);

    vm_system_destroy(&sys);
}

static void test_sleep_until_future_blocks(void) {
    g_fake_ticks = 100;
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .tick_source = fake_tick_source,
        .ticks_per_second = 1000,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    sys.sched->global_tick = 100;

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[REG_A0] = 200;  /* deadline 100 ticks ahead */
    cpu.regs[REG_A7] = 1046;  /* SYS_SLEEP_UNTIL */
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    ASSERT_EQ_INT((int)BLOCK_SLEEP, (int)cpu.block_reason);
    ASSERT_EQ_INT(200, (int)cpu.block_deadline);

    vm_system_destroy(&sys);
}

static void test_sleep_until_past_yields(void) {
    g_fake_ticks = 100;
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .tick_source = fake_tick_source,
        .ticks_per_second = 1000,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    sys.sched->global_tick = 100;

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[REG_A0] = 50;  /* deadline already past */
    cpu.regs[REG_A7] = 1046;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    /* Past deadline → yield instead of indefinite block. */
    ASSERT_EQ_INT((int)BLOCK_YIELDED, (int)cpu.block_reason);
    ASSERT_EQ_INT(0, (int)cpu.regs[REG_A0]);

    vm_system_destroy(&sys);
}

static void test_sleep_until_wraparound_safe(void) {
    /* If global_tick is near UINT32_MAX and deadline is just past
     * the wrap (e.g., 5), we should treat the deadline as in the
     * future, not the past. Tested via the signed-subtract idiom. */
    g_fake_ticks = 0xFFFFFFF0;
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .tick_source = fake_tick_source,
        .ticks_per_second = 1000,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    sys.sched->global_tick = 0xFFFFFFF0;

    VmCpu cpu;
    vm_init(&cpu, 0);
    cpu.regs[REG_A0] = 5;  /* deadline = 5 = "0xFFFFFFF0 + 21" mod 2^32 */
    cpu.regs[REG_A7] = 1046;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    /* Should treat as 21 ticks in the future, not "way in the past." */
    ASSERT_EQ_INT((int)BLOCK_SLEEP, (int)cpu.block_reason);
    ASSERT_EQ_INT(5, (int)cpu.block_deadline);

    vm_system_destroy(&sys);
}

/* ============================================================
 *  Auto-reload periodic timer
 * ============================================================ */

/* Helper: stand up a sys + cpu at a known tick. */
static void reload_setup(VmSystem *sys, VmCpu *cpu, uint32_t at_tick) {
    g_fake_ticks = at_tick;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 4,
        .tick_source = fake_tick_source,
        .ticks_per_second = 1000,
    };
    ASSERT(vm_system_init(sys, &cfg));
    sys->sched->global_tick = at_tick;
    vm_init(cpu, 0);
}

static void test_set_reload_period_anchors_deadline(void) {
    /* set_reload_period(p) should anchor reload_next_deadline at
     * now + p so the first yield_until_reload sleeps one period. */
    VmSystem sys;
    VmCpu cpu;
    reload_setup(&sys, &cpu, 100);

    cpu.regs[REG_A0] = 125;             /* period = 125 ticks */
    cpu.regs[REG_A7] = 1047;             /* SYS_SET_RELOAD_PERIOD */
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    ASSERT_EQ_INT(125, (int)cpu.reload_period);
    ASSERT_EQ_INT(225, (int)cpu.reload_next_deadline);  /* 100 + 125 */
    ASSERT_EQ_INT(0, (int)cpu.regs[REG_A0]);

    vm_system_destroy(&sys);
}

static void test_set_reload_period_zero_clears(void) {
    VmSystem sys;
    VmCpu cpu;
    reload_setup(&sys, &cpu, 100);

    /* First set a real period... */
    cpu.regs[REG_A0] = 125;
    cpu.regs[REG_A7] = 1047;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    ASSERT_EQ_INT(125, (int)cpu.reload_period);

    /* ...then clear it. */
    cpu.regs[REG_A0] = 0;
    cpu.regs[REG_A7] = 1047;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    ASSERT_EQ_INT(0, (int)cpu.reload_period);
    ASSERT_EQ_INT(0, (int)cpu.reload_next_deadline);

    vm_system_destroy(&sys);
}

static void test_yield_until_reload_blocks_until_first_deadline(void) {
    /* First yield after set_reload_period: deadline is in the
     * future, so BLOCK_SLEEP until that tick. Next deadline is
     * advanced by one period for the subsequent yield. */
    VmSystem sys;
    VmCpu cpu;
    reload_setup(&sys, &cpu, 100);

    /* set_reload_period(125) → next_deadline = 225 */
    cpu.regs[REG_A0] = 125;
    cpu.regs[REG_A7] = 1047;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    /* yield_until_reload — still at tick 100, deadline 225 is
     * in the future → block until 225, advance next to 350. */
    cpu.regs[REG_A7] = 1048;             /* SYS_YIELD_UNTIL_RELOAD */
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    ASSERT_EQ_INT((int)BLOCK_SLEEP, (int)cpu.block_reason);
    ASSERT_EQ_INT(225, (int)cpu.block_deadline);
    ASSERT_EQ_INT(350, (int)cpu.reload_next_deadline);

    vm_system_destroy(&sys);
}

static void test_yield_until_reload_subsequent_cycles(void) {
    /* Simulate a sequence of yields where the guest is on time
     * each frame. Each yield should target the next boundary
     * exactly. */
    VmSystem sys;
    VmCpu cpu;
    reload_setup(&sys, &cpu, 100);

    cpu.regs[REG_A0] = 50;               /* period = 50 */
    cpu.regs[REG_A7] = 1047;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    /* next_deadline now = 150 */

    /* Yield 1: tick=100, sleep until 150, next=200 */
    cpu.regs[REG_A7] = 1048;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    ASSERT_EQ_INT(150, (int)cpu.block_deadline);
    ASSERT_EQ_INT(200, (int)cpu.reload_next_deadline);

    /* Advance to tick 150 (the deadline we just hit) and reset
     * block state as if the scheduler woke us. */
    cpu.block_reason = BLOCK_NONE;
    cpu.block_deadline = 0;
    sys.sched->global_tick = 150;

    /* Yield 2: tick=150, sleep until 200, next=250 */
    cpu.regs[REG_A7] = 1048;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    ASSERT_EQ_INT(200, (int)cpu.block_deadline);
    ASSERT_EQ_INT(250, (int)cpu.reload_next_deadline);

    /* Yield 3: tick=200, sleep until 250, next=300 */
    cpu.block_reason = BLOCK_NONE;
    sys.sched->global_tick = 200;
    cpu.regs[REG_A7] = 1048;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    ASSERT_EQ_INT(250, (int)cpu.block_deadline);
    ASSERT_EQ_INT(300, (int)cpu.reload_next_deadline);

    vm_system_destroy(&sys);
}

static void test_yield_until_reload_catchup_skips_to_future(void) {
    /* If the guest is multiple periods past the deadline, the
     * kernel skips ahead to the next FUTURE boundary and blocks
     * until it. Phase preserved on the original tick grid;
     * missed frames dropped cleanly (no rapid-fire catch-up).
     *
     * Earlier versions of this handler issued BLOCK_YIELDED in
     * the "already past" case, which let the guest fire one
     * catch-up frame back-to-back. That produced visible stutter
     * on hosts where the OS scheduler is jittery: every late
     * frame was followed by a too-fast one. The current
     * implementation always BLOCK_SLEEPs until the next future
     * boundary — same phase preservation, no visible burst. */
    VmSystem sys;
    VmCpu cpu;
    reload_setup(&sys, &cpu, 0);

    cpu.regs[REG_A0] = 100;              /* period = 100 */
    cpu.regs[REG_A7] = 1047;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    /* next_deadline = 100 */

    /* Pretend the guest got hung up: we're now at tick 350,
     * way past the deadline of 100. Call yield. */
    sys.sched->global_tick = 350;

    cpu.regs[REG_A7] = 1048;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    /* Sleep until the next future boundary: 100, 200, 300, 400
     * — first future is 400. reload_next_deadline advances by
     * one more period (500) so the call AFTER this targets it. */
    ASSERT_EQ_INT((int)BLOCK_SLEEP, (int)cpu.block_reason);
    ASSERT_EQ_INT(400, (int)cpu.block_deadline);
    ASSERT_EQ_INT(500, (int)cpu.reload_next_deadline);

    vm_system_destroy(&sys);
}

static void test_yield_until_reload_small_overshoot_still_sleeps(void) {
    /* Regression test for the 'goes fast' bug: when the host
     * oversleeps by even a small amount (here 5 ticks past the
     * 100-tick period), the kernel must still BLOCK_SLEEP until
     * the next future boundary — NOT BLOCK_YIELDED, which would
     * cause the guest to rapid-fire one extra frame and the
     * user to perceive a stutter. */
    VmSystem sys;
    VmCpu cpu;
    reload_setup(&sys, &cpu, 0);

    cpu.regs[REG_A0] = 100;              /* period = 100 */
    cpu.regs[REG_A7] = 1047;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    /* reload_next_deadline = 100 */

    /* Host slept 105 ticks instead of 100 — 5 ticks of slack. */
    sys.sched->global_tick = 105;

    cpu.regs[REG_A7] = 1048;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    /* Should sleep until 200, NOT yield. The 5-tick slack is
     * absorbed into the next frame's deadline, which lands at
     * the original-grid 200. */
    ASSERT_EQ_INT((int)BLOCK_SLEEP, (int)cpu.block_reason);
    ASSERT_EQ_INT(200, (int)cpu.block_deadline);
    ASSERT_EQ_INT(300, (int)cpu.reload_next_deadline);

    vm_system_destroy(&sys);
}

static void test_yield_until_reload_without_period_yields(void) {
    /* Calling yield_until_reload before set_reload_period
     * should NOT block forever (which would be the result of a
     * naive "wait until tick 0" interpretation). Instead treat
     * it as a plain yield. */
    VmSystem sys;
    VmCpu cpu;
    reload_setup(&sys, &cpu, 100);

    /* No set_reload_period was called → reload_period is 0. */
    ASSERT_EQ_INT(0, (int)cpu.reload_period);

    cpu.regs[REG_A7] = 1048;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    ASSERT_EQ_INT((int)BLOCK_YIELDED, (int)cpu.block_reason);
    ASSERT_EQ_INT(0, (int)cpu.regs[REG_A0]);

    vm_system_destroy(&sys);
}

static void test_yield_until_reload_independent_of_sleep_until(void) {
    /* SYS_SLEEP_UNTIL should NOT touch the reload state, and
     * vice versa. A guest mixing the two should see them as
     * independent timers. */
    VmSystem sys;
    VmCpu cpu;
    reload_setup(&sys, &cpu, 100);

    /* Set up reload state */
    cpu.regs[REG_A0] = 50;
    cpu.regs[REG_A7] = 1047;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);
    /* reload_next_deadline = 150 */

    /* Now call SYS_SLEEP_UNTIL for a totally unrelated deadline */
    cpu.regs[REG_A0] = 999;
    cpu.regs[REG_A7] = 1046;
    vm_ecall_dispatch(sys.ecall_router, &cpu, &sys);

    /* sleep_until set block state — that's expected */
    ASSERT_EQ_INT((int)BLOCK_SLEEP, (int)cpu.block_reason);
    ASSERT_EQ_INT(999, (int)cpu.block_deadline);
    /* But reload state untouched */
    ASSERT_EQ_INT(50, (int)cpu.reload_period);
    ASSERT_EQ_INT(150, (int)cpu.reload_next_deadline);

    vm_system_destroy(&sys);
}



/* ============================================================
 *  Test runner
 * ============================================================ */

int main(void) {
    TEST_SUITE("vm_system");

    /* Lifecycle */
    RUN(test_init_succeeds);
    RUN(test_init_rejects_missing_storage);
    RUN(test_init_rejects_null_args);

    /* Loading + running VMs */
    RUN(test_load_and_run_exit_vm);
    RUN(test_load_and_run_self_vm);

    /* Alloc/free */
    RUN(test_alloc_and_free);
    RUN(test_alloc_zero_returns_einval);

    /* Unload / auto-cleanup */
    RUN(test_unload_releases_local_slab);
    RUN(test_unload_auto_cleans_leaked_sys_alloc);
    RUN(test_unload_concurrent_non_lifo);
    RUN(test_unload_invalid_args);

    /* Mailbox info */
    RUN(test_mailbox_info_on_self);
    RUN(test_mailbox_info_without_whitelist_returns_eperm);

    /* Multi-VM */
    RUN(test_three_vms_all_exit);

    /* Send/recv */
    RUN(test_send_and_recv_direct);
    RUN(test_send_and_recv_blocking);

    /* Arena */
    RUN(test_local_bytes_used_grows);

    /* Timer / clock syscalls */
    RUN(test_tick_hz_zero_without_source);
    RUN(test_tick_hz_returns_configured_value);
    RUN(test_ticks_now_reads_scheduler_tick);
    RUN(test_sleep_ticks_sets_block_sleep);
    RUN(test_sleep_ticks_zero_yields);
    RUN(test_sleep_until_future_blocks);
    RUN(test_sleep_until_past_yields);
    RUN(test_sleep_until_wraparound_safe);

    /* Auto-reload periodic timer */
    RUN(test_set_reload_period_anchors_deadline);
    RUN(test_set_reload_period_zero_clears);
    RUN(test_yield_until_reload_blocks_until_first_deadline);
    RUN(test_yield_until_reload_subsequent_cycles);
    RUN(test_yield_until_reload_catchup_skips_to_future);
    RUN(test_yield_until_reload_small_overshoot_still_sleeps);
    RUN(test_yield_until_reload_without_period_yields);
    RUN(test_yield_until_reload_independent_of_sleep_until);

    /* Suppress unused warnings */
    (void)sw_; (void)lw_;

    return TEST_SUITE_RESULT();
}
