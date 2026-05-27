/* ============================================================
 *  test_vm_host_platform.c — tests for the platform syscalls.
 *
 *  Each test sets up a small VmSystem with a fake VmCpu that has
 *  one writable data region, then drives the syscall handlers
 *  directly via vm_ecall_dispatch. No actual guest code runs;
 *  the tests assemble syscall args into cpu->regs and read back
 *  the result + any data written into the fake data region.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#define _POSIX_C_SOURCE 200809L

#include "test_runner.h"
#include "test_portable.h"
#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_platform.h"
#include "vm/vm_ecall.h"
#include "vm/vm_core.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define SHARED_BYTES  (32 * 1024)
#define LOCAL_BYTES   (128 * 1024)
#define DATA_BYTES    (8 * 1024)

static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];
static uint8_t g_data[DATA_BYTES];

static VmSystem g_sys;
static VmCpu    g_cpu;

/* Tracking values for the fake realtime source. */
static uint32_t g_fake_secs  = 0;
static uint32_t g_fake_nanos = 0;
static bool     g_fake_realtime_succeeds = true;

static bool fake_realtime(void *ud, uint32_t *s, uint32_t *n) {
    (void)ud;
    if (!g_fake_realtime_succeeds) return false;
    *s = g_fake_secs;
    *n = g_fake_nanos;
    return true;
}

static bool fixture_init(bool with_realtime) {
    /* Wipe global state from any previous test. */
    g_fake_secs = 0;
    g_fake_nanos = 0;
    g_fake_realtime_succeeds = true;

    memset(g_data, 0, sizeof(g_data));

    VmSystemConfig cfg = {
        .shared_storage      = g_shared,
        .shared_storage_size = SHARED_BYTES,
        .local_storage       = g_local,
        .local_storage_size  = LOCAL_BYTES,
        .max_vms             = 1,
        .spawn_data_kb       = 8,
    };
    if (!vm_system_init(&g_sys, &cfg)) return false;

    VmHostPlatformConfig pc = {0};
    if (with_realtime) {
        pc.realtime_source = fake_realtime;
        pc.rand_seed       = 0xDEADBEEFCAFEBABEULL;
    } else {
        pc.realtime_source = NULL;
        pc.rand_seed       = 0xDEADBEEFCAFEBABEULL;
    }
    if (!vm_host_install_platform(&g_sys, &pc)) return false;

    /* Fake VmCpu with one writable data region at base 0x80000000.
     * vm_id must be < VM_SCHED_MAX_VMS (64) — handle_alloc indexes
     * alloc_tracking[vm_id]. */
    vm_init(&g_cpu, 0);
    g_cpu.regions[2].base     = g_data;
    g_cpu.regions[2].length   = sizeof(g_data);
    g_cpu.regions[2].writable = true;
    /* Shared region (0xC0000000+) — points at the shared slab so
     * SYS_ALLOC results can be translated back via vm_translate_read. */
    g_cpu.regions[3].base     = g_shared;
    g_cpu.regions[3].length   = sizeof(g_shared);
    g_cpu.regions[3].writable = true;
    return true;
}

static void fixture_teardown(void) {
    vm_system_destroy(&g_sys);
}

/* Drop a string into the fake data region at offset `off` and
 * return its guest address. */
static uint32_t put_string(const char *s, uint32_t off) {
    size_t n = strlen(s) + 1;
    memcpy(g_data + off, s, n);
    return 0x80000000 + off;
}

/* Drop a u32 array (host endian) into the data region. */
static uint32_t put_u32_array(const uint32_t *arr, unsigned n, uint32_t off) {
    memcpy(g_data + off, arr, n * sizeof(uint32_t));
    return 0x80000000 + off;
}

