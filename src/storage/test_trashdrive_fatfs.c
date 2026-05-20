/* test_trashdrive_fatfs.c — exercise the diskio shim through real
 * FatFs operations on a RAM-backed volume.
 *
 * Build:
 *   cc -Wall -Wextra -Wpedantic -std=c11 -O2 \
 *      -Iinclude -DHAVE_FATFS \
 *      -Ithird_party/fatfs/source -Ithird_party/fatfs \
 *      -o test_trashdrive_fatfs \
 *      src/storage/test_trashdrive_fatfs.c \
 *      src/storage/trashdrive_fatfs.c \
 *      src/storage/trashdrive.c \
 *      third_party/fatfs/source/ff.c \
 *      third_party/fatfs/source/ffsystem.c
 *
 * If FatFs hasn't been extracted yet, omit the `-DHAVE_FATFS` and
 * the third_party paths — the test will compile to a stub that
 * reports "skipped" and exits cleanly. This keeps the test
 * discoverable in CI/build pipelines without erroring out.
 */

#include "test_runner.h"

#ifndef HAVE_FATFS

/* Stub mode: FatFs not present. Print a single "skip" notice. */
#include <stdio.h>
int main(void) {
    /* Touch the test_runner.h statics so -Wunused-variable doesn't
     * complain when we compile in stub mode (no RUN(...) calls). */
    (void)tr_passed_;
    (void)tr_failed_;
    (void)tr_current_failed_;
    (void)tr_suite_name_;
    (void)tr_current_test_name_;

    TEST_SUITE("trashdrive_fatfs");
    printf("  SKIP  FatFs not built in (compile with -DHAVE_FATFS and "
           "-Ithird_party/fatfs/source -Ithird_party/fatfs)\n");
    printf("0 passed, 0 failed (skipped)\n");
    return 0;
}

#else  /* HAVE_FATFS */

#include "storage/trashdrive.h"
#include "storage/trashdrive_fatfs.h"
#include "ff.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* 64 KB region — comfortably above FatFs's minimum (FatFs wants
 * enough sectors for the boot record, FAT table, root directory,
 * and at least one data cluster; ~16 KB is the practical floor
 * for FAT12). 64 KB gives us room for a handful of small files. */
/* 128 KB — needs to be at least ~96 KB for FatFs R0.16 f_mkfs to
 * succeed with our config (FM_FAT, n_fat=1). 128 KB leaves headroom
 * for files and FatFs's own bookkeeping. */
#define POOL_BYTES (128 * 1024)
static uint8_t g_pool[POOL_BYTES];
static TrashDrive g_drive;
static FATFS g_fs;

/* Helper: format and mount a fresh volume for each test. We do
 * this in every test rather than once globally so each test
 * starts from a known-clean state. */
static bool fresh_volume(void) {
    /* Clear the registry — previous tests may have registered
     * other slots. */
    for (uint8_t i = 0; i < TRASH_FATFS_MAX_VOLUMES; i++) {
        trash_fatfs_register(i, NULL);
    }
    f_mount(NULL, "0:", 0);  /* Unmount any previous mount. */
    memset(g_pool, 0, sizeof(g_pool));

    if (trash_init(&g_drive, g_pool, sizeof(g_pool)) != TRASH_OK) return false;
    if (!trash_fatfs_register(0, &g_drive)) return false;

    /* Format. f_mkfs needs a work buffer of FF_MAX_SS = 512 bytes. */
    BYTE work[FF_MAX_SS];
    MKFS_PARM opt = {0};
    opt.fmt = FM_FAT;       /* Force FAT12/16 (no FAT32) for small volumes */
    opt.n_fat = 1;          /* One FAT copy (saves space) */
    FRESULT r = f_mkfs("0:", &opt, work, sizeof(work));
    if (r != FR_OK) {
        printf("    [f_mkfs failed: %d]\n", r);
        return false;
    }
    r = f_mount(&g_fs, "0:", 1);
    if (r != FR_OK) {
        printf("    [f_mount failed: %d]\n", r);
        return false;
    }
    return true;
}

/* ----- tests ----- */

