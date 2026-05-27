/* Tests for trashfs Phase 1: format + mount + geometry.
 *
 * Standalone unit test (like test_trashdrive.c). Build and run:
 *   cc -std=c11 -Wall -Wextra -Wpedantic -Iinclude -Isrc/storage \
 *      -o test_trashfs src/storage/tests/test_trashfs.c src/storage/trashfs.c
 *   ./test_trashfs
 *
 * Public domain (CC0). No warranty.
 */

#include "test_runner.h"
#include "storage/trashfs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A 64 KB working region, plus a tiny one for floor cases. */
static uint8_t g_region[64 * 1024];

/* ============================================================
 *  format: argument validation
 * ============================================================ */

static void test_format_basic_succeeds(void) {
    TrashfsResult r = trashfs_format(g_region, sizeof(g_region), 0, 0);
    ASSERT_EQ_INT(TRASHFS_OK, (int)r);
}

static void test_format_null_region_fails(void) {
    TrashfsResult r = trashfs_format(NULL, sizeof(g_region), 0, 0);
    ASSERT_EQ_INT(TRASHFS_ERR_INVALID_ARG, (int)r);
}

static void test_format_unaligned_size_fails(void) {
    /* Not a multiple of the 128-byte block size. */
    TrashfsResult r = trashfs_format(g_region, 16 * 1024 + 1, 0, 0);
    ASSERT_EQ_INT(TRASHFS_ERR_INVALID_ARG, (int)r);
}

static void test_format_below_min_fails(void) {
    /* 8 KB is below the 16 KB minimum. */
    TrashfsResult r = trashfs_format(g_region, 8 * 1024, 0, 0);
    ASSERT_EQ_INT(TRASHFS_ERR_TOO_SMALL, (int)r);
}

/* ============================================================
 *  mount: validation + round-trip
 * ============================================================ */

static void test_mount_after_format(void) {
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, sizeof(g_region), 0, 0));
    TrashfsVolume vol;
    TrashfsResult r = trashfs_mount(&vol, g_region, sizeof(g_region));
    ASSERT_EQ_INT(TRASHFS_OK, (int)r);
    ASSERT(vol.mounted);
}

static void test_mount_unformatted_fails(void) {
    memset(g_region, 0, sizeof(g_region));   /* no magic */
    TrashfsVolume vol;
    TrashfsResult r = trashfs_mount(&vol, g_region, sizeof(g_region));
    ASSERT_EQ_INT(TRASHFS_ERR_BAD_MAGIC, (int)r);
}

static void test_mount_corrupt_magic_fails(void) {
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, sizeof(g_region), 0, 0));
    g_region[0] ^= 0xFF;   /* corrupt the magic */
    TrashfsVolume vol;
    TrashfsResult r = trashfs_mount(&vol, g_region, sizeof(g_region));
    ASSERT_EQ_INT(TRASHFS_ERR_BAD_MAGIC, (int)r);
}

static void test_mount_wrong_size_fails(void) {
    /* Format at full size, then mount claiming a different size:
     * total_blocks won't match region_bytes/block_size. */
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, sizeof(g_region), 0, 0));
    TrashfsVolume vol;
    TrashfsResult r = trashfs_mount(&vol, g_region, 32 * 1024);
    ASSERT_EQ_INT(TRASHFS_ERR_BAD_GEOMETRY, (int)r);
}

/* ============================================================
 *  geometry: the spec's worked examples
 * ============================================================ */

static void check_geom(uint32_t bytes, uint32_t exp_inodes,
                       uint32_t exp_inode_blocks, uint32_t exp_bitmap_blocks) {
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, bytes, 0, 0));
    TrashfsVolume vol;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mount(&vol, g_region, bytes));
    ASSERT_EQ_INT((int)(bytes / 128u), (int)trashfs_total_blocks(&vol));
    ASSERT_EQ_INT((int)exp_inodes, (int)trashfs_inode_count(&vol));
    ASSERT_EQ_INT((int)exp_inode_blocks, (int)vol.inode_blocks);
    ASSERT_EQ_INT((int)exp_bitmap_blocks, (int)vol.bitmap_blocks);
}

static void test_geom_16k(void) {
    /* 16 KB = 128 blocks. inodes = max(16, 128/16=8) = 16 (floor).
     * inode_blocks = 16/2 = 8. bitmap = ceil(128/8)=16 B -> 1 block. */
    check_geom(16 * 1024, 16, 8, 1);
}