/* Invoke a syscall via vm_ecall_dispatch. */
static int32_t invoke_syscall(uint32_t sys_num,
                              uint32_t a0, uint32_t a1, uint32_t a2,
                              uint32_t a3, uint32_t a4) {
    g_cpu.regs[VM_REG_A7] = sys_num;
    g_cpu.regs[VM_REG_A0] = a0;
    g_cpu.regs[VM_REG_A1] = a1;
    g_cpu.regs[VM_REG_A2] = a2;
    g_cpu.regs[VM_REG_A3] = a3;
    g_cpu.regs[VM_REG_A4] = a4;
    vm_ecall_dispatch(g_sys.ecall_router, &g_cpu, &g_sys);
    return (int32_t)g_cpu.regs[VM_REG_A0];
}

/* ============================================================
 *  Helpers — capture fd writes to a buffer for assertions
 * ============================================================ */

/* Redirect fd to a temp file, run the test body, and return the
 * captured content. Caller frees with free(). */
typedef struct {
    int saved_fd;
    int tmp_fd;
    char path[260];
} FdCapture;

static bool capture_begin(FdCapture *c, int target_fd) {
    /* Flush libc buffers BEFORE redirecting — otherwise the next
     * fflush(stdout) inside capture_end pushes our prior PASS lines
     * into the captured tmp file and they vanish from the test
     * output. */
    fflush(stdout);
    fflush(stderr);
    snprintf(c->path, sizeof c->path,
             "%s/microgarbage_platform_test_XXXXXX", tp_tmpdir());
    c->tmp_fd = mkstemp(c->path);
    if (c->tmp_fd < 0) return false;
    c->saved_fd = dup(target_fd);
    if (c->saved_fd < 0) { close(c->tmp_fd); return false; }
    if (dup2(c->tmp_fd, target_fd) < 0) {
        close(c->saved_fd); close(c->tmp_fd);
        return false;
    }
    return true;
}

static char *capture_end(FdCapture *c, int target_fd, size_t *out_len) {
    fflush(stdout);
    tp_fsync(target_fd);
    dup2(c->saved_fd, target_fd);
    close(c->saved_fd);

    /* Read the temp file. */
    lseek(c->tmp_fd, 0, SEEK_SET);
    char *buf = malloc(8192);
    if (!buf) { close(c->tmp_fd); unlink(c->path); return NULL; }
    ssize_t n = read(c->tmp_fd, buf, 8191);
    if (n < 0) n = 0;
    buf[n] = '\0';
    if (out_len) *out_len = (size_t)n;
    close(c->tmp_fd);
    unlink(c->path);
    return buf;
}

/* ============================================================
 *  SYS_FORMAT_TO_BUF tests (easier — no fd capture needed)
 * ============================================================ */

static void test_format_to_buf_literal(void) {
    ASSERT(fixture_init(false));
    uint32_t buf  = 0x80000000 + 0;
    uint32_t fmt  = put_string("hello world", 256);
    uint32_t args = 0;   /* no args needed for literal */
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 64, fmt, args, 0);
    ASSERT_EQ_INT(11, r);
    ASSERT_EQ_STR("hello world", (const char *)g_data);
    fixture_teardown();
}

static void test_format_to_buf_int(void) {
    ASSERT(fixture_init(false));
    uint32_t buf  = 0x80000000 + 0;
    uint32_t fmt  = put_string("count=%d", 256);
    uint32_t args_arr[] = { 42 };
    uint32_t args = put_u32_array(args_arr, 1, 512);
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 64, fmt, args, 1);
    ASSERT_EQ_INT(8, r);
    ASSERT_EQ_STR("count=42", (const char *)g_data);
    fixture_teardown();
}

static void test_format_to_buf_negative_int(void) {
    ASSERT(fixture_init(false));
    uint32_t buf  = 0x80000000 + 0;
    uint32_t fmt  = put_string("v=%d", 256);
    uint32_t args_arr[] = { (uint32_t)-17 };
    uint32_t args = put_u32_array(args_arr, 1, 512);
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 64, fmt, args, 1);
    ASSERT_EQ_INT(5, r);
    ASSERT_EQ_STR("v=-17", (const char *)g_data);
    fixture_teardown();
}

