/* Tests for trashdrive.
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "storage/trashdrive.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* 32 KB region — large enough for meaningful FAT12 formatting,
 * small enough to verify byte-by-byte. */
static uint8_t g_region[32 * 1024];

/* ============================================================
 *  Init validation
 * ============================================================ */

static void test_init_basic(void) {
    TrashDrive td;
    TrashResult r = trash_init(&td, g_region, sizeof(g_region));
    ASSERT_EQ_INT(TRASH_OK, (int)r);
    ASSERT_EQ_INT((int)(sizeof(g_region) / TRASH_SECTOR_SIZE),
                   (int)trash_sector_count(&td));
}

static void test_init_null_handle_fails(void) {
    TrashResult r = trash_init(NULL, g_region, sizeof(g_region));
    ASSERT_EQ_INT(TRASH_ERR_INVALID_ARG, (int)r);
}

static void test_init_null_region_fails(void) {
    TrashDrive td;
    TrashResult r = trash_init(&td, NULL, sizeof(g_region));
    ASSERT_EQ_INT(TRASH_ERR_INVALID_ARG, (int)r);
}

static void test_init_below_min_fails(void) {
    TrashDrive td;
    /* 4 KB is below TRASH_MIN_BYTES (16 KB). */
    TrashResult r = trash_init(&td, g_region, 4 * 1024);
    ASSERT_EQ_INT(TRASH_ERR_INVALID_ARG, (int)r);
}

static void test_init_non_sector_aligned_fails(void) {
    TrashDrive td;
    /* 16385 is not a multiple of 512. */
    TrashResult r = trash_init(&td, g_region, 16385);
    ASSERT_EQ_INT(TRASH_ERR_INVALID_ARG, (int)r);
}

/* ============================================================
 *  Introspection
 * ============================================================ */

static void test_sector_size_fixed_at_512(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));
    ASSERT_EQ_INT(512, (int)trash_sector_size(&td));
}

static void test_sector_count_matches_region(void) {
    TrashDrive td;
    trash_init(&td, g_region, 16 * 1024);
    /* 16384 / 512 = 32 sectors. */
    ASSERT_EQ_INT(32, (int)trash_sector_count(&td));
}

static void test_total_bytes_matches_region(void) {
    TrashDrive td;
    trash_init(&td, g_region, 16 * 1024);
    ASSERT_EQ_INT(16384, (int)trash_total_bytes(&td));
}

/* ============================================================
 *  Single-sector read/write
 * ============================================================ */

static void test_write_read_sector_zero(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));

    uint8_t out[TRASH_SECTOR_SIZE];
    for (size_t i = 0; i < TRASH_SECTOR_SIZE; i++) out[i] = (uint8_t)(i & 0xFF);
    ASSERT_EQ_INT(TRASH_OK, (int)trash_write(&td, out, 0, 1));

    uint8_t in[TRASH_SECTOR_SIZE];
    memset(in, 0xAA, sizeof(in));
    ASSERT_EQ_INT(TRASH_OK, (int)trash_read(&td, in, 0, 1));
    ASSERT_EQ_INT(0, memcmp(out, in, TRASH_SECTOR_SIZE));
}

static void test_write_read_arbitrary_sector(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));

    /* Write a distinct pattern to sector 17. */
    uint8_t out[TRASH_SECTOR_SIZE];
    for (size_t i = 0; i < TRASH_SECTOR_SIZE; i++)
        out[i] = (uint8_t)((i * 13 + 5) & 0xFF);
    ASSERT_EQ_INT(TRASH_OK, (int)trash_write(&td, out, 17, 1));

    uint8_t in[TRASH_SECTOR_SIZE];
    ASSERT_EQ_INT(TRASH_OK, (int)trash_read(&td, in, 17, 1));
    ASSERT_EQ_INT(0, memcmp(out, in, TRASH_SECTOR_SIZE));
}

static void test_write_to_one_does_not_affect_others(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));
    trash_clear(&td);

    uint8_t pattern[TRASH_SECTOR_SIZE];
    memset(pattern, 0xFE, sizeof(pattern));
    trash_write(&td, pattern, 5, 1);

    /* Sector 4 and sector 6 should still be zero. */
    uint8_t buf[TRASH_SECTOR_SIZE];
    trash_read(&td, buf, 4, 1);
    for (size_t i = 0; i < TRASH_SECTOR_SIZE; i++) ASSERT_EQ_INT(0, buf[i]);

    trash_read(&td, buf, 6, 1);
    for (size_t i = 0; i < TRASH_SECTOR_SIZE; i++) ASSERT_EQ_INT(0, buf[i]);
}

/* ============================================================
 *  Multi-sector read/write
 * ============================================================ */