static void test_register_and_get(void) {
    /* Reset registry. */
    for (uint8_t i = 0; i < TRASH_FATFS_MAX_VOLUMES; i++) {
        trash_fatfs_register(i, NULL);
    }
    ASSERT_EQ_PTR(NULL, trash_fatfs_get(0));

    TrashDrive d;
    ASSERT_EQ_INT(TRASH_OK, trash_init(&d, g_pool, sizeof(g_pool)));
    ASSERT(trash_fatfs_register(0, &d));
    ASSERT_EQ_PTR(&d, trash_fatfs_get(0));

    /* Unregister. */
    ASSERT(trash_fatfs_register(0, NULL));
    ASSERT_EQ_PTR(NULL, trash_fatfs_get(0));

    /* Out-of-range pdrv. */
    ASSERT(!trash_fatfs_register(TRASH_FATFS_MAX_VOLUMES, &d));
    ASSERT_EQ_PTR(NULL, trash_fatfs_get(TRASH_FATFS_MAX_VOLUMES));
}

static void test_mkfs_and_mount(void) {
    ASSERT(fresh_volume());
    /* Free space query should succeed and report a sensible
     * number (most of the 64 KB minus FatFs overhead). */
    DWORD free_clusters = 0;
    FATFS *fs_ptr = NULL;
    FRESULT r = f_getfree("0:", &free_clusters, &fs_ptr);
    ASSERT_EQ_INT(FR_OK, r);
    /* Should have more than 1 free cluster on a freshly-mkfs'd 64 KB volume. */
    ASSERT(free_clusters > 1);
}

static void test_write_then_read_file(void) {
    ASSERT(fresh_volume());

    const char *msg = "Hello from a RAM-mounted FAT volume!\n";
    UINT msg_len = (UINT)strlen(msg);

    /* Write. */
    FIL f;
    FRESULT r = f_open(&f, "0:/hello.txt", FA_WRITE | FA_CREATE_ALWAYS);
    ASSERT_EQ_INT(FR_OK, r);
    UINT bw = 0;
    r = f_write(&f, msg, msg_len, &bw);
    ASSERT_EQ_INT(FR_OK, r);
    ASSERT_EQ_INT((int)msg_len, (int)bw);
    r = f_close(&f);
    ASSERT_EQ_INT(FR_OK, r);

    /* Read it back. */
    r = f_open(&f, "0:/hello.txt", FA_READ);
    ASSERT_EQ_INT(FR_OK, r);
    char buf[128] = {0};
    UINT br = 0;
    r = f_read(&f, buf, sizeof(buf) - 1, &br);
    ASSERT_EQ_INT(FR_OK, r);
    ASSERT_EQ_INT((int)msg_len, (int)br);
    ASSERT_EQ_INT(0, memcmp(buf, msg, msg_len));
    f_close(&f);
}

static void test_mkdir_and_ls(void) {
    ASSERT(fresh_volume());

    FRESULT r = f_mkdir("0:/dir1");
    ASSERT_EQ_INT(FR_OK, r);
    r = f_mkdir("0:/dir2");
    ASSERT_EQ_INT(FR_OK, r);

    /* Create a file inside one of them. */
    FIL f;
    r = f_open(&f, "0:/dir1/file.dat", FA_WRITE | FA_CREATE_ALWAYS);
    ASSERT_EQ_INT(FR_OK, r);
    UINT bw = 0;
    r = f_write(&f, "x", 1, &bw);
    ASSERT_EQ_INT(FR_OK, r);
    f_close(&f);

    /* Enumerate root — should find dir1, dir2 (and nothing else,
     * since we just formatted). */
    DIR d;
    r = f_opendir(&d, "0:/");
    ASSERT_EQ_INT(FR_OK, r);
    int saw_dir1 = 0, saw_dir2 = 0;
    for (;;) {
        FILINFO fi;
        r = f_readdir(&d, &fi);
        if (r != FR_OK || fi.fname[0] == '\0') break;
        if (strcmp(fi.fname, "DIR1") == 0) saw_dir1 = 1;
        if (strcmp(fi.fname, "DIR2") == 0) saw_dir2 = 1;
    }
    f_closedir(&d);
    /* 8.3 filenames are uppercase. */
    ASSERT(saw_dir1);
    ASSERT(saw_dir2);
}

