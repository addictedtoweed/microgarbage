/* Tests for the host stdio bridge (vm_host_stdio.c) and the
 * counter example.
 *
 * These tests work by redirecting fd 1 (stdout) to a temp file
 * via dup/dup2, running the VM for a bounded number of cycles,
 * then reading the file back to check the output.
 *
 * Some of the tests load real ELFs from disk; they must run
 * from the repository root so paths resolve. */

/* For dup, dup2, getpid — must precede any system header. */
#define _POSIX_C_SOURCE 200809L

#include "test_runner.h"
#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SHARED_BYTES  (64 * 1024)
#define LOCAL_BYTES   (256 * 1024)

static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];

static int load_file(const char *path, uint8_t **out_buf, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return -1; }
    *out_buf = buf;
    *out_size = (size_t)sz;
    return 0;
}

/* Run a guest ELF with stdout captured to a buffer. Returns the
 * total bytes written and writes them into `out_buf` (up to
 * out_cap-1 bytes, then null-terminates).
 *
 * Uses dup/dup2 on fd 1 to redirect stdout to a pipe and restore
 * it afterward. This properly preserves the test runner's own
 * stdout (which would print PASS/FAIL lines for subsequent tests). */
static int run_with_captured_stdout(const char *elf_path,
                                     uint64_t max_cycles,
                                     char *out_buf, size_t out_cap) {
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file(elf_path, &elf, &elf_size) != 0) return -1;

    /* Flush stdout so the captured output starts fresh. */
    fflush(stdout);

    /* Save fd 1, open a temp file, point fd 1 at it. */
    int saved_stdout = dup(1);
    if (saved_stdout < 0) { free(elf); return -1; }

    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path),
             "/tmp/vm_runner_test_%d.out", (int)getpid());
    FILE *tmp = fopen(tmp_path, "w+");
    if (!tmp) {
        close(saved_stdout);
        free(elf);
        return -1;
    }
    if (dup2(fileno(tmp), 1) < 0) {
        fclose(tmp);
        close(saved_stdout);
        free(elf);
        return -1;
    }

    /* Set up and run the VM. */
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage      = g_shared,
        .shared_storage_size = SHARED_BYTES,
        .local_storage       = g_local,
        .local_storage_size  = LOCAL_BYTES,
        .baseline_quantum    = 200,
    };
    bool init_ok = vm_system_init(&sys, &cfg);
    if (init_ok) {
        vm_host_install_stdio(&sys);
        VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size,
                                               /*data_region=*/16 * 1024,
                                               VM_BACKING_COPY_RAM,
                                               VM_BACKING_COPY_RAM);
        if (lr.code == VM_SYS_OK) {
            vm_system_run(&sys, max_cycles);
        }
        vm_system_destroy(&sys);
    }

    /* Flush whatever was written to the redirected stdout. */
    fflush(stdout);

    /* Restore stdout. */
    dup2(saved_stdout, 1);
    close(saved_stdout);

    /* Read back what was captured. */
    fseek(tmp, 0, SEEK_SET);
    size_t n = fread(out_buf, 1, out_cap - 1, tmp);
    out_buf[n] = '\0';
    fclose(tmp);
    remove(tmp_path);

    free(elf);
    return init_ok ? (int)n : -1;
}

/* ============================================================
 *  Tests
 * ============================================================ */

static void test_counter_writes_to_stdout(void) {
    char buf[4096];
    int n = run_with_captured_stdout("examples/02_counter/build/guest.elf",
                                      /*max_cycles=*/2000,
                                      buf, sizeof(buf));
    if (n < 0) {
        FAIL("could not run counter.elf (need to run from repo root)");
        return;
    }
    /* Should have written several lines like "counter: N\n". */
    ASSERT(n > 0);
    ASSERT(strstr(buf, "counter: 0\n") != NULL);
    ASSERT(strstr(buf, "counter: 1\n") != NULL);
    ASSERT(strstr(buf, "counter: 2\n") != NULL);
}

static void test_install_stdio_returns_true(void) {
    /* Build a system with no autoinstall conflict, install, verify
     * it returns true and SYS_WRITE is now in the router. */
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage      = g_shared,
        .shared_storage_size = SHARED_BYTES,
        .local_storage       = g_local,
        .local_storage_size  = LOCAL_BYTES,
    };
    ASSERT(vm_system_init(&sys, &cfg));
    ASSERT(vm_host_install_stdio(&sys));

    /* Second call should fail — already installed. */
    ASSERT(!vm_host_install_stdio(&sys));

    vm_system_destroy(&sys);
}

static void test_install_stdio_null_safe(void) {
    ASSERT(!vm_host_install_stdio(NULL));
}

/* ============================================================
 *  SYS_READ tests
 *
 *  Use pipes to inject known bytes into the VM's stdin and a
 *  redirected stdout to read back what the guest wrote in
 *  response. Tests the keydump.elf guest, which echoes each
 *  input byte as a hex-bracketed string and exits on 'q'.
 * ============================================================ */