static void test_multisector_write_read(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));

    /* Build a 4-sector buffer with a distinct byte pattern. */
    uint8_t out[TRASH_SECTOR_SIZE * 4];
    for (size_t i = 0; i < sizeof(out); i++) out[i] = (uint8_t)((i * 31) & 0xFF);
    ASSERT_EQ_INT(TRASH_OK, (int)trash_write(&td, out, 10, 4));

    uint8_t in[TRASH_SECTOR_SIZE * 4];
    ASSERT_EQ_INT(TRASH_OK, (int)trash_read(&td, in, 10, 4));
    ASSERT_EQ_INT(0, memcmp(out, in, sizeof(out)));
}

static void test_count_zero_succeeds(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));
    /* count==0 is a no-op and should succeed. */
    uint8_t buf[TRASH_SECTOR_SIZE];
    ASSERT_EQ_INT(TRASH_OK, (int)trash_read(&td, buf, 0, 0));
    ASSERT_EQ_INT(TRASH_OK, (int)trash_write(&td, buf, 0, 0));
}

/* ============================================================
 *  Out-of-range protection
 * ============================================================ */

static void test_read_past_end_fails(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));
    size_t n = trash_sector_count(&td);
    uint8_t buf[TRASH_SECTOR_SIZE];
    ASSERT_EQ_INT(TRASH_ERR_OUT_OF_RANGE, (int)trash_read(&td, buf, n, 1));
    ASSERT_EQ_INT(TRASH_ERR_OUT_OF_RANGE, (int)trash_read(&td, buf, n + 100, 1));
}

static void test_write_past_end_fails(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));
    size_t n = trash_sector_count(&td);
    uint8_t buf[TRASH_SECTOR_SIZE] = {0};
    ASSERT_EQ_INT(TRASH_ERR_OUT_OF_RANGE, (int)trash_write(&td, buf, n, 1));
}

static void test_multisector_partial_overrun_fails(void) {
    /* Last valid sector is n-1. Writing 2 sectors starting at n-1
     * would overrun by 1 — must fail entirely (not just write 1). */
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));
    size_t n = trash_sector_count(&td);
    uint8_t buf[TRASH_SECTOR_SIZE * 2] = {0};
    ASSERT_EQ_INT(TRASH_ERR_OUT_OF_RANGE,
                   (int)trash_write(&td, buf, n - 1, 2));
}

static void test_last_sector_accessible(void) {
    /* The very last sector (index n-1) should be readable and
     * writable — exclusive upper bound, not inclusive. */
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));
    size_t n = trash_sector_count(&td);
    uint8_t out[TRASH_SECTOR_SIZE];
    memset(out, 0xC3, sizeof(out));
    ASSERT_EQ_INT(TRASH_OK, (int)trash_write(&td, out, n - 1, 1));

    uint8_t in[TRASH_SECTOR_SIZE];
    ASSERT_EQ_INT(TRASH_OK, (int)trash_read(&td, in, n - 1, 1));
    ASSERT_EQ_INT(0xC3, in[0]);
    ASSERT_EQ_INT(0xC3, in[TRASH_SECTOR_SIZE - 1]);
}

/* ============================================================
 *  Clear behavior
 * ============================================================ */

static void test_clear_zeros_region(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));

    /* Dirty the region. */
    uint8_t junk[TRASH_SECTOR_SIZE];
    memset(junk, 0xFF, sizeof(junk));
    trash_write(&td, junk, 0, 1);
    trash_write(&td, junk, 10, 1);
    trash_write(&td, junk, trash_sector_count(&td) - 1, 1);

    trash_clear(&td);

    uint8_t buf[TRASH_SECTOR_SIZE];
    trash_read(&td, buf, 0, 1);
    for (size_t i = 0; i < TRASH_SECTOR_SIZE; i++) ASSERT_EQ_INT(0, buf[i]);
    trash_read(&td, buf, 10, 1);
    for (size_t i = 0; i < TRASH_SECTOR_SIZE; i++) ASSERT_EQ_INT(0, buf[i]);
}

/* ============================================================
 *  Filesystem-style integration simulation
 *
 *  Verify that a sequence of operations resembling what a
 *  filesystem would do (write metadata to sector 0, then write
 *  data to later sectors, then re-read the metadata, etc.) all
 *  work together as expected.
 * ============================================================ */

