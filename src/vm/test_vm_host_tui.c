/* ============================================================
 *  test_vm_host_tui.c — tests for the TUI service.
 *
 *  Strategy: redirect stdout to a temp file, drive the syscalls
 *  via vm_ecall_dispatch, then inspect the captured output for
 *  the expected escape sequences and characters.
 * ============================================================ */

#define _POSIX_C_SOURCE 200809L

#include "test_runner.h"
#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_tui.h"
#include "vm/vm_ecall.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define SHARED_BYTES (16 * 1024)
#define LOCAL_BYTES  (128 * 1024)
#define DATA_BYTES   (8 * 1024)

static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];
static uint8_t g_data[DATA_BYTES];

static VmSystem g_sys;
static VmCpu    g_cpu;

static bool fixture_init(void) {
    memset(g_data, 0, sizeof(g_data));

    VmSystemConfig cfg = {
        .shared_storage = g_shared,
        .shared_storage_size = SHARED_BYTES,
        .local_storage = g_local,
        .local_storage_size = LOCAL_BYTES,
        .max_vms = 2,
        .spawn_data_kb = 8,
    };
    if (!vm_system_init(&g_sys, &cfg)) return false;
    if (!vm_host_install_tui(&g_sys)) return false;

    vm_init(&g_cpu, 0);
    g_cpu.regions[2].base     = g_data;
    g_cpu.regions[2].length   = sizeof(g_data);
    g_cpu.regions[2].writable = true;
    return true;
}

static void fixture_teardown(void) {
    /* Force release in case a test left the canvas owned. */
    vm_host_tui_release_for_vm(0);
    vm_host_tui_release_for_vm(1);
    vm_system_destroy(&g_sys);
}

static int32_t invoke_syscall(uint32_t num, uint32_t a0, uint32_t a1, uint32_t a2) {
    g_cpu.regs[VM_REG_A7] = num;
    g_cpu.regs[VM_REG_A0] = a0;
    g_cpu.regs[VM_REG_A1] = a1;
    g_cpu.regs[VM_REG_A2] = a2;
    vm_ecall_dispatch(g_sys.ecall_router, &g_cpu, &g_sys);
    return (int32_t)g_cpu.regs[VM_REG_A0];
}

/* Redirect fd 1 to a tmp file and return captured bytes. */
typedef struct {
    int saved_fd;
    int tmp_fd;
    char path[64];
} Capture;

static bool cap_begin(Capture *c) {
    fflush(stdout); fflush(stderr);
    strcpy(c->path, "/tmp/microgarbage_tui_XXXXXX");
    c->tmp_fd = mkstemp(c->path);
    if (c->tmp_fd < 0) return false;
    c->saved_fd = dup(1);
    if (c->saved_fd < 0) { close(c->tmp_fd); return false; }
    if (dup2(c->tmp_fd, 1) < 0) {
        close(c->saved_fd); close(c->tmp_fd);
        return false;
    }
    return true;
}

static char *cap_end(Capture *c, size_t *out_len) {
    fflush(stdout);
    fsync(1);
    dup2(c->saved_fd, 1);
    close(c->saved_fd);
    lseek(c->tmp_fd, 0, SEEK_SET);
    char *buf = malloc(65536);
    if (!buf) { close(c->tmp_fd); unlink(c->path); return NULL; }
    ssize_t n = read(c->tmp_fd, buf, 65535);
    if (n < 0) n = 0;
    buf[n] = '\0';
    if (out_len) *out_len = (size_t)n;
    close(c->tmp_fd);
    unlink(c->path);
    return buf;
}

/* ============================================================
 *  Tests
 * ============================================================ */

static void test_init_shutdown(void) {
    ASSERT(fixture_init());
    Capture cap;
    ASSERT(cap_begin(&cap));

    /* Default flags: no alt-screen, no mouse, no cursor hide.
     * Output should be empty (just the canvas init has no writes). */
    int32_t r = invoke_syscall(SYS_TUI_INIT, 10, 40, 0);
    ASSERT_EQ_INT(0, r);

    /* GET_DIMS returns rows<<16 | cols. */
    int32_t d = invoke_syscall(SYS_TUI_GET_DIMS, 0, 0, 0);
    ASSERT_EQ_INT((10 << 16) | 40, d);

    r = invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    ASSERT_EQ_INT(0, r);

    size_t n = 0;
    char *out = cap_end(&cap, &n);
    /* Shutdown without alt-screen writes "\x1b[0m\r\n" (6 bytes). */
    ASSERT(n >= 4);
    ASSERT(strstr(out, "\x1b[0m") != NULL);
    free(out);
    fixture_teardown();
}

static void test_init_busy_when_another_owns(void) {
    ASSERT(fixture_init());

    /* VM 0 inits. */
    g_cpu.vm_id = 0;
    ASSERT_EQ_INT(0, invoke_syscall(SYS_TUI_INIT, 10, 40, 0));

    /* VM 1 tries to init. Should get -EBUSY. */
    g_cpu.vm_id = 1;
    ASSERT_EQ_INT(-(int32_t)VM_EBUSY, invoke_syscall(SYS_TUI_INIT, 10, 40, 0));

    /* VM 0 shuts down. */
    g_cpu.vm_id = 0;
    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);

    /* Now VM 1 can init. */
    g_cpu.vm_id = 1;
    ASSERT_EQ_INT(0, invoke_syscall(SYS_TUI_INIT, 10, 40, 0));
    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);

    fixture_teardown();
}

