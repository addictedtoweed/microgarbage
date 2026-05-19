/* 01_hello/host.c — minimal embedding host for a single guest.
 *
 * Demonstrates the absolute minimum amount of host code to:
 *   1. Initialize a VmSystem with shared and local memory pools
 *   2. Install the host stdio bridge so the guest's SYS_WRITE
 *      reaches our stdout
 *   3. Load a guest ELF from disk
 *   4. Run the scheduler until the guest exits
 *
 * After this example, look at 02_counter for guests that yield
 * cooperatively and 03_mailbox for multi-guest IPC.
 */

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* ---------------------------------------------------------------
 * Memory pools.
 *
 * The VmSystem doesn't malloc — the host owns all storage and
 * hands it in via the config struct. Sizes here are roomy enough
 * for a few small guests; production code would size to fit.
 * --------------------------------------------------------------- */

#define SHARED_BYTES  (64 * 1024)   /* SYS_ALLOC backs into this  */
#define LOCAL_BYTES   (64 * 1024)   /* VmCpu structs, code, data  */

static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];

/* ---------------------------------------------------------------
 * File loader.
 *
 * The VM loader expects the ELF as a contiguous byte buffer in
 * host memory. For demo purposes we just slurp the whole file.
 * --------------------------------------------------------------- */

static int load_file(const char *path, uint8_t **out_buf, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "host: cannot open '%s'\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return -1; }
    *out_buf = buf; *out_size = (size_t)sz;
    return 0;
}

int main(int argc, char **argv) {
    const char *elf_path = (argc >= 2) ? argv[1] : "build/guest.elf";

    /* 1. Load the guest ELF from disk. */
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file(elf_path, &elf, &elf_size) != 0) {
        return 1;
    }

    /* 2. Initialize the system. Most defaults are fine; we only
     *    need to point it at our memory pools. */
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage      = g_shared,
        .shared_storage_size = SHARED_BYTES,
        .local_storage       = g_local,
        .local_storage_size  = LOCAL_BYTES,
    };
    if (!vm_system_init(&sys, &cfg)) {
        fprintf(stderr, "host: vm_system_init failed\n");
        return 1;
    }

    /* 3. Install the optional stdio bridge so guest SYS_WRITE
     *    reaches the terminal. Without this, SYS_WRITE returns
     *    -ENOSYS from the default fallback. */
    if (!vm_host_install_stdio(&sys)) {
        fprintf(stderr, "host: vm_host_install_stdio failed\n");
        return 1;
    }

    /* 4. Load the guest. */
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size,
                                           /*data_region=*/ 4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        fprintf(stderr, "host: load failed (code=%d, load_result=%d)\n",
                lr.code, lr.load_result);
        return 1;
    }

    /* 5. Run until the guest exits. vm_system_run returns true
     *    when all VMs have halted. The cycle cap is just a
     *    safety net; this guest finishes in a few dozen
     *    instructions. */
    bool done = vm_system_run(&sys, 10000);
    if (!done) {
        fprintf(stderr, "host: cycle cap hit (guest still running)\n");
    }

    vm_system_destroy(&sys);
    free(elf);
    return 0;
}
