/* 05_shell/host.c — run the file-system shell guest.
 *
 * Sets up the full stack:
 *   - 64 KB trashdrive (RAM block device)
 *   - FatFs filesystem mounted at "0:/" (format-on-start)
 *   - VmSystem with stdio bridge and file syscalls installed
 *   - shell.elf loaded as the sole VM
 *
 * Then runs the scheduler until the shell calls SYS_EXIT (or
 * Ctrl-C from the user, since this example uses cooked-mode
 * terminal input, not raw mode).
 *
 * Build dependencies (beyond the standard -Iinclude):
 *   -Ithird_party/fatfs/source -Ithird_party/fatfs -DHAVE_FATFS
 *
 * Without those flags this example will fail to build because
 * FatFs symbols (f_mount, f_mkfs, etc.) won't resolve. See the
 * build.sh in this directory for the full link line.
 */

#define _POSIX_C_SOURCE 200809L

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_fs.h"
#include "storage/trashdrive.h"
#include "storage/trashdrive_fatfs.h"
#include "ff.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>

/* ---------------------------------------------------------------
 * Backing storage
 *
 * The trashdrive pool needs to be large enough for FatFs R0.16 to
 * lay out a valid FAT volume. Empirically the minimum on R0.16
 * with our config (FM_FAT, n_fat=1) is around 96 KB; below that
 * f_mkfs returns FR_MKFS_ABORTED. We use 128 KB to leave headroom
 * for files plus FatFs's own bookkeeping.
 *
 * If you change this, keep it a multiple of TRASH_SECTOR_SIZE (512).
 * --------------------------------------------------------------- */
#define POOL_BYTES   (128 * 1024)
#define SHARED_BYTES (64 * 1024)
#define LOCAL_BYTES  (96 * 1024)

static uint8_t g_pool[POOL_BYTES];
static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];

static TrashDrive g_drive;
static FATFS g_fs;

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
    const char *elf_path = (argc >= 2) ? argv[1] : "build/shell.elf";

    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);

    /* 1. Initialize the block device. */
    if (trash_init(&g_drive, g_pool, sizeof(g_pool)) != TRASH_OK) {
        fprintf(stderr, "host: trash_init failed\n");
        return 1;
    }

    /* 2. Register with FatFs as drive 0. */
    if (!trash_fatfs_register(0, &g_drive)) {
        fprintf(stderr, "host: trash_fatfs_register failed\n");
        return 1;
    }

    /* 3. Format the volume (always — trashdrive is RAM so we
     * start fresh each run). For persistence between runs you'd
     * skip f_mkfs and just f_mount; FatFs auto-detects a
     * pre-formatted volume. */
    BYTE work[FF_MAX_SS];
    MKFS_PARM opt = {0};
    opt.fmt = FM_FAT;
    opt.n_fat = 1;
    FRESULT fr = f_mkfs("0:", &opt, work, sizeof(work));
    if (fr != FR_OK) {
        fprintf(stderr, "host: f_mkfs failed: %d\n", fr);
        return 1;
    }

    /* 4. Mount the volume. */
    fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) {
        fprintf(stderr, "host: f_mount failed: %d\n", fr);
        return 1;
    }

    /* Pre-create a few items in the volume so `ls` has something
     * to show on first launch. Pure convenience — remove if you
     * want a truly empty start. */
    f_mkdir("0:/home");
    f_mkdir("0:/tmp");
    {
        FIL f;
        UINT bw;
        if (f_open(&f, "0:/readme.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            const char *msg =
                "Welcome to the VM shell.\n"
                "Try: ls, cd /home, mkdir foo, touch bar.txt, cat readme.txt\n";
            f_write(&f, msg, (UINT)strlen(msg), &bw);
            f_close(&f);
        }
    }

    /* 5. Load the guest ELF. */
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file(elf_path, &elf, &elf_size) != 0) {
        return 1;
    }

    /* 6. Build the VmSystem and install both bridges. */
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
    if (!vm_host_install_fs(&sys)) {
        fprintf(stderr, "host: vm_host_install_fs failed\n");
        return 1;
    }

    /* 7. Load the shell. */
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 16 * 1024,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        fprintf(stderr, "host: load failed (code=%d)\n", lr.code);
        return 1;
    }

    /* 8. Run. The shell never exits on its own unless the user
     * types 'exit' (or the host terminates). We use the stepping
     * loop rather than vm_system_run so SIGINT can break us out
     * cleanly. */
    for (;;) {
        if (g_stop) {
            fprintf(stderr, "\nhost: SIGINT received, stopping.\n");
            break;
        }
        VmSchedStepResult r = vm_system_step(&sys);
        if (r == VM_SCHED_ALL_HALTED) {
            break;
        }
        /* VM_SCHED_IDLE means all VMs are blocked. The shell
         * blocks on SYS_READ when the user isn't typing — but
         * our SYS_READ is non-blocking (returns 0), so the shell
         * actually loops with SYS_YIELDs. We won't see IDLE in
         * practice. */
    }

    vm_system_destroy(&sys);
    f_mount(NULL, "0:", 0);
    free(elf);
    return 0;
}
