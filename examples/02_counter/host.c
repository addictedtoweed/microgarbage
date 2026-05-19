/* 02_counter/host.c — runs a counter guest that prints forever.
 *
 * Differences from 01_hello:
 *   - SIGINT handler so Ctrl-C cleanly stops the scheduler instead
 *     of leaving the terminal in an awkward state
 *   - Cycle cap is much higher (the guest never exits on its own)
 *   - vm_system_step in a loop rather than vm_system_run, so we
 *     can check the SIGINT flag between scheduling decisions
 */

/* For sigaction. */
#define _POSIX_C_SOURCE 200809L

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>

#define SHARED_BYTES  (64 * 1024)
#define LOCAL_BYTES   (64 * 1024)

static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];

/* SIGINT-on-Ctrl-C handler. We just flip a flag; the main loop
 * checks it between scheduler steps. Doing real work in a signal
 * handler is risky (only async-signal-safe functions are
 * guaranteed safe), so the flag pattern is standard. */
static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo) { (void)signo; g_stop = 1; }

static int load_file(const char *path, uint8_t **out_buf, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "host: cannot open '%s'\n", path); return -1; }
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

    /* Install SIGINT handler. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file(elf_path, &elf, &elf_size) != 0) return 1;

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
    if (!vm_host_install_stdio(&sys)) {
        fprintf(stderr, "host: vm_host_install_stdio failed\n");
        return 1;
    }

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        fprintf(stderr, "host: load failed (code=%d)\n", lr.code);
        return 1;
    }

    fprintf(stderr, "host: running counter. Ctrl-C to stop.\n");

    /* Custom loop instead of vm_system_run: we want to check the
     * SIGINT flag every scheduler step. */
    for (;;) {
        if (g_stop) {
            fprintf(stderr, "\nhost: SIGINT received, stopping.\n");
            break;
        }
        VmSchedStepResult r = vm_system_step(&sys);
        if (r == VM_SCHED_ALL_HALTED) {
            fprintf(stderr, "host: guest halted.\n");
            break;
        }
        /* VM_SCHED_IDLE means no ready VMs but some blocked; in
         * this example the guest never blocks, so we don't hit it.
         * If we did, we'd typically sleep a short time. */
    }

    vm_system_destroy(&sys);
    free(elf);
    return 0;
}
