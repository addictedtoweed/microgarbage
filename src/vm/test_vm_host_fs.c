/* test_vm_host_fs.c — exercise the file syscall handlers
 * against a host-side FatFs volume backed by a trashdrive.
 *
 * Compiles in two modes:
 *
 *   - Without HAVE_FATFS: tiny stub that prints "SKIP" and exits.
 *     Same pattern as test_trashdrive_fatfs.c.
 *
 *   - With -DHAVE_FATFS plus -Ithird_party/fatfs/source and
 *     -Ithird_party/fatfs: runs real tests by:
 *       1. Setting up a TrashDrive + FatFs mount on the host
 *       2. Setting up a VmSystem with stdio + fs installed
 *       3. Synthesizing ECALL invocations by directly calling
 *          vm_ecall_dispatch with controlled CPU state, and
 *          inspecting the resulting registers
 *
 * Build (HAVE_FATFS mode):
 *   cc -Wall -Wextra -Wpedantic -std=c11 -O2 \
 *      -Iinclude -DHAVE_FATFS \
 *      -Ithird_party/fatfs/source -Ithird_party/fatfs \
 *      -o test_vm_host_fs \
 *      src/vm/test_vm_host_fs.c \
 *      src/vm/vm_host_fs.c \
 *      src/storage/trashdrive_fatfs.c src/storage/trashdrive.c \
 *      src/vm/vm_host_stdio.c src/vm/vm_system.c src/vm/vm_sched.c \
 *      src/vm/vm_ecall.c src/vm/vm_ecall_handlers.c \
 *      src/vm/vm_mailbox.c src/vm/vm_loader.c src/vm/vm_core.c \
 *      src/memory/slab_stack.c src/memory/bump.c \
 *      src/containers/fifo_queue.c src/containers/ring_buffer.c \
 *      third_party/fatfs/ff_wrapped.c third_party/fatfs/source/ffsystem.c
 *
 * Public domain (CC0). No warranty.
 */

#include "test_runner.h"

#ifndef HAVE_FATFS

#include <stdio.h>
int main(void) {
    (void)tr_passed_;
    (void)tr_failed_;
    (void)tr_current_failed_;
    (void)tr_suite_name_;
    (void)tr_current_test_name_;
    TEST_SUITE("vm_host_fs");
    printf("  SKIP  FatFs not built in (compile with -DHAVE_FATFS and "
           "-Ithird_party/fatfs/source -Ithird_party/fatfs)\n");
    printf("0 passed, 0 failed (skipped)\n");
    return 0;
}

#else  /* HAVE_FATFS */

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_fs.h"
#include "vm/vm_ecall.h"
#include "vm/vm_core.h"
#include "storage/trashdrive.h"
#include "storage/trashdrive_fatfs.h"
#include "ff.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>     /* mkdir */
#include <unistd.h>       /* rmdir */

/* ============================================================
 *  Test fixture
 *
 *  Each test runs against a fresh-formatted 64 KB trashdrive
 *  volume mounted as FatFs drive 0. The VmSystem has just enough
 *  setup to exercise the syscall router; we don't actually run
 *  guest code — we drive the handlers via vm_ecall_dispatch
 *  directly.
 * ============================================================ */

#define SHARED_BYTES (32 * 1024)
/* Sized for max_vms=2 spawn_data_kb=16 in fixture_init below
 * (vm_system_local_required reports ~211 KB; we round to 256). */
#define LOCAL_BYTES  (256 * 1024)
/* 128 KB — needs to be at least ~96 KB for FatFs R0.16 f_mkfs (see
 * test_trashdrive_fatfs.c for the same rationale). */
#define POOL_BYTES   (128 * 1024)
#define DATA_BYTES   (4 * 1024)   /* fake guest "memory" for path strings etc. */

static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];
static uint8_t g_pool[POOL_BYTES];
static uint8_t g_data[DATA_BYTES];   /* fake guest data region */

static VmSystem g_sys;
static TrashDrive g_drive;
static FATFS g_fs;
static VmCpu g_cpu;        /* fake "guest" CPU used to invoke syscalls */

/* Set up the full stack: trashdrive + FatFs mount + VmSystem +
 * fs syscalls registered. Returns true on success. */
