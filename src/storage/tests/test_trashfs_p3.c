/* Tests for trashfs Phase 3: write path — create, grow, in-place
 * overwrite, O_TRUNC/O_APPEND, unlink + reclamation, ENOSPC.
 *
 * Build and run:
 *   cc -std=c11 -Wall -Wextra -Wpedantic -Iinclude -Isrc/storage \
 *      -o test_trashfs_p3 src/storage/tests/test_trashfs_p3.c src/storage/trashfs.c
 *   ./test_trashfs_p3
 *
 * Unlike Phase 2, these use the REAL library to create and write
 * files — no plant_file scaffolding. Round-tripping create -> write
 * -> read proves the write path against the (already-tested) read
 * path.
 */

#include "test_runner.h"
#include "storage/trashfs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_region[64 * 1024];

static void fresh(TrashfsVolume *vol) {
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, sizeof(g_region), 0, 0));
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mount(vol, g_region, sizeof(g_region)));
}

/* Helper: write a whole buffer (loops on short writes). */
static TrashfsResult write_all(TrashfsFile *f, const void *buf, uint32_t n,
                               uint32_t *total) {
    const uint8_t *p = (const uint8_t*)buf; uint32_t done = 0;
    if (total) *total = 0;
    while (done < n) {
        uint32_t w = 0;
        TrashfsResult r = trashfs_write(f, p + done, n - done, &w, 0);
        if (r != TRASHFS_OK) return r;
        if (w == 0) break;
        done += w;
    }
    if (total) *total = done;
    return TRASHFS_OK;
}

/* ============================================================
 *  create + write + read-back
 * ============================================================ */

static void test_create_write_read(void) {
    TrashfsVolume vol; fresh(&vol);
    TrashfsFile f;
    ASSERT_EQ_INT(TRASHFS_OK,
        (int)trashfs_open(&vol, "hello.txt", TRASHFS_O_CREAT, &f));
    const char *msg = "hello, write path";
    uint32_t w = 0;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_write(&f, msg, (uint32_t)strlen(msg), &w, 0));
    ASSERT_EQ_INT((int)strlen(msg), (int)w);
    trashfs_close(&f);

    /* Reopen and read back. */
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_open(&vol, "hello.txt", 0, &f));
    ASSERT_EQ_INT((int)strlen(msg), (int)f.size);
    char buf[64]; uint32_t got = 0;
    trashfs_read(&f, buf, sizeof(buf), &got);
    buf[got] = '\0';
    ASSERT_EQ_STR(msg, buf);
    trashfs_close(&f);
}

static void test_create_missing_without_creat_fails(void) {
    TrashfsVolume vol; fresh(&vol);
    TrashfsFile f;
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_FOUND,
        (int)trashfs_open(&vol, "ghost.txt", 0, &f));
}

static void test_free_counts_change_on_create(void) {
    TrashfsVolume vol; fresh(&vol);
    uint32_t fi0 = trashfs_free_inodes(&vol);
    uint32_t fb0 = trashfs_free_blocks(&vol);
    TrashfsFile f;
    trashfs_open(&vol, "a.txt", TRASHFS_O_CREAT, &f);
    trashfs_write(&f, "data", 4, NULL, 0);
    trashfs_close(&f);
    /* One inode consumed; at least one data block consumed. */
    ASSERT_EQ_INT((int)(fi0 - 1), (int)trashfs_free_inodes(&vol));
    ASSERT(trashfs_free_blocks(&vol) < fb0);
}

/* ============================================================
 *  growth across the indirect boundary
 * ============================================================ */

static void test_write_spans_indirect(void) {
    TrashfsVolume vol; fresh(&vol);
    /* 3000 bytes: 8 direct (1024) + 16 single-indirect blocks. */
    uint8_t pattern[3000];
    for (uint32_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i*31 + 5);

    TrashfsFile f;
    trashfs_open(&vol, "big.dat", TRASHFS_O_CREAT, &f);
    uint32_t w = 0;
    ASSERT_EQ_INT(TRASHFS_OK, (int)write_all(&f, pattern, sizeof(pattern), &w));
    ASSERT_EQ_INT((int)sizeof(pattern), (int)w);
    trashfs_close(&f);

    trashfs_open(&vol, "big.dat", 0, &f);
    ASSERT_EQ_INT((int)sizeof(pattern), (int)f.size);
    uint8_t out[3000]; uint32_t total = 0, got = 0;
    do { trashfs_read(&f, out+total, (uint32_t)sizeof(out)-total, &got); total += got; }
    while (got > 0 && total < sizeof(out));
    ASSERT_EQ_INT((int)sizeof(pattern), (int)total);
    ASSERT_EQ_INT(0, memcmp(pattern, out, sizeof(pattern)));
    trashfs_close(&f);
}