static void test_filesystem_style_workflow(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));
    trash_clear(&td);

    /* Step 1: write "boot sector" at sector 0 with a magic. */
    uint8_t boot[TRASH_SECTOR_SIZE] = {0};
    boot[0] = 0xEB;            /* x86 jump */
    boot[1] = 0x3C;
    boot[2] = 0x90;
    memcpy(boot + 3, "MSWIN4.1", 8);
    boot[TRASH_SECTOR_SIZE - 2] = 0x55;  /* boot signature */
    boot[TRASH_SECTOR_SIZE - 1] = 0xAA;
    trash_write(&td, boot, 0, 1);

    /* Step 2: write "FAT" entries at sectors 1 and 9 (typical FAT
     * has two copies of the allocation table). */
    uint8_t fat[TRASH_SECTOR_SIZE * 8];
    for (size_t i = 0; i < sizeof(fat); i++) fat[i] = (uint8_t)(i & 0xFF);
    trash_write(&td, fat, 1, 8);
    trash_write(&td, fat, 9, 8);

    /* Step 3: write "directory entries" at sector 17. */
    uint8_t dirent[TRASH_SECTOR_SIZE] = {0};
    memcpy(dirent, "MYFILE  TXT", 11);   /* 8.3 name padded */
    trash_write(&td, dirent, 17, 1);

    /* Step 4: write "file content" at sector 20. */
    uint8_t content[TRASH_SECTOR_SIZE];
    memset(content, 'A', sizeof(content));
    strcpy((char *)content, "This is the file content.");
    trash_write(&td, content, 20, 1);

    /* Step 5: read everything back and verify it's intact. */
    uint8_t check[TRASH_SECTOR_SIZE];

    trash_read(&td, check, 0, 1);
    ASSERT_EQ_INT(0xEB, check[0]);
    ASSERT_EQ_INT(0x55, check[TRASH_SECTOR_SIZE - 2]);
    ASSERT_EQ_INT(0xAA, check[TRASH_SECTOR_SIZE - 1]);
    ASSERT_EQ_INT(0, memcmp(check + 3, "MSWIN4.1", 8));

    uint8_t fat_check[TRASH_SECTOR_SIZE * 8];
    trash_read(&td, fat_check, 1, 8);
    ASSERT_EQ_INT(0, memcmp(fat, fat_check, sizeof(fat)));
    trash_read(&td, fat_check, 9, 8);
    ASSERT_EQ_INT(0, memcmp(fat, fat_check, sizeof(fat)));

    trash_read(&td, check, 17, 1);
    ASSERT_EQ_INT(0, memcmp(check, "MYFILE  TXT", 11));

    trash_read(&td, check, 20, 1);
    ASSERT_EQ_INT(0, strcmp((char *)check, "This is the file content."));
}

/* ============================================================
 *  Stress: write all sectors in a pattern and verify
 * ============================================================ */

static void test_all_sectors_distinct_patterns(void) {
    TrashDrive td;
    trash_init(&td, g_region, sizeof(g_region));

    size_t n = trash_sector_count(&td);
    uint8_t sec[TRASH_SECTOR_SIZE];

    /* Write a distinct pattern to every sector: each sector's
     * first byte is its sector index mod 256, the rest is filler. */
    for (size_t s = 0; s < n; s++) {
        memset(sec, (uint8_t)((s * 7) & 0xFF), TRASH_SECTOR_SIZE);
        sec[0] = (uint8_t)(s & 0xFF);
        trash_write(&td, sec, s, 1);
    }

    /* Read back and verify. */
    for (size_t s = 0; s < n; s++) {
        trash_read(&td, sec, s, 1);
        ASSERT_EQ_INT((int)(s & 0xFF), sec[0]);
        ASSERT_EQ_INT((int)((s * 7) & 0xFF), sec[100]);
        ASSERT_EQ_INT((int)((s * 7) & 0xFF), sec[TRASH_SECTOR_SIZE - 1]);
    }
}

/* ============================================================
 *  Main
 * ============================================================ */

int main(void) {
    TEST_SUITE("trashdrive");

    /* Init */
    RUN(test_init_basic);
    RUN(test_init_null_handle_fails);
    RUN(test_init_null_region_fails);
    RUN(test_init_below_min_fails);
    RUN(test_init_non_sector_aligned_fails);

    /* Introspection */
    RUN(test_sector_size_fixed_at_512);
    RUN(test_sector_count_matches_region);
    RUN(test_total_bytes_matches_region);

    /* Single-sector ops */
    RUN(test_write_read_sector_zero);
    RUN(test_write_read_arbitrary_sector);
    RUN(test_write_to_one_does_not_affect_others);

    /* Multi-sector ops */
    RUN(test_multisector_write_read);
    RUN(test_count_zero_succeeds);

    /* Range protection */
    RUN(test_read_past_end_fails);
    RUN(test_write_past_end_fails);
    RUN(test_multisector_partial_overrun_fails);
    RUN(test_last_sector_accessible);

    /* Clear */
    RUN(test_clear_zeros_region);

    /* Integration */
    RUN(test_filesystem_style_workflow);
    RUN(test_all_sectors_distinct_patterns);

    return TEST_SUITE_RESULT();
}