static void test_format_to_buf_hex(void) {
    ASSERT(fixture_init(false));
    uint32_t buf  = 0x80000000 + 0;
    uint32_t fmt  = put_string("0x%08x", 256);
    uint32_t args_arr[] = { 0xdeadbeef };
    uint32_t args = put_u32_array(args_arr, 1, 512);
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 64, fmt, args, 1);
    ASSERT_EQ_INT(10, r);
    ASSERT_EQ_STR("0xdeadbeef", (const char *)g_data);
    fixture_teardown();
}

static void test_format_to_buf_string(void) {
    ASSERT(fixture_init(false));
    uint32_t buf      = 0x80000000 + 0;
    uint32_t fmt      = put_string("hello %s!", 256);
    uint32_t the_name = put_string("world", 512);
    uint32_t args_arr[] = { the_name };
    uint32_t args = put_u32_array(args_arr, 1, 1024);
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 64, fmt, args, 1);
    ASSERT_EQ_INT(12, r);
    ASSERT_EQ_STR("hello world!", (const char *)g_data);
    fixture_teardown();
}

static void test_format_to_buf_mixed(void) {
    ASSERT(fixture_init(false));
    uint32_t buf      = 0x80000000 + 0;
    uint32_t fmt      = put_string("[%s] %d frames @ %u fps", 256);
    uint32_t game     = put_string("snake", 512);
    uint32_t args_arr[] = { game, 20, 8 };
    uint32_t args = put_u32_array(args_arr, 3, 1024);
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 64, fmt, args, 3);
    ASSERT_EQ_INT(25, r);
    ASSERT_EQ_STR("[snake] 20 frames @ 8 fps", (const char *)g_data);
    fixture_teardown();
}

static void test_format_to_buf_truncated(void) {
    ASSERT(fixture_init(false));
    /* Cap small to force truncation. */
    uint32_t buf  = 0x80000000 + 0;
    uint32_t fmt  = put_string("abcdefghij", 256);
    /* cap=5 means 4 chars of content + null */
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 5, fmt, 0, 0);
    /* The return value should be the count the formatter WOULD have
     * written (10), not the truncated count. */
    ASSERT_EQ_INT(10, r);
    /* Buffer should be "abcd" + null. */
    ASSERT_EQ_STR("abcd", (const char *)g_data);
    fixture_teardown();
}

static void test_format_to_buf_percent(void) {
    ASSERT(fixture_init(false));
    uint32_t buf  = 0x80000000 + 0;
    uint32_t fmt  = put_string("100%% complete", 256);
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 64, fmt, 0, 0);
    ASSERT_EQ_INT(13, r);
    ASSERT_EQ_STR("100% complete", (const char *)g_data);
    fixture_teardown();
}

static void test_format_to_buf_width_left(void) {
    ASSERT(fixture_init(false));
    uint32_t buf  = 0x80000000 + 0;
    uint32_t fmt  = put_string("[%-6d]", 256);
    uint32_t args_arr[] = { 42 };
    uint32_t args = put_u32_array(args_arr, 1, 512);
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 64, fmt, args, 1);
    ASSERT_EQ_INT(8, r);
    ASSERT_EQ_STR("[42    ]", (const char *)g_data);
    fixture_teardown();
}

static void test_format_to_buf_zero_pad(void) {
    ASSERT(fixture_init(false));
    uint32_t buf  = 0x80000000 + 0;
    uint32_t fmt  = put_string("%05d", 256);
    uint32_t args_arr[] = { 42 };
    uint32_t args = put_u32_array(args_arr, 1, 512);
    int32_t r = invoke_syscall(SYS_FORMAT_TO_BUF, buf, 64, fmt, args, 1);
    ASSERT_EQ_INT(5, r);
    ASSERT_EQ_STR("00042", (const char *)g_data);
    fixture_teardown();
}

/* ============================================================
 *  SYS_FORMAT_AND_WRITE tests (capture stdout)
 * ============================================================ */

