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
 *      third_party/fatfs/source/ff.c third_party/fatfs/source/ffsystem.c
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

    uint32_t path = put_string("/test.txt", 0);
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

    uint32_t path = put_string("/nope.txt", 0);
    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path, VM_O_RDONLY, 0);
    ASSERT_EQ_INT(-VM_ENOENT, fd);

    fixture_teardown();
}

static void test_openat_wrong_dirfd_rejected(void) {
    ASSERT(fixture_init());

    uint32_t path = put_string("/some.txt", 0);
    /* Pass dirfd != AT_FDCWD; should be rejected with EINVAL. */
    int32_t r = invoke_syscall(SYS_OPENAT, 5, path,
                                VM_O_RDONLY | VM_O_CREAT, 0);
    ASSERT_EQ_INT(-VM_EINVAL, r);

    fixture_teardown();
}

static void test_mkdirat_and_readdir(void) {
    ASSERT(fixture_init());

    /* Create two directories. */
    uint32_t p1 = put_string("/d1", 0);
    uint32_t p2 = put_string("/d2", 64);
    ASSERT_EQ_INT(0, invoke_syscall(SYS_MKDIRAT, VM_AT_FDCWD, p1, 0, 0));
    ASSERT_EQ_INT(0, invoke_syscall(SYS_MKDIRAT, VM_AT_FDCWD, p2, 0, 0));

    /* Open root for readdir. */
    uint32_t root = put_string("/", 128);
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
    uint32_t path = put_string("/del.txt", 0);
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

    uint32_t path = put_string("/seek.dat", 0);
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
    char namebuf[16];
    for (i = 0; i < VM_HOST_FS_MAX_FILES; i++) {
        snprintf(namebuf, sizeof(namebuf), "/f%u", i);
        uint32_t path = put_string(namebuf, i * 16);
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
    snprintf(namebuf, sizeof(namebuf), "/overflow");
    uint32_t path = put_string(namebuf, i * 16);
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

static void test_path_translation_strips_volume_prefix(void) {
    /* If the guest passes "0:/foo", our path-handling should
     * leave it as-is rather than mistakenly prefixing it. */
    ASSERT(fixture_init());

    uint32_t path = put_string("0:/explicit", 0);
    int32_t fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path,
                                 VM_O_WRONLY | VM_O_CREAT, 0);
    ASSERT(fd >= 3);
    invoke_syscall(SYS_CLOSE, (uint32_t)fd, 0, 0, 0);

    /* Re-open as "/explicit" — should find the same file. */
    uint32_t path2 = put_string("/explicit", 64);
    fd = invoke_syscall(SYS_OPENAT, VM_AT_FDCWD, path2, VM_O_RDONLY, 0);
    ASSERT(fd >= 3);
    invoke_syscall(SYS_CLOSE, (uint32_t)fd, 0, 0, 0);

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
 * (the minimal program is sys_exit(0)). */
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

    /* Write the ELF into the FatFs volume at /spawn.elf. */
    FIL ff;
    ASSERT(f_open(&ff, "0:/spawn.elf", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK);
    UINT bw;
    ASSERT(f_write(&ff, elf, (UINT)elf_size, &bw) == FR_OK);
    ASSERT(bw == elf_size);
    ASSERT(f_close(&ff) == FR_OK);
    free(elf);

    /* Spawn it. The path "/spawn.elf" gets translated to
     * "0:/spawn.elf" by our path resolver. */
    uint32_t path = put_string("/spawn.elf", 0);
    int32_t rc = invoke_syscall(SYS_SPAWN_AND_WAIT, path, 0, 0, 0);

    /* guest_minimal calls sys_exit(0); exit code masked to 8 bits
     * by our handler. */
    ASSERT_EQ_INT(rc, 0);

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
    RUN(test_path_translation_strips_volume_prefix);
    RUN(test_spawn_and_wait_minimal_elf);
    return TEST_SUITE_RESULT();
}

#endif /* HAVE_FATFS */
