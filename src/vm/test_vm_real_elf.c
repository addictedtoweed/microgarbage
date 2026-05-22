/* Integration test: load and run a real ELF produced by
 * riscv64-unknown-elf-gcc with -march=rv32imc.
 *
 * The ELF is loaded as binary data at build time via objcopy
 * (the .elf is built by examples/01_hello/build.sh). For this test we read it
 * from disk at run time, since the test build doesn't have
 * a Makefile yet — we just keep the .elf checked in.
 *
 * The hello.elf is:
 *   li   a0, 0
 *   li   a7, 93        ; SYS_EXIT
 *   ecall
 *
 * Expected behavior: load succeeds, run terminates with VM
 * halted, all instructions retired correctly. */

#include "test_runner.h"
#include "vm/vm_system.h"

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define SHARED_BYTES  (64 * 1024)
#define LOCAL_BYTES   (256 * 1024)

static uint8_t g_shared_storage[SHARED_BYTES];
static uint8_t g_local_storage[LOCAL_BYTES];

static int load_file(const char *path, uint8_t **out_buf, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) { fclose(f); return -1; }

    uint8_t *buf = (uint8_t *)malloc((size_t)size);
    if (!buf) { fclose(f); return -1; }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) { free(buf); return -1; }

    *out_buf = buf;
    *out_size = (size_t)size;
    return 0;
}

static void test_hello_elf_runs_and_exits(void) {
    /* Path is relative to where the test binary is run from. */
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file("examples/01_hello/build/guest_minimal.elf", &elf, &elf_size) != 0) {
        /* Couldn't open the ELF — skip with a clear failure message. */
        FAIL("could not open examples/01_hello/build/guest_minimal.elf "
             "(run test from repo root)");
        return;
    }

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 16,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size,
                                           /*data_region=*/4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        printf("    [load failed: code=%d, load_result=%d]\n",
               lr.code, lr.load_result);
    }
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);

    /* Run until halt. The program is 3 instructions: li, li, ecall.
     * SYS_EXIT halts the VM. */
    bool done = vm_system_run(&sys, 100);
    ASSERT(done);

    VmCpu *cpu = vm_sched_get(sys.sched, (uint16_t)lr.assigned_vm_id);
    ASSERT(cpu != NULL);
    ASSERT(cpu->halted);
    /* Two ALU instructions retired before the ECALL. ECALL itself
     * doesn't count as retired (it traps out of the dispatch loop). */
    ASSERT_EQ_INT(2, (int)cpu->instructions_retired);
    /* a7 should hold 93 (SYS_EXIT) — the guest set it just before
     * the ecall. */
    ASSERT_EQ_INT(93, (int)cpu->regs[VM_REG_A7]);
    /* a0 was 0 (the exit code) when ecall was issued. SYS_EXIT
     * handler doesn't write a0 (only sets halted), so it remains 0. */
    ASSERT_EQ_INT(0, (int)cpu->regs[VM_REG_A0]);

    vm_system_destroy(&sys);
    free(elf);
}

static void test_hello2_factorial_runs(void) {
    /* Loads hello2.elf — a program that calls factorial(5) (which
     * the compiler constant-folds to 120) and exits with that
     * value. Exercises:
     *   - non-zero entry point (entry = 0x6, not 0x0)
     *   - function call via jal (uses ra register)
     *   - stack push/pop (uses sp)
     *
     * The compiler's optimizer reduced factorial(5) to a constant,
     * but the call/return sequence and stack frame are still
     * emitted, which is what we want to exercise. */
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file("examples/01_hello/build/guest_factorial.elf", &elf, &elf_size) != 0) {
        FAIL("could not open examples/01_hello/build/guest_factorial.elf");
        return;
    }

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 16,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size,
                                           /*data_region=*/4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        printf("    [load failed: code=%d, load_result=%d]\n",
               lr.code, lr.load_result);
    }
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);

    bool done = vm_system_run(&sys, 200);
    ASSERT(done);

    VmCpu *cpu = vm_sched_get(sys.sched, (uint16_t)lr.assigned_vm_id);
    ASSERT(cpu != NULL);
    ASSERT(cpu->halted);
    /* a0 = 120 = factorial(5) — the exit code we passed.
     * SYS_EXIT doesn't write a0, so it holds the value the guest
     * set just before the ecall. */
    ASSERT_EQ_INT(120, (int)cpu->regs[VM_REG_A0]);

    vm_system_destroy(&sys);
    free(elf);
}

