/* 10_hwio_sim/host.c — embedding host that installs the hardware-IO
 * ecalls with the desktop SIMULATION backend.
 *
 * Same minimal shape as 01_hello, plus one line: vm_host_install_hwio.
 * The guest issues portable GPIO/I2C/SPI/ADC/PWM transactions; the sim
 * backend (src/host/platform_hwio_sim.c) answers them with a fake pin
 * bank and a fake I2C temperature sensor — proving the whole path
 * (guest hwio.h -> ecall -> pointer translation -> host_hwio_* backend)
 * on a PC with no real hardware. The real platform_rpi.c mirrors this.
 *
 * Public domain (CC0). No warranty.
 */

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_hwio.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* Roomy pools — the default slab bucket layout has grown past the old
 * 64 KB that 01_hello assumes; a PC host can be generous. Production /
 * MCU builds tune slab_config + these sizes to fit. */
#define SHARED_BYTES (1024 * 1024)
#define LOCAL_BYTES  (1024 * 1024)

static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];

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
        .spawn_data_kb       = 8,
    };
    if (!vm_system_init(&sys, &cfg)) {
        fprintf(stderr, "host: vm_system_init failed\n");
        return 1;
    }

    if (!vm_host_install_stdio(&sys)) {
        fprintf(stderr, "host: vm_host_install_stdio failed\n");
        return 1;
    }
    /* The one new line vs 01_hello: wire up the hardware-IO ecalls. */
    if (!vm_host_install_hwio(&sys)) {
        fprintf(stderr, "host: vm_host_install_hwio failed\n");
        return 1;
    }

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size,
                                          /*data_region=*/ 8192,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        fprintf(stderr, "host: load failed (code=%d, load_result=%d)\n",
                lr.code, lr.load_result);
        return 1;
    }

    bool done = vm_system_run(&sys, 100000);
    if (!done) fprintf(stderr, "host: cycle cap hit (guest still running)\n");

    vm_system_destroy(&sys);
    free(elf);
    return 0;
}