static bool fixture_init(void) {
    /* 1. Wipe the trashdrive backing and any prior FatFs state. */
    memset(g_pool, 0, sizeof(g_pool));
    f_mount(NULL, "0:", 0);                  /* unmount any prior */
    for (uint8_t i = 0; i < TRASH_FATFS_MAX_VOLUMES; i++) {
        trash_fatfs_register(i, NULL);
    }
    vm_host_fs_reset();                       /* close any leaked fds */

    /* 2. Block device. */
    if (trash_init(&g_drive, g_pool, sizeof(g_pool)) != TRASH_OK) return false;
    if (!trash_fatfs_register(0, &g_drive)) return false;

    /* 3. FatFs format and mount. */
    BYTE work[FF_MAX_SS];
    MKFS_PARM opt = {0};
    opt.fmt = FM_FAT;
    opt.n_fat = 1;
    if (f_mkfs("0:", &opt, work, sizeof(work)) != FR_OK) return false;
    if (f_mount(&g_fs, "0:", 1) != FR_OK) return false;

    /* 4. VmSystem. */
    VmSystemConfig cfg = {
        .shared_storage      = g_shared,
        .shared_storage_size = SHARED_BYTES,
        .local_storage       = g_local,
        .local_storage_size  = LOCAL_BYTES,
        /* Small VM count — these tests load at most a guest VM via
         * the spawn handler. 1-2 slots cover everything. */
        .max_vms             = 2,
        .spawn_data_kb       = 16,
    };
    if (!vm_system_init(&g_sys, &cfg)) return false;
    if (!vm_host_install_stdio(&g_sys)) return false;
    if (!vm_host_install_fs(&g_sys)) return false;

    /* Register the freshly-mounted FatFs as /td0. */
    if (!vm_host_fs_mount_fatfs("td0", 0, &g_fs)) return false;

    /* 5. Fake VmCpu with one writable data region so handlers
     * can translate guest pointers. We put it at region 2 (DATA),
     * which is the default writable region. */
    vm_init(&g_cpu, 99);
    g_cpu.regions[2].base       = g_data;
    g_cpu.regions[2].length     = sizeof(g_data);
    g_cpu.regions[2].writable   = true;
    /* region 0 (CODE) and 1 (RODATA) are left unset — we don't
     * read from them in these tests. */
    return true;
}

static void fixture_teardown(void) {
    vm_host_fs_reset();
    f_mount(NULL, "0:", 0);
    trash_fatfs_register(0, NULL);
    vm_system_destroy(&g_sys);
}

/* Write a guest-visible string into the data region and return
 * its guest address. */
static uint32_t put_string(const char *s, uint32_t offset) {
    size_t n = strlen(s) + 1;
    memcpy(g_data + offset, s, n);
    return 0x80000000 + offset;   /* region 2 = DATA, base addr */
}

/* Invoke a syscall by setting a0..a3 + a7 and calling dispatch.
 * Returns the value in a0 after the call. */
static int32_t invoke_syscall(uint32_t sys_num,
                              uint32_t a0, uint32_t a1,
                              uint32_t a2, uint32_t a3) {
    g_cpu.regs[VM_REG_A0] = a0;
    g_cpu.regs[VM_REG_A1] = a1;
    g_cpu.regs[VM_REG_A2] = a2;
    g_cpu.regs[VM_REG_A3] = a3;
    g_cpu.regs[VM_REG_A7] = sys_num;
    vm_ecall_dispatch(g_sys.ecall_router, &g_cpu, &g_sys);
    return (int32_t)g_cpu.regs[VM_REG_A0];
}

/* ============================================================
 *  Tests
 * ============================================================ */