static void test_hello3_sum_of_squares_runs(void) {
    /* hello3.elf computes sum of squares 1..10 = 385 in a real
     * loop (volatile prevents constant-folding), exercising:
     *   - RV32M's mul instruction
     *   - Branches, loads/stores, compressed jumps
     *   - sp-relative load/store with non-trivial frame */
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file("examples/01_hello/build/guest_squares.elf", &elf, &elf_size) != 0) {
        FAIL("could not open examples/01_hello/build/guest_squares.elf");
        return;
    }

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 16,
        .baseline_quantum = 1000,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size,
                                           /*data_region=*/4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        printf("    [load failed: code=%d, load_result=%d]\n",
               lr.code, lr.load_result);
    }
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);

    bool done = vm_system_run(&sys, 500);
    ASSERT(done);

    VmCpu *cpu = vm_sched_get(sys.sched, (uint16_t)lr.assigned_vm_id);
    ASSERT(cpu != NULL);
    ASSERT(cpu->halted);
    /* a0 = 385 = sum of i*i for i=1..10 */
    ASSERT_EQ_INT(385, (int)cpu->regs[VM_REG_A0]);

    vm_system_destroy(&sys);
    free(elf);
}

/* Round V.6 lifecycle: the multi-session TCP host detects that a
 * session ended — and thus that its port can accept a reconnection —
 * by checking whether the shell's VM is still registered:
 * vm_sched_get(sched, vm_id) == NULL means it exited.
 *
 * That only works if stepping the system to halt actually UNLOADS
 * the halted VM (vs leaving it parked as halted). vm_system_step
 * runs the reap, which unloads any halted VM. This test pins that
 * contract: after stepping a top-level VM to exit, vm_sched_get
 * returns NULL for its id. If this regressed, reconnection would
 * silently break (the slot would never reopen). */
static void test_stepped_halt_unloads_vm(void) {
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file("examples/01_hello/build/guest_minimal.elf",
                  &elf, &elf_size) != 0) {
        FAIL("could not open guest_minimal.elf (run from repo root)");
        return;
    }

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage = g_shared_storage,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local_storage,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 16,
        .baseline_quantum = 100,
    };
    ASSERT(vm_system_init(&sys, &cfg));

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    ASSERT_EQ_INT(VM_SYS_OK, lr.code);
    uint16_t id = (uint16_t)lr.assigned_vm_id;

    /* Right after load it's registered. */
    ASSERT(vm_sched_get(sys.sched, id) != NULL);

    /* Step like the host loop does, until the reap unloads it (or a
     * sane cap). guest_minimal is li/li/ecall(SYS_EXIT). */
    int guard = 1000;
    while (vm_sched_get(sys.sched, id) != NULL && guard-- > 0) {
        vm_system_step(&sys);
    }
    ASSERT(guard > 0);   /* it did get unloaded */

    /* THE CONTRACT: a halted top-level VM is gone from the scheduler.
     * This is exactly the signal the host uses to reopen the port. */
    ASSERT(vm_sched_get(sys.sched, id) == NULL);

    vm_system_destroy(&sys);
    free(elf);
}

int main(void) {
    TEST_SUITE("vm_real_elf");

    RUN(test_hello_elf_runs_and_exits);
    RUN(test_hello2_factorial_runs);
    RUN(test_hello3_sum_of_squares_runs);
    RUN(test_stepped_halt_unloads_vm);

    return TEST_SUITE_RESULT();
}
