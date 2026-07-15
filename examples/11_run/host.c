/* 11_run/host.c — universal desktop guest runner.
 *
 * The local counterpart to loading a guest on the Pi dev kit: it wires
 * up the common host services — stdio, the platform layer (printf /
 * rand / realtime), and the simulated hardware-IO backend — with roomy
 * pools, loads an ELF from the command line, runs it to exit, and
 * reports. Pair it with rvcc for the full dev-kit loop:
 *
 *     rvcc app.c -o app.elf
 *     ./build/run app.elf
 *
 * The SAME app.elf runs here and on the Pi against the real backend —
 * only the host shim changes.
 *
 * Public domain (CC0). No warranty.
 */

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_platform.h"
#include "vm/vm_host_hwio.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define SHARED_BYTES (1024 * 1024)
#define LOCAL_BYTES  (1024 * 1024)

static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];

static int load_file(const char *path, uint8_t **out_buf, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "run: cannot open '%s'\n", path); return -1; }
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
    const char *elf_path = (argc >= 2) ? argv[1] : "build/hello.elf";

    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file(elf_path, &elf, &elf_size) != 0) return 1;

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage      = g_shared,
        .shared_storage_size = SHARED_BYTES,
        .local_storage       = g_local,
        .local_storage_size  = LOCAL_BYTES,
        .max_vms             = 1,
        .spawn_data_kb       = 16,
    };
    if (!vm_system_init(&sys, &cfg)) {
        fprintf(stderr, "run: vm_system_init failed\n");
        return 1;
    }

    /* Common dev-kit services. */
    VmHostPlatformConfig pc = {0};   /* defaults: no realtime clock, seeded rand */
    if (!vm_host_install_stdio(&sys)      /* SYS_WRITE / fd stdio            */
     || !vm_host_install_platform(&sys, &pc)  /* printf / rand / realtime  */
     || !vm_host_install_hwio(&sys)) {    /* GPIO / I2C / SPI / ADC / PWM   */
        fprintf(stderr, "run: service install failed\n");
        return 1;
    }

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size,
                                          /*data_region=*/ 16384,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        fprintf(stderr, "run: load failed (code=%d, load_result=%d)\n",
                lr.code, lr.load_result);
        return 1;
    }

    bool done = vm_system_run(&sys, 100000000ull);
    if (!done) fprintf(stderr, "run: cycle cap hit (guest still running)\n");

    vm_system_destroy(&sys);
    free(elf);
    return done ? 0 : 2;
}