static void test_openat_create_writes_and_reads_back(void) {
    ASSERT(fixture_init());

    uint32_t path = put_string("/td0/test.txt", 0);
    uint32_t data = put_string("hello, file!", 256);
    uint32_t buf  = 0x80000000 + 512;

    /* open with O_RDWR | O_CREAT */
    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                                 VM_O_RDWR | VM_O_CREAT, 0);
    ASSERT(fd >= 3);   /* file fd, not stdin/stdout/stderr */

    /* write */
    int32_t written = invoke_syscall(SYS_WRITE, (uint32_t)fd, data, 12, 0);
    ASSERT_EQ_INT(12, written);

    /* lseek to start */
    int32_t pos = invoke_syscall(SYS_LSEEK, (uint32_t)fd, 0, VM_SEEK_SET, 0);
    ASSERT_EQ_INT(0, pos);

    /* read back */
    int32_t bytes_read = invoke_syscall(SYS_READ, (uint32_t)fd, buf, 64, 0);
    ASSERT_EQ_INT(12, bytes_read);
    ASSERT_EQ_INT(0, memcmp(g_data + 512, "hello, file!", 12));

    /* close */
    int32_t cr = invoke_syscall(SYS_CLOSE, (uint32_t)fd, 0, 0, 0);
    ASSERT_EQ_INT(0, cr);

    fixture_teardown();
}

static void test_openat_nonexistent_returns_enoent(void) {
    ASSERT(fixture_init());

    uint32_t path = put_string("/td0/nope.txt", 0);
    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path, VM_O_RDONLY, 0);
    ASSERT_EQ_INT(-VM_ENOENT, fd);

    fixture_teardown();
}

static void test_openat_wrong_dirfd_rejected(void) {
    ASSERT(fixture_init());

    uint32_t path = put_string("/td0/some.txt", 0);
    /* Pass dirfd != AT_FDCWD; should be rejected with EINVAL. */
    int32_t r = invoke_syscall(SYS_OPENAT, 5, path,
                                VM_O_RDONLY | VM_O_CREAT, 0);
    ASSERT_EQ_INT(-VM_EINVAL, r);

    fixture_teardown();
}

static void test_mkdirat_and_readdir(void) {
    ASSERT(fixture_init());

    /* Create two directories. */
    uint32_t p1 = put_string("/td0/d1", 0);
    uint32_t p2 = put_string("/td0/d2", 64);
    ASSERT_EQ_INT(0, invoke_syscall(SYS_MKDIRAT, VM_AT_FDCWD, p1, 0, 0));
    ASSERT_EQ_INT(0, invoke_syscall(SYS_MKDIRAT, VM_AT_FDCWD, p2, 0, 0));

    /* Open root of the FatFs mount for readdir. */
    uint32_t root = put_string("/td0", 128);
    int32_t dfd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, root,
                                  VM_O_RDONLY | VM_O_DIRECTORY, 0);
    ASSERT(dfd >= 3);

    /* Read entries; we should find D1 and D2. */
    uint32_t dirent_p = 0x80000000 + 256;
    int saw_d1 = 0, saw_d2 = 0;
    for (int i = 0; i < 32; i++) {
        int32_t r = invoke_syscall(SYS_READDIR, (uint32_t)dfd, dirent_p, 0, 0);
        if (r == 1) break;                /* end of directory */
        ASSERT_EQ_INT(0, r);
        VmDirent *de = (VmDirent *)(g_data + 256);
        if (strcmp(de->name, "D1") == 0) { saw_d1 = 1; ASSERT_EQ_INT((int)VM_DT_DIR, (int)de->type); }
        if (strcmp(de->name, "D2") == 0) { saw_d2 = 1; ASSERT_EQ_INT((int)VM_DT_DIR, (int)de->type); }
    }
    ASSERT(saw_d1);
    ASSERT(saw_d2);

    invoke_syscall(SYS_CLOSE, (uint32_t)dfd, 0, 0, 0);
    fixture_teardown();
}

static void test_unlinkat_removes_file(void) {
    ASSERT(fixture_init());

    /* Create then unlink. */
    uint32_t path = put_string("/td0/del.txt", 0);
    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                                 VM_O_WRONLY | VM_O_CREAT, 0);
    ASSERT(fd >= 3);
    invoke_syscall(SYS_CLOSE, (uint32_t)fd, 0, 0, 0);

    int32_t r = invoke_syscall(SYS_UNLINKAT, VM_AT_FDCWD, path, 0, 0);
    ASSERT_EQ_INT(0, r);

    /* Re-open should now fail with ENOENT. */
    fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path, VM_O_RDONLY, 0);
    ASSERT_EQ_INT(-VM_ENOENT, fd);

    fixture_teardown();
}