static void test_geom_64k(void) {
    /* 64 KB = 512 blocks. inodes = 512/16 = 32. inode_blocks = 16.
     * bitmap = ceil(512/8)=64 B -> 1 block. */
    check_geom(64 * 1024, 32, 16, 1);
}

static void test_geom_inode_hint(void) {
    /* Ask for 64 inodes explicitly on a 64 KB volume. */
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, 64 * 1024, 64, 0));
    TrashfsVolume vol;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mount(&vol, g_region, 64 * 1024));
    ASSERT_EQ_INT(64, (int)trashfs_inode_count(&vol));
    ASSERT_EQ_INT(32, (int)vol.inode_blocks);   /* 64/2 */
}

/* ============================================================
 *  free counts after a fresh format
 * ============================================================ */

static void test_fresh_free_counts(void) {
    /* 64 KB = 512 blocks; inodes=32, inode_blocks=16, bitmap=1.
     * data_start = 1 (sb) + 1 (bitmap) + 16 (inodes) = 18.
     * Used at format: blocks 0..17 (metadata) + 18 (root) = 19 used.
     * free_blocks = 512 - 19 = 493.
     * free_inodes = 32 - 1 (root) = 31. */
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, 64 * 1024, 0, 0));
    TrashfsVolume vol;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mount(&vol, g_region, 64 * 1024));
    ASSERT_EQ_INT(18, (int)vol.data_start);
    ASSERT_EQ_INT(493, (int)trashfs_free_blocks(&vol));
    ASSERT_EQ_INT(31, (int)trashfs_free_inodes(&vol));
}

static void test_cache_self_heals(void) {
    /* Corrupt the cached free_blocks in the superblock; mount must
     * recompute the true value from the bitmap, ignoring the cache. */
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, 64 * 1024, 0, 0));
    /* free_blocks cache is at superblock offset 44. Trash it. */
    g_region[44] = 0xAB; g_region[45] = 0xCD;
    g_region[46] = 0xEF; g_region[47] = 0x12;
    TrashfsVolume vol;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mount(&vol, g_region, 64 * 1024));
    /* Recomputed, not the garbage we wrote. */
    ASSERT_EQ_INT(493, (int)trashfs_free_blocks(&vol));
}

/* ============================================================
 *  root directory inode established by format
 * ============================================================ */

static void test_root_inode_is_empty_dir(void) {
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, 64 * 1024, 0, 0));
    TrashfsVolume vol;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mount(&vol, g_region, 64 * 1024));
    /* inode 0 sits at the start of the inode table. */
    const uint8_t *inode0 = g_region + (size_t)vol.inode_start * 128u;
    uint16_t mode = (uint16_t)(inode0[0] | (inode0[1] << 8));
    ASSERT(mode & TRASHFS_MODE_USED);
    ASSERT(mode & TRASHFS_MODE_DIR);
    /* size 0 (empty dir) at offset 4 */
    uint32_t size = (uint32_t)inode0[4] | ((uint32_t)inode0[5] << 8)
                  | ((uint32_t)inode0[6] << 16) | ((uint32_t)inode0[7] << 24);
    ASSERT_EQ_INT(0, (int)size);
    /* direct[0] (offset 16) points at the first data block. */
    uint32_t d0 = (uint32_t)inode0[16] | ((uint32_t)inode0[17] << 8)
                | ((uint32_t)inode0[18] << 16) | ((uint32_t)inode0[19] << 24);
    ASSERT_EQ_INT((int)vol.data_start, (int)d0);
}

/* ============================================================ */

int main(void) {
    TEST_SUITE("trashfs (Phase 1: format + mount)");

    /* format validation */
    RUN(test_format_basic_succeeds);
    RUN(test_format_null_region_fails);
    RUN(test_format_unaligned_size_fails);
    RUN(test_format_below_min_fails);

    /* mount validation + round-trip */
    RUN(test_mount_after_format);
    RUN(test_mount_unformatted_fails);
    RUN(test_mount_corrupt_magic_fails);
    RUN(test_mount_wrong_size_fails);

    /* geometry */
    RUN(test_geom_16k);
    RUN(test_geom_64k);
    RUN(test_geom_inode_hint);

    /* free counts + self-healing caches */
    RUN(test_fresh_free_counts);
    RUN(test_cache_self_heals);

    /* root directory */
    RUN(test_root_inode_is_empty_dir);

    return TEST_SUITE_RESULT();
}