/* ============================================================
 *  in-place overwrite (the pre-allocate + write-in-place idiom)
 * ============================================================ */

static void test_in_place_overwrite(void) {
    TrashfsVolume vol; fresh(&vol);
    /* Pre-allocate: write 500 zero bytes. */
    uint8_t zeros[500]; memset(zeros, 0, sizeof(zeros));
    TrashfsFile f;
    trashfs_open(&vol, "log.bin", TRASHFS_O_CREAT, &f);
    uint32_t w = 0; write_all(&f, zeros, sizeof(zeros), &w);
    ASSERT_EQ_INT(500, (int)w);
    uint32_t blocks_after_prealloc = trashfs_free_blocks(&vol);

    /* Seek into the middle and overwrite a record in place. */
    uint32_t pos = 0;
    trashfs_lseek(&f, 200, TRASHFS_SEEK_SET, &pos);
    const char *rec = "RECORD";
    trashfs_write(&f, rec, 6, &w, 0);
    ASSERT_EQ_INT(6, (int)w);

    /* In-place write must NOT have changed size or allocated blocks. */
    ASSERT_EQ_INT(500, (int)f.size);
    ASSERT_EQ_INT((int)blocks_after_prealloc, (int)trashfs_free_blocks(&vol));
    trashfs_close(&f);

    /* Verify the record landed at offset 200 and the rest is intact. */
    trashfs_open(&vol, "log.bin", 0, &f);
    uint8_t out[500]; uint32_t total = 0, got = 0;
    do { trashfs_read(&f, out+total, (uint32_t)sizeof(out)-total, &got); total += got; }
    while (got > 0 && total < sizeof(out));
    ASSERT_EQ_INT(500, (int)total);
    ASSERT_EQ_INT(0, memcmp(out + 200, rec, 6));
    ASSERT_EQ_INT(0, out[199]);   /* byte before record untouched */
    ASSERT_EQ_INT(0, out[206]);   /* byte after record untouched */
    trashfs_close(&f);
}

/* ============================================================
 *  O_TRUNC and O_APPEND
 * ============================================================ */

static void test_o_trunc(void) {
    TrashfsVolume vol; fresh(&vol);
    TrashfsFile f;
    trashfs_open(&vol, "t.txt", TRASHFS_O_CREAT, &f);
    write_all(&f, "some existing content here", 26, NULL);
    trashfs_close(&f);
    uint32_t fb_before = trashfs_free_blocks(&vol);

    /* Reopen with O_TRUNC -> size 0, blocks freed. */
    trashfs_open(&vol, "t.txt", TRASHFS_O_TRUNC, &f);
    ASSERT_EQ_INT(0, (int)f.size);
    trashfs_close(&f);
    ASSERT(trashfs_free_blocks(&vol) > fb_before);   /* reclaimed */
}

static void test_o_append(void) {
    TrashfsVolume vol; fresh(&vol);
    TrashfsFile f;
    trashfs_open(&vol, "a.log", TRASHFS_O_CREAT, &f);
    write_all(&f, "AAA", 3, NULL);
    trashfs_close(&f);

    /* Reopen O_APPEND -> writes go to the end. */
    trashfs_open(&vol, "a.log", TRASHFS_O_APPEND, &f);
    ASSERT_EQ_INT(3, (int)f.pos);   /* positioned at EOF */
    write_all(&f, "BBB", 3, NULL);
    trashfs_close(&f);

    trashfs_open(&vol, "a.log", 0, &f);
    ASSERT_EQ_INT(6, (int)f.size);
    char buf[8]; uint32_t got = 0;
    trashfs_read(&f, buf, sizeof(buf), &got);
    buf[got] = '\0';
    ASSERT_EQ_STR("AAABBB", buf);
    trashfs_close(&f);
}