static void test_lseek_set_cur_end(void) {
    ASSERT(fixture_init());

    uint32_t path = put_string("/td0/seek.dat", 0);
    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                                 VM_O_RDWR | VM_O_CREAT, 0);
    ASSERT(fd >= 3);

    /* Write 100 bytes. */
    memset(g_data + 256, 'A', 100);
    int32_t bw = invoke_syscall(SYS_WRITE, (uint32_t)fd,
                                 0x80000000 + 256, 100, 0);
    ASSERT_EQ_INT(100, bw);

    /* SEEK_SET to 25. */
    int32_t pos = invoke_syscall(SYS_LSEEK, (uint32_t)fd, 25, VM_SEEK_SET, 0);
    ASSERT_EQ_INT(25, pos);

    /* SEEK_CUR +10 → 35. */
    pos = invoke_syscall(SYS_LSEEK, (uint32_t)fd, 10, VM_SEEK_CUR, 0);
    ASSERT_EQ_INT(35, pos);

    /* SEEK_END +0 → 100. */
    pos = invoke_syscall(SYS_LSEEK, (uint32_t)fd, 0, VM_SEEK_END, 0);
    ASSERT_EQ_INT(100, pos);

    /* SEEK_SET with negative offset is rejected as EINVAL. */
    pos = invoke_syscall(SYS_LSEEK, (uint32_t)fd, (uint32_t)-5, VM_SEEK_SET, 0);
    ASSERT_EQ_INT(-VM_EINVAL, pos);

    invoke_syscall(SYS_CLOSE, (uint32_t)fd, 0, 0, 0);
    fixture_teardown();
}

static void test_open_fd_limit(void) {
    ASSERT(fixture_init());

    /* Open up to the limit, then verify the next open fails. */
    int fds[VM_HOST_FS_MAX_FILES + 1];
    unsigned i;
    char namebuf[24];
    for (i = 0; i < VM_HOST_FS_MAX_FILES; i++) {
        snprintf(namebuf, sizeof(namebuf), "/td0/f%u", i);
        uint32_t path = put_string(namebuf, i * 24);
        int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                                     VM_O_WRONLY | VM_O_CREAT, 0);
        if (fd < 0) {
            printf("    [open %u failed: %d]\n", i, fd);
            FAIL("could not open up to limit");
            fixture_teardown();
            return;
        }
        fds[i] = fd;
    }
    /* One more should fail with EMFILE. */
    snprintf(namebuf, sizeof(namebuf), "/td0/overflow");
    uint32_t path = put_string(namebuf, i * 24);
    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                                 VM_O_WRONLY | VM_O_CREAT, 0);
    ASSERT_EQ_INT(-VM_EMFILE, fd);

    /* Close one and try again. */
    invoke_syscall(SYS_CLOSE, (uint32_t)fds[0], 0, 0, 0);
    fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                        VM_O_WRONLY | VM_O_CREAT, 0);
    ASSERT(fd >= 3);

    /* Clean up all fds. */
    for (unsigned j = 1; j < VM_HOST_FS_MAX_FILES; j++) {
        invoke_syscall(SYS_CLOSE, (uint32_t)fds[j], 0, 0, 0);
    }
    invoke_syscall(SYS_CLOSE, (uint32_t)fd, 0, 0, 0);

    fixture_teardown();
}

/* After the M.3 refactor, the only valid absolute path shape is
 * /<name>/.... Bare paths like "/foo" and FatFs's native
 * "0:/foo" form must be rejected. This test pins that down. */
