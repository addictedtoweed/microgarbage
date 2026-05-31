/* ============================================================
 *  test_vm_host_tui.c — tests for the TUI service.
 *
 *  Strategy: redirect stdout to a temp file, drive the syscalls
 *  via vm_ecall_dispatch, then inspect the captured output for
 *  the expected escape sequences and characters.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#define _POSIX_C_SOURCE 200809L

#include "test_runner.h"
#include "test_portable.h"
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
    char path[260];
} Capture;

static bool cap_begin(Capture *c) {
    fflush(stdout); fflush(stderr);
    snprintf(c->path, sizeof c->path, "%s/microgarbage_tui_XXXXXX", tp_tmpdir());
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
    tp_fsync(1);
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

/* ============================================================
 *  Input parser tests
 *
 *  Use the test-only injection hook to feed bytes into the
 *  input ring without going through real stdin.
 * ============================================================ */

extern unsigned vm_host_tui_test_inject_input_(const void *bytes, unsigned n);

static void test_poll_event_printable_ascii(void) {
    ASSERT(fixture_init());
    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);

    vm_host_tui_test_inject_input_("q", 1);
    uint32_t evp = 0x80000000u;
    int32_t r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    VmTuiEventRecord *ev = (VmTuiEventRecord *)g_data;
    ASSERT_EQ_INT(VM_TUI_EVK_KEY, ev->kind);
    ASSERT_EQ_INT('q', ev->key);

    /* Second poll: no event */
    r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(0, r);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_poll_event_enter(void) {
    ASSERT(fixture_init());
    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);

    vm_host_tui_test_inject_input_("\r", 1);
    uint32_t evp = 0x80000000u;
    int32_t r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    VmTuiEventRecord *ev = (VmTuiEventRecord *)g_data;
    ASSERT_EQ_INT(VM_TUI_KEY_ENTER, ev->key);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_poll_event_arrow_keys(void) {
    ASSERT(fixture_init());
    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);

    /* CSI A = up arrow. Send "\x1b[A". */
    vm_host_tui_test_inject_input_("\x1b[A", 3);
    uint32_t evp = 0x80000000u;
    int32_t r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    VmTuiEventRecord *ev = (VmTuiEventRecord *)g_data;
    ASSERT_EQ_INT(VM_TUI_KEY_UP, ev->key);

    /* CSI B = down. */
    vm_host_tui_test_inject_input_("\x1b[B", 3);
    r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    ASSERT_EQ_INT(VM_TUI_KEY_DOWN, ev->key);

    /* CSI C = right. */
    vm_host_tui_test_inject_input_("\x1b[C", 3);
    r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    ASSERT_EQ_INT(VM_TUI_KEY_RIGHT, ev->key);

    /* CSI D = left. */
    vm_host_tui_test_inject_input_("\x1b[D", 3);
    r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    ASSERT_EQ_INT(VM_TUI_KEY_LEFT, ev->key);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_poll_event_sgr_mouse(void) {
    ASSERT(fixture_init());
    invoke_syscall(SYS_TUI_INIT, 20, 80, 0);

    /* SGR mouse press: CSI < 0 ; 5 ; 10 M
     * button=0 (left), col=5, row=10, final 'M' = press */
    vm_host_tui_test_inject_input_("\x1b[<0;5;10M", 10);
    uint32_t evp = 0x80000000u;
    int32_t r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    VmTuiEventRecord *ev = (VmTuiEventRecord *)g_data;
    ASSERT_EQ_INT(VM_TUI_EVK_MOUSE, ev->kind);
    ASSERT_EQ_INT(VM_TUI_MB_LEFT, ev->button);
    ASSERT_EQ_INT(10, ev->row);
    ASSERT_EQ_INT(5,  ev->col);
    ASSERT(ev->flags & VM_TUI_EVF_PRESS);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

/* Any-motion tracking (xterm 1003): the terminal reports bare
 * cursor movement with button code 35 (motion bit 32 + "no button"
 * 3). The parser must accept this as a motion event with
 * VM_TUI_MB_NONE rather than dropping it — move-to-steer UIs (car)
 * depend on it. */
static void test_poll_event_sgr_mouse_bare_motion(void) {
    ASSERT(fixture_init());
    invoke_syscall(SYS_TUI_INIT, 20, 80, 0);

    /* CSI < 35 ; 12 ; 7 M : motion, no button, col=12, row=7. */
    vm_host_tui_test_inject_input_("\x1b[<35;12;7M", 11);
    uint32_t evp = 0x80000000u;
    int32_t r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    VmTuiEventRecord *ev = (VmTuiEventRecord *)g_data;
    ASSERT_EQ_INT(VM_TUI_EVK_MOUSE, ev->kind);
    ASSERT_EQ_INT(VM_TUI_MB_NONE, ev->button);
    ASSERT_EQ_INT(7,  ev->row);
    ASSERT_EQ_INT(12, ev->col);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_poll_event_function_keys(void) {
    ASSERT(fixture_init());
    invoke_syscall(SYS_TUI_INIT, 20, 80, 0);

    /* F1 via SS3: ESC O P */
    vm_host_tui_test_inject_input_("\x1bOP", 3);
    uint32_t evp = 0x80000000u;
    int32_t r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    VmTuiEventRecord *ev = (VmTuiEventRecord *)g_data;
    ASSERT_EQ_INT(VM_TUI_KEY_F1, ev->key);

    /* F5 via CSI ~: ESC [ 1 5 ~ */
    vm_host_tui_test_inject_input_("\x1b[15~", 5);
    r = invoke_syscall(SYS_TUI_POLL_EVENT, evp, 0, 0);
    ASSERT_EQ_INT(1, r);
    ASSERT_EQ_INT(VM_TUI_KEY_F1 + 4, ev->key);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

/* ============================================================
 *  Tile tests
 * ============================================================ */

static void test_tile_create_returns_handle(void) {
    ASSERT(fixture_init());
    invoke_syscall(SYS_TUI_INIT, 10, 40, 0);

    /* Create a 4x4 tile. */
    int32_t h = invoke_syscall(SYS_TUI_TILE_CREATE, 4, 4, 0);
    ASSERT(h > 0);
    /* Lower 8 bits = slot (should be 0 for first tile). */
    ASSERT_EQ_INT(0, h & 0xff);
    /* Bits 15..8 = vm_id (0 in this fixture). */
    ASSERT_EQ_INT(0, (h >> 8) & 0xff);
    /* Bits 31..16 = generation > 0. */
    ASSERT((h >> 16) > 0);

    /* TILE_DIMS returns (rows<<16)|cols. */
    int32_t d = invoke_syscall(SYS_TUI_TILE_DIMS, h, 0, 0);
    ASSERT_EQ_INT((4 << 16) | 4, d);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_tile_destroy_invalidates_handle(void) {
    ASSERT(fixture_init());
    invoke_syscall(SYS_TUI_INIT, 10, 40, 0);

    int32_t h = invoke_syscall(SYS_TUI_TILE_CREATE, 4, 4, 0);
    ASSERT(h > 0);

    ASSERT_EQ_INT(0, invoke_syscall(SYS_TUI_TILE_DESTROY, h, 0, 0));

    /* Stale handle now returns EBADF. */
    int32_t d = invoke_syscall(SYS_TUI_TILE_DIMS, h, 0, 0);
    ASSERT_EQ_INT(-(int32_t)VM_EBADF, d);

    /* Re-create: should get a different handle (generation bumped). */
    int32_t h2 = invoke_syscall(SYS_TUI_TILE_CREATE, 4, 4, 0);
    ASSERT(h2 > 0);
    ASSERT(h2 != h);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_tile_fill_then_blit(void) {
    ASSERT(fixture_init());
    Capture cap;
    ASSERT(cap_begin(&cap));

    invoke_syscall(SYS_TUI_INIT, 10, 40, 0);

    /* 3x3 tile filled with 'X' (no transparency). */
    int32_t h = invoke_syscall(SYS_TUI_TILE_CREATE, 3, 3, 0);
    ASSERT(h > 0);
    /* TILE_FILL: a1=(c<<8)|attrs, a2=fg, a3=bg */
    uint32_t ca = ((uint32_t)'X' << 8) | 0;
    g_cpu.regs[VM_REG_A7] = SYS_TUI_TILE_FILL;
    g_cpu.regs[VM_REG_A0] = (uint32_t)h;
    g_cpu.regs[VM_REG_A1] = ca;
    g_cpu.regs[VM_REG_A2] = 7;
    g_cpu.regs[VM_REG_A3] = VM_TUI_DEFAULT_COLOR;
    vm_ecall_dispatch(g_sys.ecall_router, &g_cpu, &g_sys);
    ASSERT_EQ_INT(0, (int32_t)g_cpu.regs[VM_REG_A0]);

    /* Blit at (2, 5). */
    g_cpu.regs[VM_REG_A7] = SYS_TUI_TILE_BLIT;
    g_cpu.regs[VM_REG_A0] = (uint32_t)h;
    g_cpu.regs[VM_REG_A1] = 2;
    g_cpu.regs[VM_REG_A2] = 5;
    vm_ecall_dispatch(g_sys.ecall_router, &g_cpu, &g_sys);
    ASSERT_EQ_INT(0, (int32_t)g_cpu.regs[VM_REG_A0]);

    /* Present and verify the X's made it to the terminal. */
    invoke_syscall(SYS_TUI_PRESENT, 0, 0, 0);
    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);

    size_t n = 0;
    char *out = cap_end(&cap, &n);
    /* We should see "XXX" in the output for at least one row. */
    ASSERT(strstr(out, "XXX") != NULL);
    free(out);
    fixture_teardown();
}

static void test_tile_set_then_blit_with_transparency(void) {
    ASSERT(fixture_init());
    Capture cap;
    ASSERT(cap_begin(&cap));

    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);

    /* Background fill canvas via flush_draw OP_FILL_RECT row=1,col=1,h=5,w=10 with 'B' */
    uint8_t *db = g_data;
    int o = 0;
    db[o++] = VM_TUI_OP_FILL_RECT;
    db[o++] = 1; db[o++] = 0;       /* row=1 */
    db[o++] = 1; db[o++] = 0;       /* col=1 */
    db[o++] = 5; db[o++] = 0;       /* h=5 */
    db[o++] = 10; db[o++] = 0;      /* w=10 */
    db[o++] = 'B';
    db[o++] = VM_TUI_OP_END;
    invoke_syscall(SYS_TUI_FLUSH_DRAW, 0x80000000u, (uint32_t)o, 0);

    /* 2x2 tile, fill with 'F', then set (1,1) transparent. */
    int32_t h = invoke_syscall(SYS_TUI_TILE_CREATE, 2, 2, 0);
    ASSERT(h > 0);

    uint32_t ca = ((uint32_t)'F' << 8) | 0;
    g_cpu.regs[VM_REG_A7] = SYS_TUI_TILE_FILL;
    g_cpu.regs[VM_REG_A0] = (uint32_t)h;
    g_cpu.regs[VM_REG_A1] = ca;
    g_cpu.regs[VM_REG_A2] = VM_TUI_DEFAULT_COLOR;
    g_cpu.regs[VM_REG_A3] = VM_TUI_DEFAULT_COLOR;
    vm_ecall_dispatch(g_sys.ecall_router, &g_cpu, &g_sys);

    /* Mark cell (1, 1) transparent. */
    invoke_syscall(SYS_TUI_TILE_SET_TRANSPARENT, (uint32_t)h, 1, 1);

    /* Blit at (2, 2). Canvas cell (2,2) stays 'B'; (2,3) becomes 'F'. */
    g_cpu.regs[VM_REG_A7] = SYS_TUI_TILE_BLIT;
    g_cpu.regs[VM_REG_A0] = (uint32_t)h;
    g_cpu.regs[VM_REG_A1] = 2;
    g_cpu.regs[VM_REG_A2] = 2;
    vm_ecall_dispatch(g_sys.ecall_router, &g_cpu, &g_sys);

    invoke_syscall(SYS_TUI_PRESENT, 0, 0, 0);
    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);

    size_t n = 0;
    char *out = cap_end(&cap, &n);
    /* Both 'B' and 'F' should be in the output. */
    ASSERT(strchr(out, 'B') != NULL);
    ASSERT(strchr(out, 'F') != NULL);
    free(out);
    fixture_teardown();
}

static void test_tile_other_vm_handle_rejected(void) {
    ASSERT(fixture_init());

    /* VM 0 owns the canvas and creates a tile. */
    g_cpu.vm_id = 0;
    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);
    int32_t h = invoke_syscall(SYS_TUI_TILE_CREATE, 2, 2, 0);
    ASSERT(h > 0);

    /* Switch to VM 1 — that VM can't use VM 0's handle (it doesn't
     * even own the canvas; gets EBUSY first). */
    g_cpu.vm_id = 1;
    int32_t d = invoke_syscall(SYS_TUI_TILE_DIMS, h, 0, 0);
    ASSERT_EQ_INT(-(int32_t)VM_EBUSY, d);

    g_cpu.vm_id = 0;
    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_tile_release_on_vm_unload(void) {
    ASSERT(fixture_init());

    g_cpu.vm_id = 0;
    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);
    /* Create several tiles. */
    int32_t h1 = invoke_syscall(SYS_TUI_TILE_CREATE, 4, 4, 0);
    int32_t h2 = invoke_syscall(SYS_TUI_TILE_CREATE, 4, 4, 0);
    int32_t h3 = invoke_syscall(SYS_TUI_TILE_CREATE, 4, 4, 0);
    ASSERT(h1 > 0 && h2 > 0 && h3 > 0);

    /* Simulate VM 0 dying without proper cleanup. */
    vm_host_tui_release_for_vm(0);

    /* After release, VM 0 starting fresh can create tiles again
     * (slots reclaimed). */
    invoke_syscall(SYS_TUI_INIT, 5, 10, 0);
    int32_t h4 = invoke_syscall(SYS_TUI_TILE_CREATE, 4, 4, 0);
    ASSERT(h4 > 0);
    /* Stale handle should be rejected. */
    int32_t d = invoke_syscall(SYS_TUI_TILE_DIMS, h1, 0, 0);
    ASSERT_EQ_INT(-(int32_t)VM_EBADF, d);

    invoke_syscall(SYS_TUI_SHUTDOWN, 0, 0, 0);
    fixture_teardown();
}

static void test_tile_create_oversize_rejected(void) {
    ASSERT(fixture_init());
    invoke_syscall(SYS_TUI_INIT, 10, 40, 0);

    /* Try to create a tile much larger than canvas → ENOMEM
     * (we cap at VM_TUI_MAX_ROWS/COLS). */
    int32_t h = invoke_syscall(SYS_TUI_TILE_CREATE, 999, 999, 0);
    ASSERT_EQ_INT(-(int32_t)VM_ENOMEM, h);

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
    RUN(test_poll_event_printable_ascii);
    RUN(test_poll_event_enter);
    RUN(test_poll_event_arrow_keys);
    RUN(test_poll_event_sgr_mouse);
    RUN(test_poll_event_sgr_mouse_bare_motion);
    RUN(test_poll_event_function_keys);
    RUN(test_tile_create_returns_handle);
    RUN(test_tile_destroy_invalidates_handle);
    RUN(test_tile_fill_then_blit);
    RUN(test_tile_set_then_blit_with_transparency);
    RUN(test_tile_other_vm_handle_rejected);
    RUN(test_tile_release_on_vm_unload);
    RUN(test_tile_create_oversize_rejected);
    return TEST_SUITE_RESULT();
}
