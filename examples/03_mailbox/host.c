/* 03_mailbox/host.c — loads two guests (producer + consumer) and
 * runs the scheduler until both halt.
 *
 * Demonstrates:
 *   - Loading multiple guests into one VmSystem
 *   - The scheduler round-robining between them
 *   - Cross-VM messaging via the mailbox subsystem
 *
 * Each guest gets its own VmCpu, its own data region, and its own
 * mailbox — all allocated from the local arena by
 * vm_system_load_vm. The producer and consumer cooperate via the
 * mailbox ABI (SYS_SEND / SYS_RECV / SYS_WHITELIST_ADD).
 */

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

/* Local arena needs to fit two VmCpus + two mailboxes + two
 * data regions + a bit of overhead. We size generously. */
#define SHARED_BYTES  (64 * 1024)
#define LOCAL_BYTES   (128 * 1024)

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

int main(void) {
    /* 1. Load both ELFs. */
    uint8_t *producer_elf = NULL, *consumer_elf = NULL;
    size_t producer_size = 0, consumer_size = 0;
    if (load_file("build/producer.elf", &producer_elf, &producer_size) != 0) return 1;
    if (load_file("build/consumer.elf", &consumer_elf, &consumer_size) != 0) return 1;

    /* 2. Initialize the system. */
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

    /* 3. Install the stdio bridge so the consumer can use SYS_WRITE. */
    if (!vm_host_install_stdio(&sys)) {
        fprintf(stderr, "host: vm_host_install_stdio failed\n");
        return 1;
    }

    /* 4. Load the producer FIRST so it gets vm_id 0, then the
     *    consumer (vm_id 1). The hard-coded vm_ids in the guests
     *    rely on this ordering. */
    VmLoadVmResult lr0 = vm_system_load_vm(&sys, producer_elf, producer_size,
                                           4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    if (lr0.code != VM_SYS_OK || lr0.assigned_vm_id != 0) {
        fprintf(stderr, "host: producer load failed (code=%d, id=%d)\n",
                lr0.code, lr0.assigned_vm_id);
        return 1;
    }

    VmLoadVmResult lr1 = vm_system_load_vm(&sys, consumer_elf, consumer_size,
                                           4096,
                                           VM_BACKING_COPY_RAM,
                                           VM_BACKING_COPY_RAM);
    if (lr1.code != VM_SYS_OK || lr1.assigned_vm_id != 1) {
        fprintf(stderr, "host: consumer load failed (code=%d, id=%d)\n",
                lr1.code, lr1.assigned_vm_id);
        return 1;
    }

    fprintf(stderr, "host: loaded producer (vm 0) and consumer (vm 1)\n");
    fprintf(stderr, "host: running until both halt...\n\n");

    /* 5. Run. The cycle cap is conservative: producer + consumer
     *    each do ~10 iterations of small loops, plus a handful of
     *    yields and ecalls per iteration. 100k cycles is several
     *    orders of magnitude more than needed. */
    bool done = vm_system_run(&sys, 100000);
    if (!done) {
        fprintf(stderr, "\nhost: cycle cap hit (guests still running)\n");
    } else {
        fprintf(stderr, "\nhost: all guests halted\n");
    }

    vm_system_destroy(&sys);
    free(producer_elf);
    free(consumer_elf);
    return 0;
}