static void test_path_translation_rejects_bare_paths(void) {
    ASSERT(fixture_init());

    /* Bare absolute path */
    uint32_t p1 = put_string("/explicit", 0);
    int32_t r1 = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, p1,
                                 VM_O_WRONLY | VM_O_CREAT, 0);
    ASSERT_EQ_INT(-(int)VM_ENOENT, (int)r1);

    /* FatFs-native volume prefix — also rejected now */
    uint32_t p2 = put_string("0:/explicit", 64);
    int32_t r2 = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, p2,
                                 VM_O_WRONLY | VM_O_CREAT, 0);
    ASSERT_EQ_INT(-(int)VM_ENOENT, (int)r2);

    /* The legacy /host/ prefix is also gone */
    uint32_t p3 = put_string("/host/foo.txt", 128);
    int32_t r3 = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, p3,
                                 VM_O_RDONLY, 0);
    ASSERT_EQ_INT(-(int)VM_ENOENT, (int)r3);

    /* Unknown mount name — also ENOENT */
    uint32_t p4 = put_string("/nope/foo.txt", 192);
    int32_t r4 = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, p4,
                                 VM_O_RDONLY, 0);
    ASSERT_EQ_INT(-(int)VM_ENOENT, (int)r4);

    /* And one positive control — /td0/ DOES work */
    uint32_t p5 = put_string("/td0/works.txt", 256);
    int32_t r5 = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, p5,
                                 VM_O_WRONLY | VM_O_CREAT, 0);
    ASSERT(r5 >= 3);
    invoke_syscall(SYS_CLOSE, (uint32_t)r5, 0, 0, 0);

    fixture_teardown();
}

/* Locate a pre-built guest_minimal.elf. Returns NULL if not
 * found anywhere we know to look — the test that uses this
 * will SKIP. The minimal ELF is a 3-instruction RV32IMC program
 * that does sys_exit(0); it lives in examples/01_hello/. */
static const char *locate_minimal_elf(void) {
    /* 1. Environment override. Useful for CI where the test binary
     *    might run from a build directory removed from the source. */
    const char *env = getenv("VM_TEST_MINIMAL_ELF");
    if (env) {
        FILE *f = fopen(env, "rb");
        if (f) { fclose(f); return env; }
    }
    /* 2. Standard checked-in path, run from repo root. */
    static const char *candidates[] = {
        "examples/01_hello/build/guest_minimal.elf",
        "../examples/01_hello/build/guest_minimal.elf",
        "../../examples/01_hello/build/guest_minimal.elf",
    };
    for (size_t i = 0; i < sizeof(candidates) / sizeof(*candidates); i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (f) { fclose(f); return candidates[i]; }
    }
    return NULL;
}

/* Read a host file into a freshly malloc'd buffer. Caller frees. */
static uint8_t *slurp(const char *path, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    rewind(f);
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        free(buf); fclose(f); return NULL;
    }
    fclose(f);
    *out_size = (size_t)sz;
    return buf;
}

/* SYS_SPAWN_AND_WAIT: write guest_minimal.elf into the FatFs
 * volume, spawn it via the syscall, verify return value is 0
 * (the minimal program is sys_exit(0)).
 *
 * Round V: spawn is ASYNCHRONOUS. The syscall parks the parent
 * (BLOCK_ON_CHILD) and returns; the child runs under the scheduler
 * and, when it halts, vm_system_reap_halted_children delivers the
 * exit code to the parent's a0 and wakes it. So this test must:
 *   1. register the fake parent as a real scheduler VM (so the
 *      reap can find it to wake it),
 *   2. invoke the spawn syscall,
 *   3. confirm the parent parked on a child,
 *   4. step the system until the parent wakes,
 *   5. read the delivered exit code from a0.
 */