static void test_format_and_write_basic(void) {
    ASSERT(fixture_init(false));
    FdCapture cap;
    ASSERT(capture_begin(&cap, 1));

    uint32_t fmt = put_string("score=%d\n", 256);
    uint32_t args_arr[] = { 100 };
    uint32_t args = put_u32_array(args_arr, 1, 512);
    int32_t r = invoke_syscall(SYS_FORMAT_AND_WRITE, 1, fmt, args, 1, 0);

    size_t len = 0;
    char *out = capture_end(&cap, 1, &len);
    ASSERT_EQ_INT(10, r);
    ASSERT_EQ_STR("score=100\n", out);
    free(out);
    fixture_teardown();
}

static void test_format_and_write_rejects_bad_fd(void) {
    ASSERT(fixture_init(false));
    uint32_t fmt = put_string("foo", 256);
    int32_t r = invoke_syscall(SYS_FORMAT_AND_WRITE, 5, fmt, 0, 0, 0);
    ASSERT_EQ_INT(-(int32_t)VM_EBADF, r);
    fixture_teardown();
}

/* ============================================================
 *  SYS_REALTIME_NOW
 * ============================================================ */

static void test_realtime_now_returns_data(void) {
    ASSERT(fixture_init(true));
    g_fake_secs  = 1700000000;
    g_fake_nanos = 123456789;

    uint32_t out = 0x80000000 + 0;
    int32_t r = invoke_syscall(SYS_REALTIME_NOW, out, 0, 0, 0, 0);
    ASSERT_EQ_INT(0, r);

    VmRealtimeRecord *rec = (VmRealtimeRecord *)g_data;
    ASSERT_EQ_INT(1, (int)rec->version);
    ASSERT_EQ_INT(1700000000, (int)rec->seconds);
    ASSERT_EQ_INT(123456789, (int)rec->nanos);
    fixture_teardown();
}

static void test_realtime_now_no_source(void) {
    ASSERT(fixture_init(false));   /* no realtime source */
    uint32_t out = 0x80000000 + 0;
    int32_t r = invoke_syscall(SYS_REALTIME_NOW, out, 0, 0, 0, 0);
    ASSERT_EQ_INT(-(int32_t)VM_ENOSYS, r);
    fixture_teardown();
}

static void test_realtime_now_source_fails(void) {
    ASSERT(fixture_init(true));
    g_fake_realtime_succeeds = false;
    uint32_t out = 0x80000000 + 0;
    int32_t r = invoke_syscall(SYS_REALTIME_NOW, out, 0, 0, 0, 0);
    ASSERT_EQ_INT(-(int32_t)VM_ENOSYS, r);
    fixture_teardown();
}

/* ============================================================
 *  SYS_RAND
 * ============================================================ */

static void test_rand_is_deterministic_with_seed(void) {
    ASSERT(fixture_init(true));   /* with_realtime sets seed=0xDEADBEEFCAFEBABE */
    uint32_t r1 = (uint32_t)invoke_syscall(SYS_RAND, 0, 0, 0, 0, 0);
    uint32_t r2 = (uint32_t)invoke_syscall(SYS_RAND, 0, 0, 0, 0, 0);
    uint32_t r3 = (uint32_t)invoke_syscall(SYS_RAND, 0, 0, 0, 0, 0);
    /* Re-init to same seed; should produce same sequence. */
    fixture_teardown();
    ASSERT(fixture_init(true));
    uint32_t s1 = (uint32_t)invoke_syscall(SYS_RAND, 0, 0, 0, 0, 0);
    uint32_t s2 = (uint32_t)invoke_syscall(SYS_RAND, 0, 0, 0, 0, 0);
    uint32_t s3 = (uint32_t)invoke_syscall(SYS_RAND, 0, 0, 0, 0, 0);
    ASSERT_EQ_INT((int)r1, (int)s1);
    ASSERT_EQ_INT((int)r2, (int)s2);
    ASSERT_EQ_INT((int)r3, (int)s3);
    /* And the values differ from each other. */
    ASSERT(r1 != r2);
    ASSERT(r2 != r3);
    fixture_teardown();
}