static void test_flush_draw_set_cell(void) {
    ASSERT(fixture_init());
    Capture cap;
    ASSERT(cap_begin(&cap));

    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);

    /* Build a draw buffer at g_data[0]:
     *   OP_SET_CELL(row=1, col=1, c='X', fg=7, bg=256, attrs=0)  → 11 bytes
     *   OP_END                                                    → 1 byte
     */
    uint8_t *db = g_data;
    int o = 0;
    db[o++] = VM_TUI_OP_SET_CELL;
    db[o++] = 1; db[o++] = 0;          /* row=1 (u16 LE) */
    db[o++] = 1; db[o++] = 0;          /* col=1 */
    db[o++] = 'X';
    db[o++] = 7; db[o++] = 0;          /* fg=7 */
    db[o++] = 0; db[o++] = 1;          /* bg=256 (LE: 0x0100) */
    db[o++] = 0;                        /* attrs */
    db[o++] = VM_TUI_OP_END;

    int32_t r = invoke_syscall(SYS_TUI_FLUSH_DRAW, 0x80000000u, (uint32_t)o, 0);
    ASSERT_EQ_INT(0, r);

    /* Now PRESENT. Should emit at least one escape sequence and an 'X'. */
    r = invoke_syscall(SYS_TUI_PRESENT, 0, 0, 0);
    ASSERT_EQ_INT(0, r);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);

    size_t n = 0;
    char *out = cap_end(&cap, &n);
    ASSERT(strstr(out, "\x1b[1;1H") != NULL);    /* cursor home at row 1 */
    ASSERT(strchr(out, 'X') != NULL);
    free(out);
    fixture_teardown();
}

static void test_flush_draw_clear(void) {
    ASSERT(fixture_init());

    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);

    /* CLEAR should set all cells to space + default colors. */
    uint8_t *db = g_data;
    int o = 0;
    db[o++] = VM_TUI_OP_CLEAR;
    db[o++] = VM_TUI_OP_END;

    int32_t r = invoke_syscall(SYS_TUI_FLUSH_DRAW, 0x80000000u, (uint32_t)o, 0);
    ASSERT_EQ_INT(0, r);
    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_flush_draw_print(void) {
    ASSERT(fixture_init());
    Capture cap;
    ASSERT(cap_begin(&cap));

    invoke_syscall(SYS_TUI_INIT, 5, 20, 0);

    /* PRINT "Hi" at row 1, col 1, fg=7, bg=256, attrs=0. */
    uint8_t *db = g_data;
    int o = 0;
    db[o++] = VM_TUI_OP_PRINT;
    db[o++] = 1; db[o++] = 0;       /* row=1 */
    db[o++] = 1; db[o++] = 0;       /* col=1 */
    db[o++] = 7; db[o++] = 0;       /* fg */
    db[o++] = 0; db[o++] = 1;       /* bg=256 */
    db[o++] = 0;                     /* attrs */
    db[o++] = 2; db[o++] = 0;       /* len=2 */
    db[o++] = 'H';
    db[o++] = 'i';
    db[o++] = VM_TUI_OP_END;

    int32_t r = invoke_syscall(SYS_TUI_FLUSH_DRAW, 0x80000000u, (uint32_t)o, 0);
    ASSERT_EQ_INT(0, r);

    invoke_syscall(SYS_TUI_PRESENT, 0, 0, 0);
    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);

    size_t n = 0;
    char *out = cap_end(&cap, &n);
    /* The output should contain "Hi" somewhere. */
    ASSERT(strstr(out, "Hi") != NULL);
    free(out);
    fixture_teardown();
}

static void test_flush_draw_rejects_bad_op(void) {
    ASSERT(fixture_init());

    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);

    uint8_t *db = g_data;
    db[0] = 99;        /* not a real op */
    db[1] = VM_TUI_OP_END;
    int32_t r = invoke_syscall(SYS_TUI_FLUSH_DRAW, 0x80000000u, 2, 0);
    ASSERT_EQ_INT(-(int32_t)VM_EINVAL, r);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_flush_draw_rejects_oversize(void) {
    ASSERT(fixture_init());

    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);

    /* Send a flush with len > VM_TUI_MAX_FLUSH_BYTES. */
    int32_t r = invoke_syscall(SYS_TUI_FLUSH_DRAW, 0x80000000u,
                                VM_TUI_MAX_FLUSH_BYTES + 1, 0);
    ASSERT_EQ_INT(-(int32_t)VM_EINVAL, r);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_present_without_init_returns_ebusy(void) {
    ASSERT(fixture_init());

    /* Without INIT, no VM owns the canvas. PRESENT should EBUSY. */
    int32_t r = invoke_syscall(SYS_TUI_PRESENT, 0, 0, 0);
    ASSERT_EQ_INT(-(int32_t)VM_EBUSY, r);

    fixture_teardown();
}

static void test_release_for_vm(void) {
    ASSERT(fixture_init());

    g_cpu.vm_id = 0;
    ASSERT_EQ_INT(0, invoke_syscall(SYS_TUI_INIT, 5, 10, 0));

    /* Simulate VM 0 dying without calling shutdown. */
    vm_host_tui_release_for_vm(0);

    /* VM 1 can now init. */
    g_cpu.vm_id = 1;
    ASSERT_EQ_INT(0, invoke_syscall(SYS_TUI_INIT, 5, 10, 0));
    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);

    fixture_teardown();
}

int main(void) {
    TEST_SUITE("vm_host_tui");
    RUN(test_init_shutdown);
    RUN(test_init_busy_when_another_owns);
    RUN(test_flush_draw_set_cell);
    RUN(test_flush_draw_clear);
    RUN(test_flush_draw_print);
    RUN(test_flush_draw_rejects_bad_op);
    RUN(test_flush_draw_rejects_oversize);
    RUN(test_present_without_init_returns_ebusy);
    RUN(test_release_for_vm);
    return TEST_SUITE_RESULT();
}