static void test_spawn_and_wait_minimal_elf(void) {
    const char *elf_path = locate_minimal_elf();
    if (!elf_path) {
        printf("  SKIP  guest_minimal.elf not found "
               "(set VM_TEST_MINIMAL_ELF or build examples/01_hello)\n");
        return;
    }

    size_t elf_size;
    uint8_t *elf = slurp(elf_path, &elf_size);
    ASSERT(elf != NULL);

    ASSERT(fixture_init());

    /* Register the fake parent as a REAL scheduler VM so the async
     * machinery (block transition, reap, wake) operates exactly as
     * in production. g_cpu has a writable DATA region but no real
     * CODE; that's fine because once it parks on the child we clear
     * its ready bit by hand (mirroring the block transition that
     * vm_sched_step applies after a real spawn ecall, which the
     * test's direct invoke_syscall dispatch bypasses) — so the
     * scheduler never tries to fetch an instruction from it. */
    ASSERT(vm_sched_register_at(g_sys.sched, &g_cpu, 0) == 0);
    g_sys.vms[0] = &g_cpu;

    /* Write the ELF into the FatFs volume at /spawn.elf. */
    FIL ff;
    ASSERT(f_open(&ff, "0:/spawn.elf", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    UINT bw;
    ASSERT(f_write(&ff, elf, (UINT)elf_size, &bw) == FR_OK);
    ASSERT(bw == elf_size);
    ASSERT(f_close(&ff) == FR_OK);
    free(elf);

    /* Spawn it. The async handler loads the child, parks g_cpu
     * (BLOCK_ON_CHILD), and returns. */
    uint32_t path = put_string("/td0/spawn.elf", 0);
    (void)invoke_syscall(SYS_SPAWN_AND_WAIT, path, 0, 0, 0);

    /* Parent parked on a child. */
    ASSERT_EQ_INT(g_cpu.block_reason, BLOCK_ON_CHILD);
    ASSERT(g_cpu.block_child_vm != 0xffff);

    /* Apply the block transition the scheduler would have applied
     * after a real spawn ecall (invoke_syscall's direct dispatch
     * skips it): parent out of ready, into blocked, so wake_child's
     * bm_test(blocked) precondition holds and the parent is never
     * stepped. */
    g_sys.sched->ready   &= ~(1ull << 0);
    g_sys.sched->blocked |=  (1ull << 0);

    /* Step until the parent is woken (child halted + reaped), with a
     * sane cap to guard against a hang. */
    int guard = 100000;
    while (g_cpu.block_reason == BLOCK_ON_CHILD && guard-- > 0) {
        vm_system_step(&g_sys);
    }
    ASSERT(guard > 0);                          /* didn't time out */
    ASSERT_EQ_INT(g_cpu.block_reason, BLOCK_NONE);

    /* guest_minimal calls sys_exit(0); exit code masked to 8 bits. */
    ASSERT_EQ_INT((int32_t)g_cpu.regs[VM_REG_A0], 0);

    fixture_teardown();
}

/* The vm_host_fs_route_* functions are the entry point for hosts
 * that REPLACE vm_host_stdio with their own SYS_READ/SYS_WRITE
 * handlers (e.g., the 05_shell pipe transport on Cygwin). They
 * must accept fd values that came back from a SYS_OPENAT ecall
 * and read/write the file in those slots.
 *
 * This test does the open via the syscall (so the fd is allocated
 * the same way the guest would see it), then calls the route_*
 * functions directly with a host-side buffer — proving the
 * routing path doesn't go through stdio at all. */
static void test_route_functions_handle_file_fds(void) {
    ASSERT(fixture_init());

    /* Create a file and write some content via the syscall path. */
    uint32_t path = put_string("/td0/routed.txt", 0);
    uint32_t data = put_string("from the guest", 256);

    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                                 VM_O_RDWR | VM_O_CREAT, 0);
    ASSERT(fd >= 3);

    int32_t written = invoke_syscall(SYS_WRITE, (uint32_t)fd, data, 14, 0);
    ASSERT_EQ_INT(14, written);

    /* lseek to start. */
    int32_t pos = invoke_syscall(SYS_LSEEK, (uint32_t)fd, 0, VM_SEEK_SET, 0);
    ASSERT_EQ_INT(0, pos);

    /* Read via the public routing function with a HOST buffer. */
    char host_buf[64];
    memset(host_buf, 0, sizeof(host_buf));
    int32_t rr = vm_host_fs_route_read(fd, host_buf, sizeof(host_buf));
    ASSERT_EQ_INT(14, rr);
    ASSERT_EQ_INT(0, memcmp(host_buf, "from the guest", 14));

    /* Write via the public routing function: overwrite the start. */
    pos = invoke_syscall(SYS_LSEEK, (uint32_t)fd, 0, VM_SEEK_SET, 0);
    ASSERT_EQ_INT(0, pos);
    int32_t wr = vm_host_fs_route_write(fd, "FROM THE HOST!", 14);
    ASSERT_EQ_INT(14, wr);

    /* Verify via the syscall path: the host-written bytes are visible. */
    pos = invoke_syscall(SYS_LSEEK, (uint32_t)fd, 0, VM_SEEK_SET, 0);
    ASSERT_EQ_INT(0, pos);
    uint32_t guest_buf = 0x80000000 + 512;
    int32_t br = invoke_syscall(SYS_READ, (uint32_t)fd, guest_buf, 64, 0);
    ASSERT_EQ_INT(14, br);
    ASSERT_EQ_INT(0, memcmp(g_data + 512, "FROM THE HOST!", 14));

    /* Close via the public routing function. */
    int32_t cr = vm_host_fs_route_close(fd);
    ASSERT_EQ_INT(0, cr);

    /* After close, a route_read on the same fd should fail. */
    rr = vm_host_fs_route_read(fd, host_buf, sizeof(host_buf));
    ASSERT(rr < 0);

    fixture_teardown();
}

/* stdio fds (0, 1, 2) are NOT the routing functions' responsibility
 * — they belong to the host's own SYS_READ/SYS_WRITE handlers. The
 * router returns a sentinel (VM_HOST_FS_NOT_OURS) to signal that
 * the caller should handle the fd itself. */
static void test_route_functions_reject_stdio_fds(void) {
    ASSERT(fixture_init());

    char host_buf[8];
    int32_t r0 = vm_host_fs_route_read(0, host_buf, 8);
    ASSERT_EQ_INT(VM_HOST_FS_NOT_OURS, r0);
    int32_t r1 = vm_host_fs_route_write(1, "x", 1);
    ASSERT_EQ_INT(VM_HOST_FS_NOT_OURS, r1);
    int32_t r2 = vm_host_fs_route_close(2);
    ASSERT_EQ_INT(VM_HOST_FS_NOT_OURS, r2);

    fixture_teardown();
}

/* ---------------------------------------------------------------
 *  Mount table API tests (M.3)
 * --------------------------------------------------------------- */

static void test_mount_host_basic(void) {
    ASSERT(fixture_init());

    /* Create a host directory and a file in it. */
    const char *root = "/tmp/microgarbage_mount_test";
    mkdir(root, 0755);
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/hello.txt", root);
        FILE *f = fopen(path, "wb");
        ASSERT_NOT_NULL(f);
        fputs("hi from host", f);
        fclose(f);
    }

    /* Register as /h0 (read-only). */
    ASSERT(vm_host_fs_mount_host("h0", root, false));

    /* Open and read back via the syscall. */
    uint32_t path = put_string("/h0/hello.txt", 0);
    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                                 VM_O_RDONLY, 0);
    ASSERT(fd >= 3);
    uint32_t buf = 0x80000000 + 256;
    int32_t n = invoke_syscall(SYS_READ, (uint32_t)fd, buf, 64, 0);
    ASSERT_EQ_INT(12, n);
    ASSERT_EQ_INT(0, memcmp(g_data + 256, "hi from host", 12));
    invoke_syscall(SYS_CLOSE, (uint32_t)fd, 0, 0, 0);

    /* Writes are rejected on a read-only mount. */
    uint32_t wpath = put_string("/h0/new.txt", 64);
    int32_t r = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, wpath,
                                VM_O_WRONLY | VM_O_CREAT, 0);
    ASSERT_EQ_INT(-(int)VM_EROFS, (int)r);

    fixture_teardown();
    /* Cleanup the temp dir. */
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/hello.txt", root);
        remove(path);
        rmdir(root);
    }
}