/* ============================================================
 *  SYS_TIMING_DEADLINE_REMAINING
 * ============================================================ */

static void test_timing_deadline_no_period(void) {
    ASSERT(fixture_init(false));
    /* reload_period is 0 (init); deadline_remaining should be 0. */
    int32_t r = invoke_syscall(SYS_TIMING_DEADLINE_REMAINING, 0, 0, 0, 0, 0);
    ASSERT_EQ_INT(0, r);
    fixture_teardown();
}

static void test_timing_deadline_with_period(void) {
    ASSERT(fixture_init(false));
    /* Simulate a guest mid-frame: deadline 200, now 150 → 50 remaining. */
    g_cpu.reload_period          = 125;
    g_cpu.reload_next_deadline   = 200;
    g_sys.sched->global_tick      = 150;
    int32_t r = invoke_syscall(SYS_TIMING_DEADLINE_REMAINING, 0, 0, 0, 0, 0);
    ASSERT_EQ_INT(50, r);
    /* If we've passed the deadline, returns 0 (clamped). */
    g_sys.sched->global_tick = 250;
    r = invoke_syscall(SYS_TIMING_DEADLINE_REMAINING, 0, 0, 0, 0, 0);
    ASSERT_EQ_INT(0, r);
    fixture_teardown();
}

/* ============================================================
 *  SYS_ALLOC_SIZE
 * ============================================================ */

static void test_alloc_size_returns_block_size(void) {
    ASSERT(fixture_init(false));
    /* Call SYS_ALLOC for 100 bytes; expect a block size that's a
     * power-of-2 bin (likely 128). Then SYS_ALLOC_SIZE on it should
     * return that bin size.
     *
     * Note: SYS_ALLOC returns a guest address in the shared-region
     * range (0xC0000000+), which when interpreted as int32_t is
     * negative. So we can't use `>0`; we just check it's not an
     * error code. Errors are -EINVAL etc., small negative values;
     * 0xC0000000 is far from that. We check "not in the -4096..0
     * range" as a proxy for "not an errno". */
    uint32_t ptr = (uint32_t)invoke_syscall(SYS_ALLOC, 100, 0, 0, 0, 0);
    ASSERT(ptr >= 0xC0000000u);   /* shared-region range */
    int32_t sz = invoke_syscall(SYS_ALLOC_SIZE, ptr, 0, 0, 0, 0);
    /* 100 bytes → bin 2 (= 128B). */
    ASSERT_EQ_INT(128, sz);
    /* Free. */
    invoke_syscall(SYS_FREE, ptr, 0, 0, 0, 0);
    fixture_teardown();
}

static void test_alloc_size_null_pointer(void) {
    ASSERT(fixture_init(false));
    int32_t r = invoke_syscall(SYS_ALLOC_SIZE, 0, 0, 0, 0, 0);
    ASSERT_EQ_INT(-(int32_t)VM_EINVAL, r);
    fixture_teardown();
}

int main(void) {
    TEST_SUITE("vm_host_platform");

    RUN(test_format_to_buf_literal);
    RUN(test_format_to_buf_int);
    RUN(test_format_to_buf_negative_int);
    RUN(test_format_to_buf_hex);
    RUN(test_format_to_buf_string);
    RUN(test_format_to_buf_mixed);
    RUN(test_format_to_buf_truncated);
    RUN(test_format_to_buf_percent);
    RUN(test_format_to_buf_width_left);
    RUN(test_format_to_buf_zero_pad);

    RUN(test_format_and_write_basic);
    RUN(test_format_and_write_rejects_bad_fd);

    RUN(test_realtime_now_returns_data);
    RUN(test_realtime_now_no_source);
    RUN(test_realtime_now_source_fails);

    RUN(test_rand_is_deterministic_with_seed);

    RUN(test_timing_deadline_no_period);
    RUN(test_timing_deadline_with_period);

    RUN(test_alloc_size_returns_block_size);
    RUN(test_alloc_size_null_pointer);

    return TEST_SUITE_RESULT();
}