/* ============================================================
 *  unlink + reclamation
 * ============================================================ */

static void test_unlink_reclaims(void) {
    TrashfsVolume vol; fresh(&vol);
    uint32_t fi0 = trashfs_free_inodes(&vol);
    uint32_t fb0 = trashfs_free_blocks(&vol);

    TrashfsFile f;
    trashfs_open(&vol, "gone.dat", TRASHFS_O_CREAT, &f);
    uint8_t big[1500]; memset(big, 0xAA, sizeof(big));
    write_all(&f, big, sizeof(big), NULL);
    trashfs_close(&f);
    ASSERT(trashfs_free_inodes(&vol) < fi0);
    ASSERT(trashfs_free_blocks(&vol) < fb0);

    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_unlink(&vol, "gone.dat"));
    /* Everything reclaimed: counts back to the fresh values. */
    ASSERT_EQ_INT((int)fi0, (int)trashfs_free_inodes(&vol));
    ASSERT_EQ_INT((int)fb0, (int)trashfs_free_blocks(&vol));

    /* And it's really gone. */
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_FOUND, (int)trashfs_open(&vol, "gone.dat", 0, &f));
}

static void test_unlink_missing_fails(void) {
    TrashfsVolume vol; fresh(&vol);
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_FOUND, (int)trashfs_unlink(&vol, "nope"));
}

static void test_create_after_unlink_reuses(void) {
    TrashfsVolume vol; fresh(&vol);
    TrashfsFile f;
    trashfs_open(&vol, "x", TRASHFS_O_CREAT, &f); trashfs_write(&f,"1",1,NULL,0); trashfs_close(&f);
    trashfs_unlink(&vol, "x");
    /* Re-create: should succeed and reuse the freed inode/dir slot. */
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_open(&vol, "y", TRASHFS_O_CREAT, &f));
    trashfs_write(&f, "2", 1, NULL, 0);
    trashfs_close(&f);
    /* Only one file present. */
    TrashfsDir d; trashfs_opendir(&vol, "/", &d);
    int count = 0; bool have = false; TrashfsDirent_Out e;
    while (trashfs_readdir(&d, &e, &have) == TRASHFS_OK && have) count++;
    trashfs_closedir(&d);
    ASSERT_EQ_INT(1, count);
}

/* ============================================================
 *  ENOSPC
 * ============================================================ */

static void test_enospc_on_full(void) {
    /* Use a small 16 KB volume and write until it fills. */
    static uint8_t small[16 * 1024];
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(small, sizeof(small), 0, 0));
    TrashfsVolume vol;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mount(&vol, small, sizeof(small)));

    TrashfsFile f;
    trashfs_open(&vol, "fill.dat", TRASHFS_O_CREAT, &f);
    /* Keep writing 128-byte chunks until short/zero. */
    uint8_t chunk[128]; memset(chunk, 0x55, sizeof(chunk));
    uint32_t total = 0; int hit_limit = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t w = 0;
        TrashfsResult r = trashfs_write(&f, chunk, sizeof(chunk), &w, 0);
        total += w;
        if (r == TRASHFS_ERR_NO_SPACE || w == 0) { hit_limit = 1; break; }
    }
    ASSERT(hit_limit);            /* we did hit the ceiling */
    ASSERT(total > 0);            /* but wrote a meaningful amount first */
    ASSERT_EQ_INT(0, (int)trashfs_free_blocks(&vol));  /* genuinely full */
    trashfs_close(&f);

    /* The data we did write is readable and correct. */
    trashfs_open(&vol, "fill.dat", 0, &f);
    ASSERT_EQ_INT((int)total, (int)f.size);
    trashfs_close(&f);
}

/* ============================================================ */

int main(void) {
    TEST_SUITE("trashfs (Phase 3: write path)");

    RUN(test_create_write_read);
    RUN(test_create_missing_without_creat_fails);
    RUN(test_free_counts_change_on_create);

    RUN(test_write_spans_indirect);

    RUN(test_in_place_overwrite);

    RUN(test_o_trunc);
    RUN(test_o_append);

    RUN(test_unlink_reclaims);
    RUN(test_unlink_missing_fails);
    RUN(test_create_after_unlink_reuses);

    RUN(test_enospc_on_full);

    return TEST_SUITE_RESULT();
}