static void test_mount_host_writable(void) {
    ASSERT(fixture_init());

    const char *root = "/tmp/microgarbage_mount_test_rw";
    mkdir(root, 0755);
    ASSERT(vm_host_fs_mount_host("h0", root, true));

    /* Write a file. */
    uint32_t path = put_string("/h0/new.txt", 0);
    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                                 VM_O_WRONLY | VM_O_CREAT, 0);
    ASSERT(fd >= 3);
    uint32_t data = put_string("payload", 64);
    int32_t w = invoke_syscall(SYS_WRITE, (uint32_t)fd, data, 7, 0);
    ASSERT_EQ_INT(7, w);
    invoke_syscall(SYS_CLOSE, (uint32_t)fd, 0, 0, 0);

    /* Verify via the host fs that the file actually appeared. */
    {
        char p[256];
        snprintf(p, sizeof(p), "%s/new.txt", root);
        FILE *f = fopen(p, "rb");
        ASSERT_NOT_NULL(f);
        char buf[16] = {0};
        size_t r = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        ASSERT_EQ_INT(7, (int)r);
        ASSERT_EQ_INT(0, memcmp(buf, "payload", 7));
        remove(p);
    }
    rmdir(root);
    fixture_teardown();
}

static void test_mount_dotdot_rejected(void) {
    ASSERT(fixture_init());

    const char *root = "/tmp/microgarbage_mount_test_esc";
    mkdir(root, 0755);
    ASSERT(vm_host_fs_mount_host("h0", root, false));

    /* Try to escape with ../. Always EPERM, regardless of whether
     * the target exists. */
    uint32_t p1 = put_string("/h0/../etc/passwd", 0);
    int32_t r1 = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, p1,
                                 VM_O_RDONLY, 0);
    ASSERT_EQ_INT(-(int)VM_EPERM, (int)r1);

    /* Even .. at the very top. */
    uint32_t p2 = put_string("/h0/..", 64);
    int32_t r2 = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, p2,
                                 VM_O_RDONLY, 0);
    ASSERT_EQ_INT(-(int)VM_EPERM, (int)r2);

    rmdir(root);
    fixture_teardown();
}

