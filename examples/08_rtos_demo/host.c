/* 08_rtos_demo/host.c — several RV32 guests running CONCURRENTLY as
 * preemptive tasks.
 *
 * Extends 07_vm_task (one VM + one native task) to N guest VMs: each
 * guest ELF is loaded into its own VmCpu and added to the preemptive
 * scheduler as a task. The 1 ms systick time-slices them, so their
 * output interleaves — an "RTOS" running multiple real VMs at once.
 *
 * Each VM owns its VmCpu (never shared across threads), and its ecalls
 * are handled in that VM's own task thread — exactly the model from
 * docs/execution-model.md §7.2 ("a VM task and a native task are
 * identical to the scheduler"). This host stays tiny by routing only
 * the two ecalls the demo guests use: SYS_WRITE (their puts) and
 * SYS_EXIT.
 *
 * Build:  ./build.sh run        Link: -lpthread (+ -lrt on Linux)
 *
 * Public domain (CC0). No warranty.
 */
#include "vm/presched.h"
#include "vm/vm_core.h"
#include "vm/vm_loader.h"
#include "memory/slab_stack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>

#define SYS_WRITE 64
#define SYS_EXIT  93

#define MAX_GUESTS 4
static uint8_t g_elf[MAX_GUESTS][64 * 1024];
static VmCpu   g_cpu[MAX_GUESTS];

/* One guest VM as a preemptive task: loop vm_step, route its ecalls in
 * this thread. vm_step has already advanced PC past the ecall, so we set
 * the return register and resume. */
static void vm_task(void *arg) {
    VmCpu *cpu = (VmCpu *)arg;
    for (;;) {
        uint32_t steps = 0;
        VmStepResult r = vm_step(cpu, 50000, &steps);
        if (r == VM_STEP_ECALL) {
            uint32_t a7 = cpu->regs[17];
            if (a7 == SYS_WRITE) {
                uint32_t buf = cpu->regs[11], n = cpu->regs[12];
                const void *p = vm_translate_read(cpu, buf, n);
                if (p) { fwrite(p, 1, n, stdout); fflush(stdout); }
                cpu->regs[10] = n;          /* bytes written */
                continue;                   /* resume after the ecall */
            }
            if (a7 == SYS_EXIT) {
                cpu->halted = true;
                return;
            }
            cpu->regs[10] = 0;              /* unused ecall: ignore, resume */
            continue;
        }
        if (r == VM_STEP_HALTED || r == VM_STEP_TRAPPED) return;
        /* otherwise (quantum expired) keep running */
    }
}

int main(int argc, char **argv) {
    const char *def[] = { "guest_a.elf", "guest_b.elf", "guest_c.elf" };
    int ng = (argc > 1) ? (argc - 1) : 3;
    if (ng > MAX_GUESTS) ng = MAX_GUESTS;

    static uint8_t region[4 * 1024 * 1024];
    SlabConfig cfg; memset(&cfg, 0, sizeof cfg);
    for (int i = 0; i < 12; i++) cfg.bucket_counts[i] = 16;
    SlabAllocator slab;
    slab_init(&slab, region, sizeof region, &cfg, slab_null_locker);

    PreSched *s = presched_create(1000);   /* 1 ms systick */

    for (int i = 0; i < ng; i++) {
        const char *path = (argc > 1) ? argv[i + 1] : def[i];
        FILE *f = fopen(path, "rb");
        if (!f) { printf("rtos_demo: cannot open %s\n", path); return 1; }
        size_t n = fread(g_elf[i], 1, sizeof g_elf[i], f);
        fclose(f);
        vm_init(&g_cpu[i], (uint16_t)i);
        VmLoaderConfig spec; memset(&spec, 0, sizeof spec);
        spec.code_backing   = VM_BACKING_COPY_RAM;
        spec.rodata_backing = VM_BACKING_COPY_RAM;
        spec.ram_arena      = &slab;
        spec.region_data_size = 32 * 1024;
        if (vm_load(&g_cpu[i], g_elf[i], n, &spec) != VM_LOAD_OK) {
            printf("rtos_demo: load %s failed\n", path); return 1;
        }
        presched_add_task(s, vm_task, &g_cpu[i]);
    }

    printf("=== %d guest VMs, preemptively time-sliced (1 ms systick) ===\n", ng);
    presched_run(s);
    printf("=== all guests exited; context switches=%llu ===\n",
           (unsigned long long)presched_switches(s));
    presched_destroy(s);
    return 0;
}
