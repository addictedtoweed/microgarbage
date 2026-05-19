/* 04_keydump/host.c — runs the keydump guest in raw terminal mode.
 *
 * Demonstrates:
 *   - vm_host_install_stdio_ex with raw_mode = true: puts the
 *     controlling terminal into raw mode so each keystroke
 *     arrives immediately as bytes (no line buffering, no echo,
 *     no signal generation from Ctrl-C/Ctrl-Z).
 *   - Raw mode is restored automatically at process exit via
 *     atexit, so the user's shell doesn't end up in a broken
 *     state.
 *
 * IMPORTANT NOTE: in raw mode, Ctrl-C does NOT generate SIGINT —
 * the byte 0x03 is just delivered to the guest. To stop this
 * example, press 'q' (the guest exits and the host follows). If
 * you wedge the terminal somehow, use 'reset' from another shell
 * or close and reopen the terminal.
 */

/* For sigaction (only as fallback for non-tty input where ISIG
 * still works). */
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

    /* SIGINT handler — only useful when stdin isn't a tty
     * (because raw-mode termios disables ISIG). On a tty in raw
     * mode, Ctrl-C just delivers 0x03 to the guest. */
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

    /* Use the _ex installer to enable raw mode on stdin. The
     * vm_host_stdio module saves the original termios at install
     * and restores it via atexit, so exiting (cleanly or via
     * Ctrl-C followed by program-internal cleanup) leaves the
     * terminal in a sane state. */
    VmHostStdioConfig sio = {
        .raw_mode = true,
        /* stdin/stdout/stderr default to the process's own */
    };
    if (!vm_host_install_stdio_ex(&sys, &sio)) {
        fprintf(stderr, "host: vm_host_install_stdio_ex failed\n");
        return 1;
    }

    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 4096,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        fprintf(stderr, "host: load failed (code=%d)\n", lr.code);
        return 1;
    }

    /* Manual scheduler loop so we can check g_stop. With a tty
     * stdin in raw mode, SIGINT is unreachable from user keypresses
     * (the guest gets the bytes instead) — but on a piped stdin
     * (e.g., test harness) SIGINT still works. */
    for (;;) {
        if (g_stop) {
            fprintf(stderr, "\r\nhost: SIGINT received, stopping.\r\n");
            break;
        }
        VmSchedStepResult r = vm_system_step(&sys);
        if (r == VM_SCHED_ALL_HALTED) break;
    }

    vm_system_destroy(&sys);
    free(elf);
    return 0;
}