static void test_mount_invalid_names(void) {
    ASSERT(fixture_init());

    /* Empty, too long, and bad characters all rejected. */
    ASSERT(!vm_host_fs_mount_fatfs("",                     1, &g_fs));
    ASSERT(!vm_host_fs_mount_fatfs("toolongnameisawful_x", 1, &g_fs));
    ASSERT(!vm_host_fs_mount_fatfs("bad/slash",            1, &g_fs));
    ASSERT(!vm_host_fs_mount_fatfs("bad.dot",              1, &g_fs));
    ASSERT(!vm_host_fs_mount_fatfs("bad space",            1, &g_fs));
    /* Good ones. */
    ASSERT(vm_host_fs_mount_fatfs("a",        1, &g_fs));
    ASSERT(vm_host_fs_mount_fatfs("td-1",     2, &g_fs));
    ASSERT(vm_host_fs_mount_fatfs("UPPER_OK", 3, &g_fs));
    /* Duplicate name rejected. */
    ASSERT(!vm_host_fs_mount_fatfs("a", 4, &g_fs));

    fixture_teardown();
}

static void test_mount_count_and_unmount(void) {
    ASSERT(fixture_init());

    /* fixture_init already added one (td0). */
    ASSERT_EQ_INT(1, (int)vm_host_fs_mount_count());

    ASSERT(vm_host_fs_mount_fatfs("td1", 1, &g_fs));
    ASSERT_EQ_INT(2, (int)vm_host_fs_mount_count());

    ASSERT(vm_host_fs_mount_fatfs("td2", 2, &g_fs));
    ASSERT_EQ_INT(3, (int)vm_host_fs_mount_count());

    /* Unmount the middle one. The count drops; td0 and td2 still
     * resolvable. */
    ASSERT(vm_host_fs_unmount("td1"));
    ASSERT_EQ_INT(2, (int)vm_host_fs_mount_count());

    /* Unmount a non-existent one fails cleanly. */
    ASSERT(!vm_host_fs_unmount("nope"));

    fixture_teardown();
}

int main(void) {
    TEST_SUITE("vm_host_fs");
    RUN(test_openat_create_writes_and_reads_back);
    RUN(test_openat_nonexistent_returns_enoent);
    RUN(test_openat_wrong_dirfd_rejected);
    RUN(test_mkdirat_and_readdir);
    RUN(test_unlinkat_removes_file);
    RUN(test_lseek_set_cur_end);
    RUN(test_open_fd_limit);
    RUN(test_path_translation_rejects_bare_paths);
    RUN(test_spawn_and_wait_minimal_elf);
    RUN(test_route_functions_handle_file_fds);
    RUN(test_route_functions_reject_stdio_fds);
    RUN(test_mount_host_basic);
    RUN(test_mount_host_writable);
    RUN(test_mount_dotdot_rejected);
    RUN(test_mount_invalid_names);
    RUN(test_mount_count_and_unmount);
    return TEST_SUITE_RESULT();
}

#endif /* HAVE_FATFS */