/* Run keydump.elf with controlled stdin/stdout. Returns the
 * bytes written to stdout via the out_buf parameter (null-terminated).
 * input bytes are fed via a pipe. */
static int run_keydump_with_input(const char *input, size_t input_len,
                                   char *out_buf, size_t out_cap) {
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file("examples/04_keydump/build/guest.elf", &elf, &elf_size) != 0) {
        return -1;
    }

    /* Build a stdin pipe: we'll write `input` to the write end,
     * the VM reads from the read end. */
    int in_pipe[2];
    if (pipe(in_pipe) != 0) { free(elf); return -1; }
    if (write(in_pipe[1], input, input_len) != (ssize_t)input_len) {
        close(in_pipe[0]); close(in_pipe[1]);
        free(elf); return -1;
    }
    close(in_pipe[1]);  /* Close write end so guest sees EOF after
                         * consuming all bytes. */

    FILE *stdin_file = fdopen(in_pipe[0], "r");
    if (!stdin_file) {
        close(in_pipe[0]); free(elf); return -1;
    }

    /* Redirect stdout to a temp file. */
    fflush(stdout);
    int saved_stdout = dup(1);
    if (saved_stdout < 0) {
        fclose(stdin_file); free(elf); return -1;
    }
    char tmp_path[64];
    snprintf(tmp_path, sizeof(tmp_path),
             "/tmp/vm_keydump_test_%d.out", (int)getpid());
    FILE *tmp = fopen(tmp_path, "w+");
    if (!tmp) {
        close(saved_stdout); fclose(stdin_file); free(elf); return -1;
    }
    dup2(fileno(tmp), 1);

    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage      = g_shared,
        .shared_storage_size = SHARED_BYTES,
        .local_storage       = g_local,
        .local_storage_size  = LOCAL_BYTES,
        .baseline_quantum    = 200,
    };
    bool init_ok = vm_system_init(&sys, &cfg);

    if (init_ok) {
        VmHostStdioConfig sio = {
            .stdin_src = stdin_file,
            /* stdout_dest left NULL — uses (redirected) host stdout */
            .raw_mode = false,
        };
        vm_host_install_stdio_ex(&sys, &sio);

        VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size,
                                               16 * 1024,
                                               VM_BACKING_COPY_RAM,
                                               VM_BACKING_COPY_RAM);
        if (lr.code == VM_SYS_OK) {
            /* Run with a generous cap; the keydump halts itself on
             * 'q' or on EIO when the pipe closes. */
            vm_system_run(&sys, 10000);
        }
        vm_system_destroy(&sys);
    }

    fflush(stdout);
    dup2(saved_stdout, 1);
    close(saved_stdout);

    fseek(tmp, 0, SEEK_SET);
    size_t n = fread(out_buf, 1, out_cap - 1, tmp);
    out_buf[n] = '\0';
    fclose(tmp);
    remove(tmp_path);
    fclose(stdin_file);   /* also closes in_pipe[0] */

    free(elf);
    return init_ok ? (int)n : -1;
}

static void test_sys_read_basic_characters(void) {
    /* Feed "hiq" to keydump. Expect to see [68][69][71] in output
     * (h=0x68, i=0x69, q=0x71). The 'q' causes guest to exit. */
    char out[1024];
    int n = run_keydump_with_input("hiq", 3, out, sizeof(out));
    if (n < 0) {
        FAIL("could not run keydump.elf");
        return;
    }
    ASSERT(strstr(out, "[68]") != NULL);
    ASSERT(strstr(out, "[69]") != NULL);
    ASSERT(strstr(out, "[71]") != NULL);
    /* Output should also contain the banner and the exit message. */
    ASSERT(strstr(out, "press 'q' to quit") != NULL);
    ASSERT(strstr(out, "'q' pressed") != NULL);
}

static void test_sys_read_escape_sequences(void) {
    /* Feed an up-arrow (ESC [ A) followed by 'q'. The guest should
     * print [1B][5B][41][71]. */
    const char input[] = { 0x1B, '[', 'A', 'q' };
    char out[1024];
    int n = run_keydump_with_input(input, sizeof(input), out, sizeof(out));
    if (n < 0) {
        FAIL("could not run keydump.elf");
        return;
    }
    ASSERT(strstr(out, "[1B][5B][41]") != NULL);
    ASSERT(strstr(out, "[71]") != NULL);
}

static void test_sys_read_eof_handled(void) {
    /* Feed empty input. Pipe closes immediately; guest should see
     * EIO and print "stdin closed" then exit. */
    char out[1024];
    int n = run_keydump_with_input("", 0, out, sizeof(out));
    if (n < 0) {
        FAIL("could not run keydump.elf");
        return;
    }
    ASSERT(strstr(out, "stdin closed") != NULL);
}

int main(void) {
    TEST_SUITE("vm_host_stdio");

    RUN(test_install_stdio_returns_true);
    RUN(test_install_stdio_null_safe);
    RUN(test_counter_writes_to_stdout);
    RUN(test_sys_read_basic_characters);
    RUN(test_sys_read_escape_sequences);
    RUN(test_sys_read_eof_handled);

    return TEST_SUITE_RESULT();
}