static void test_unlink(void) {
    ASSERT(fresh_volume());

    FIL f;
    FRESULT r = f_open(&f, "0:/temp.txt", FA_WRITE | FA_CREATE_ALWAYS);
    ASSERT_EQ_INT(FR_OK, r);
    f_close(&f);

    /* File exists. */
    FILINFO fi;
    r = f_stat("0:/temp.txt", &fi);
    ASSERT_EQ_INT(FR_OK, r);

    /* Remove it. */
    r = f_unlink("0:/temp.txt");
    ASSERT_EQ_INT(FR_OK, r);

    /* Now it doesn't. */
    r = f_stat("0:/temp.txt", &fi);
    ASSERT_EQ_INT(FR_NO_FILE, r);
}

static void test_seek_and_tell(void) {
    ASSERT(fresh_volume());

    FIL f;
    FRESULT r = f_open(&f, "0:/seek.txt", FA_WRITE | FA_CREATE_ALWAYS);
    ASSERT_EQ_INT(FR_OK, r);

    /* Write 100 bytes of 'A's. */
    char fill[100];
    memset(fill, 'A', sizeof(fill));
    UINT bw = 0;
    r = f_write(&f, fill, sizeof(fill), &bw);
    ASSERT_EQ_INT(FR_OK, r);
    f_close(&f);

    /* Reopen for read+update; seek to byte 50; overwrite with 'B's. */
    r = f_open(&f, "0:/seek.txt", FA_READ | FA_WRITE);
    ASSERT_EQ_INT(FR_OK, r);
    r = f_lseek(&f, 50);
    ASSERT_EQ_INT(FR_OK, r);
    ASSERT_EQ_INT(50, (int)f_tell(&f));

    char b_fill[10];
    memset(b_fill, 'B', sizeof(b_fill));
    r = f_write(&f, b_fill, sizeof(b_fill), &bw);
    ASSERT_EQ_INT(FR_OK, r);

    f_lseek(&f, 0);
    char readback[100];
    UINT br = 0;
    r = f_read(&f, readback, sizeof(readback), &br);
    ASSERT_EQ_INT(FR_OK, r);
    ASSERT_EQ_INT(100, (int)br);

    /* Verify: 0..49 are 'A', 50..59 are 'B', 60..99 are 'A'. */
    for (int i = 0; i < 100; i++) {
        char expected = (i >= 50 && i < 60) ? 'B' : 'A';
        if (readback[i] != expected) {
            printf("    [mismatch at %d: got %c, expected %c]\n",
                   i, readback[i], expected);
            FAIL("seek/overwrite produced wrong bytes");
            f_close(&f);
            return;
        }
    }
    f_close(&f);
}

static void test_persists_across_mount(void) {
    /* Format, write a file, unmount, mount again, read it back.
     * Demonstrates that the volume content lives in the trashdrive
     * region, not in FATFS internal state. */
    ASSERT(fresh_volume());

    FIL f;
    FRESULT r = f_open(&f, "0:/persist.txt", FA_WRITE | FA_CREATE_ALWAYS);
    ASSERT_EQ_INT(FR_OK, r);
    UINT bw = 0;
    const char *content = "I persist across unmount/mount.";
    UINT len = (UINT)strlen(content);
    r = f_write(&f, content, len, &bw);
    ASSERT_EQ_INT(FR_OK, r);
    f_close(&f);

    /* Unmount. */
    f_mount(NULL, "0:", 0);

    /* Mount again — DON'T re-mkfs, just remount. */
    r = f_mount(&g_fs, "0:", 1);
    ASSERT_EQ_INT(FR_OK, r);

    /* The file should still be there. */
    r = f_open(&f, "0:/persist.txt", FA_READ);
    ASSERT_EQ_INT(FR_OK, r);
    char buf[64] = {0};
    UINT br = 0;
    r = f_read(&f, buf, sizeof(buf) - 1, &br);
    ASSERT_EQ_INT(FR_OK, r);
    ASSERT_EQ_INT((int)len, (int)br);
    ASSERT_EQ_INT(0, memcmp(buf, content, len));
    f_close(&f);
}

int main(void) {
    TEST_SUITE("trashdrive_fatfs");
    RUN(test_register_and_get);
    RUN(test_mkfs_and_mount);
    RUN(test_write_then_read_file);
    RUN(test_mkdir_and_ls);
    RUN(test_unlink);
    RUN(test_seek_and_tell);
    RUN(test_persists_across_mount);
    return TEST_SUITE_RESULT();
}

#endif /* HAVE_FATFS */
