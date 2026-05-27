/* 07_vm_task/host.c — a real RV32 VM running as a PREEMPTIVE task.
 *
 * This is Step 4 of the preemptive scheduler (docs/execution-model.md
 * §7.9): the moment the VM and the scheduler meet. A VmCpu is driven by
 * a task body (vm_task) that loops vm_step; the scheduler preempts it
 * mid-execution via the systick, exactly as it preempts a native task.
 * To the scheduler a VM task and a native task are identical — both are
 * "a thread the systick preempts" (§7.2). We run one of each and show
 * they interleave, and that the VM still computes its correct result
 * (exit code 64) despite being preempted.
 *
 * The VmCpu is owned by exactly one task thread (never shared) — the
 * single-thread-ownership invariant the VM core requires.
 *
 * Build:  ./build.sh run     Link: -lrt -lpthread (POSIX)
 *
 * Public domain (CC0). No warranty.
 */

#include "vm/presched.h"
#include "vm/vm_core.h"
#include "vm/vm_loader.h"
#include "memory/slab_stack.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

static uint8_t g_elf[65536];
static atomic_int g_vm_exit = -1;
static atomic_long g_native_progress = 0;
static atomic_int g_vm_done = 0, g_native_done = 0;

/* The VM task body: drives a VmCpu via vm_step in a loop. Preemption is
 * external (the systick), so the budget per vm_step is just "a chunk" —
 * large enough to be efficient, small enough to handle ecalls promptly.
 * This is the unrolled/budget-free spirit of §7.1: the loop runs freely
 * and the systick preempts the thread mid-vm_step. */
static void vm_task(void *arg) {
    VmCpu *cpu = (VmCpu *)arg;
    for (;;) {
        uint32_t steps = 0;
        VmStepResult r = vm_step(cpu, 50000, &steps);
        if (r == VM_STEP_HALTED) { atomic_store(&g_vm_done, 1); return; }
        if (r == VM_STEP_TRAPPED) {
            printf("  [vm] trapped cause=%d\n", (int)cpu->trap_cause);
            atomic_store(&g_vm_done, 1); return;
        }
        if (r == VM_STEP_ECALL) {
            uint32_t a7 = cpu->regs[17], a0 = cpu->regs[10];
            if (a7 == 93) {   /* SYS_EXIT */
                atomic_store(&g_vm_exit, (int)a0);
                cpu->halted = true;
                atomic_store(&g_vm_done, 1);
                printf("  [vm] guest exited with code %d\n", (int)a0);
                return;
            }
            /* other ecalls would be routed here, in THIS task's thread */
        }
        /* QUANTUM_EXPIRED: keep going */
    }
}

/* A native task running concurrently, to prove the VM task interleaves
 * with non-VM tasks under preemption. */
static void native_task(void *arg) {
    (void)arg;
    for (int c = 0; c < 5; c++) {
        volatile unsigned long x = 0;
        for (unsigned long i = 0; i < 30000000UL; i++) x += i;
        atomic_fetch_add(&g_native_progress, 1);
        printf("  [native] tick %d/5\n", c + 1);
    }
    atomic_store(&g_native_done, 1);
}

int main(int argc, char **argv) {
    FILE *f = fopen(argc > 1 ? argv[1] : "guest.elf", "rb");
    size_t n = fread(g_elf, 1, sizeof g_elf, f); fclose(f);

    static uint8_t region[2*1024*1024];
    SlabConfig cfg; memset(&cfg, 0, sizeof cfg);
    for (int i = 0; i < 12; i++) cfg.bucket_counts[i] = 8;
    SlabAllocator slab; slab_init(&slab, region, sizeof region, &cfg, slab_null_locker);

    static VmCpu cpu; vm_init(&cpu, 0);
    VmLoaderConfig spec; memset(&spec, 0, sizeof spec);
    spec.code_backing = VM_BACKING_COPY_RAM; spec.rodata_backing = VM_BACKING_COPY_RAM;
    spec.ram_arena = &slab; spec.region_data_size = 32*1024;
    if (vm_load(&cpu, g_elf, n, &spec) != VM_LOAD_OK) { printf("load fail\n"); return 1; }

    printf("Running a VM task + a native task under the preemptive scheduler:\n");
    PreSched *s = presched_create(1000);   /* 1ms systick */
    presched_add_task(s, vm_task, &cpu);        /* the VM, as a task */
    presched_add_task(s, native_task, NULL);    /* a native task */
    presched_run(s);

    int vm_exit = atomic_load(&g_vm_exit);
    long native = atomic_load(&g_native_progress);
    printf("\nResult: vm_exit=%d (expect 64), native_progress=%ld (expect 5)\n", vm_exit, native);
    printf("switches=%llu ticks=%llu\n",
           (unsigned long long)presched_switches(s),
           (unsigned long long)presched_total_ticks(s));
    int ok = (vm_exit == 64 && native == 5 && atomic_load(&g_vm_done) && atomic_load(&g_native_done));
    printf("VM-as-preemptive-task: %s\n", ok ? "WORKS" : "FAILED");
    presched_destroy(s);
    return ok ? 0 : 1;
}
