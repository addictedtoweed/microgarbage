/* Tests for trashfs Phase 2: read-only path (open/read/lseek/
 * readdir/close) and the inode block-walk.
 *
 * Build and run:
 *   cc -std=c11 -Wall -Wextra -Wpedantic -Iinclude -Isrc/storage \
 *      -o test_trashfs_p2 src/storage/test_trashfs_p2.c src/storage/trashfs.c
 *   ./test_trashfs_p2
 *
 * NOTE: file *creation* is Phase 3. To test reading, this file plants
 * files directly into a formatted volume via a small test-only helper
 * (plant_file) that pokes the on-disk bytes — it is scaffolding, NOT
 * the real write path. It deliberately mirrors the format the library
 * reads, so a passing read test proves the read path against a
 * known-correct on-disk image.
 */

#include "test_runner.h"
#include "storage/trashfs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_region[64 * 1024];

/* ---- little-endian pokes (mirror the library's private helpers) ---- */
static void w32(uint8_t *p, uint32_t v) {
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}
static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);
}
static void w16(uint8_t *p, uint16_t v) { p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); }

#define BS 128u

static uint8_t *blk(uint8_t *region, uint32_t b) { return region + (size_t)b*BS; }

/* Read superblock geometry we need for planting. */
typedef struct { uint32_t bitmap_start, inode_start, data_start, total_blocks; } Geo;
static Geo read_geo(uint8_t *region) {
    uint8_t *sb = region;
    Geo g;
    g.total_blocks = r32(sb+12);
    g.bitmap_start = r32(sb+16);
    g.inode_start  = r32(sb+24);
    g.data_start   = r32(sb+36);
    return g;
}

/* Allocate the next free data block by scanning the bitmap. Marks it
 * used and returns its index (0 = failure). Test-only. */
static uint32_t alloc_block(uint8_t *region, Geo *g) {
    uint8_t *bm = blk(region, g->bitmap_start);
    for (uint32_t b = g->data_start; b < g->total_blocks; b++) {
        uint8_t mask = (uint8_t)(1u << (b & 7u));
        if (!(bm[b>>3] & mask)) { bm[b>>3] |= mask; return b; }
    }
    return 0;
}

/* Allocate the next free inode (scan for mode&USED==0). Returns its
 * number, or UINT32_MAX on failure. Test-only. */
static uint32_t alloc_inode(uint8_t *region, Geo *g) {
    uint32_t icount = r32(region+32);
    for (uint32_t i = 0; i < icount; i++) {
        uint8_t *in = blk(region, g->inode_start + i/2u) + (i%2u)*64u;
        uint16_t mode = (uint16_t)(in[0] | (in[1]<<8));
        if (!(mode & 0x0001u)) {  /* USED bit clear */
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

/* Add a directory entry to root for (name -> inode, type). Grows the
 * root directory across already-allocated dir blocks; for the test we
 * keep root small enough to fit in its initial block, or allocate one
 * more. Test-only, mirrors the 48-byte entry layout. */
static void root_add_entry(uint8_t *region, Geo *g, const char *name,
                           uint32_t inode, uint8_t type) {
    uint8_t *root = blk(region, g->inode_start); /* inode 0 */
    uint32_t dsize = r32(root+4);
    uint32_t off = dsize;       /* append at end */
    uint32_t lbn = off / BS;

    /* Find or allocate the dir block for this lbn. For the test we
     * only use direct[0..7]. */
    uint32_t dirblk = r32(root + 16 + lbn*4);
    if (dirblk == 0) {
        dirblk = alloc_block(region, g);
        w32(root + 16 + lbn*4, dirblk);
    }
    uint8_t *e = blk(region, dirblk) + (off % BS);
    w32(e+0, inode);
    e[4] = type;
    uint8_t len = (uint8_t)strlen(name);
    e[5] = len;
    memcpy(e+6, name, len);
    /* pad bytes already zero from format's memset (fresh volume) */
    w32(root+4, dsize + 48u);   /* grow dir size */
}

/* Plant a regular file: name, contents (len bytes). Allocates an
 * inode + as many data blocks as needed (direct + single-indirect is
 * enough for our test sizes), writes the data, and adds a root entry.
 * Test-only scaffolding for exercising the read path. */
static uint32_t plant_file(uint8_t *region, const char *name,
                           const uint8_t *data, uint32_t len) {
    Geo g = read_geo(region);
    uint32_t ino = alloc_inode(region, &g);
    uint8_t *in = blk(region, g.inode_start + ino/2u) + (ino%2u)*64u;

    w16(in+0, 0x0001u);     /* USED, regular file */
    w16(in+2, 1u);          /* links */
    w32(in+4, len);         /* size */
    w32(in+8, 0u);          /* created */
    w32(in+12, 0u);         /* modified */

    uint32_t written = 0, lbn = 0;
    /* single-indirect block, allocated lazily */
    uint32_t single = 0;
    while (written < len) {
        uint32_t db = alloc_block(region, &g);
        uint32_t chunk = len - written; if (chunk > BS) chunk = BS;
        memcpy(blk(region, db), data + written, chunk);
        if (lbn < 8u) {
            w32(in + 16 + lbn*4, db);
        } else {
            uint32_t si = lbn - 8u;
            if (single == 0) { single = alloc_block(region, &g); w32(in+48, single); }
            w32(blk(region, single) + si*4u, db);
        }
        written += chunk; lbn++;
    }
    root_add_entry(region, &g, name, ino, 0 /*file*/);
    return ino;
}

static void fresh_volume(void) {
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, sizeof(g_region), 0, 0));
}

/* ============================================================
 *  open
 * ============================================================ */

static void test_open_missing_fails(void) {
    fresh_volume();
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsFile f;
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_FOUND, (int)trashfs_open(&vol, "nope.txt", 0, &f));
}

static void test_open_existing(void) {
    fresh_volume();
    const char *msg = "hello, trashfs";
    plant_file(g_region, "hello.txt", (const uint8_t*)msg, (uint32_t)strlen(msg));
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsFile f;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_open(&vol, "hello.txt", 0, &f));
    ASSERT_EQ_INT((int)strlen(msg), (int)f.size);
    trashfs_close(&f);
}

static void test_open_leading_slash(void) {
    fresh_volume();
    const char *msg = "x";
    plant_file(g_region, "a.bin", (const uint8_t*)msg, 1);
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsFile f;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_open(&vol, "/a.bin", 0, &f));
    trashfs_close(&f);
}

/* ============================================================
 *  read
 * ============================================================ */

static void test_read_small_file(void) {
    fresh_volume();
    const char *msg = "hello, trashfs";
    uint32_t mlen = (uint32_t)strlen(msg);
    plant_file(g_region, "hello.txt", (const uint8_t*)msg, mlen);
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsFile f; trashfs_open(&vol, "hello.txt", 0, &f);

    char buf[64]; uint32_t got = 0;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_read(&f, buf, sizeof(buf), &got));
    ASSERT_EQ_INT((int)mlen, (int)got);
    buf[got] = '\0';
    ASSERT_EQ_STR(msg, buf);
    trashfs_close(&f);
}

static void test_read_eof_returns_zero(void) {
    fresh_volume();
    const char *msg = "abc";
    plant_file(g_region, "abc.txt", (const uint8_t*)msg, 3);
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsFile f; trashfs_open(&vol, "abc.txt", 0, &f);

    char buf[16]; uint32_t got = 0;
    trashfs_read(&f, buf, sizeof(buf), &got);   /* reads all 3 */
    ASSERT_EQ_INT(3, (int)got);
    trashfs_read(&f, buf, sizeof(buf), &got);   /* now at EOF */
    ASSERT_EQ_INT(0, (int)got);
    trashfs_close(&f);
}

/* A file spanning multiple blocks AND into single-indirect, to
 * exercise the block-walk past the 8 direct pointers (8*128 = 1024
 * bytes is the direct limit). 2000 bytes needs direct + single. */
static void test_read_multiblock_indirect(void) {
    fresh_volume();
    uint8_t pattern[2000];
    for (uint32_t i = 0; i < sizeof(pattern); i++) pattern[i] = (uint8_t)(i * 7 + 1);
    plant_file(g_region, "big.dat", pattern, sizeof(pattern));
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsFile f; trashfs_open(&vol, "big.dat", 0, &f);
    ASSERT_EQ_INT((int)sizeof(pattern), (int)f.size);

    uint8_t out[2000]; uint32_t total = 0, got = 0;
    do {
        trashfs_read(&f, out + total, (uint32_t)sizeof(out) - total, &got);
        total += got;
    } while (got > 0 && total < sizeof(out));
    ASSERT_EQ_INT((int)sizeof(pattern), (int)total);
    ASSERT_EQ_INT(0, memcmp(pattern, out, sizeof(pattern)));
    trashfs_close(&f);
}

/* ============================================================
 *  lseek
 * ============================================================ */

static void test_lseek_set_and_read(void) {
    fresh_volume();
    const char *msg = "0123456789";
    plant_file(g_region, "n.txt", (const uint8_t*)msg, 10);
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsFile f; trashfs_open(&vol, "n.txt", 0, &f);

    uint32_t pos = 0;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_lseek(&f, 5, TRASHFS_SEEK_SET, &pos));
    ASSERT_EQ_INT(5, (int)pos);
    char buf[8]; uint32_t got = 0;
    trashfs_read(&f, buf, sizeof(buf), &got);
    ASSERT_EQ_INT(5, (int)got);
    buf[got] = '\0';
    ASSERT_EQ_STR("56789", buf);
    trashfs_close(&f);
}

static void test_lseek_cur_and_end(void) {
    fresh_volume();
    const char *msg = "ABCDEFGH";
    plant_file(g_region, "m.txt", (const uint8_t*)msg, 8);
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsFile f; trashfs_open(&vol, "m.txt", 0, &f);

    uint32_t pos = 0;
    trashfs_lseek(&f, 2, TRASHFS_SEEK_SET, &pos);
    trashfs_lseek(&f, 3, TRASHFS_SEEK_CUR, &pos);
    ASSERT_EQ_INT(5, (int)pos);
    trashfs_lseek(&f, -2, TRASHFS_SEEK_END, &pos);
    ASSERT_EQ_INT(6, (int)pos);
    char buf[8]; uint32_t got = 0;
    trashfs_read(&f, buf, sizeof(buf), &got);
    ASSERT_EQ_INT(2, (int)got);     /* "GH" */
    trashfs_close(&f);
}

static void test_lseek_negative_fails(void) {
    fresh_volume();
    plant_file(g_region, "z.txt", (const uint8_t*)"z", 1);
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsFile f; trashfs_open(&vol, "z.txt", 0, &f);
    uint32_t pos = 0;
    ASSERT_EQ_INT(TRASHFS_ERR_INVALID_ARG,
                  (int)trashfs_lseek(&f, -5, TRASHFS_SEEK_SET, &pos));
    trashfs_close(&f);
}

/* ============================================================
 *  readdir
 * ============================================================ */

static void test_readdir_empty(void) {
    fresh_volume();
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsDir d; ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_opendir(&vol, &d));
    TrashfsDirent_Out ent; bool have = true;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_readdir(&d, &ent, &have));
    ASSERT(!have);   /* empty directory */
    trashfs_closedir(&d);
}

static void test_readdir_lists_files(void) {
    fresh_volume();
    plant_file(g_region, "one.txt",  (const uint8_t*)"1", 1);
    plant_file(g_region, "two.txt",  (const uint8_t*)"22", 2);
    plant_file(g_region, "three.bin",(const uint8_t*)"333", 3);
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsDir d; trashfs_opendir(&vol, &d);

    int count = 0; bool saw_two = false; bool have = false;
    TrashfsDirent_Out ent;
    for (;;) {
        ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_readdir(&d, &ent, &have));
        if (!have) break;
        count++;
        if (strcmp(ent.name, "two.txt") == 0) {
            saw_two = true;
            ASSERT_EQ_INT(2, (int)ent.size);
            ASSERT_EQ_INT(TRASHFS_TYPE_FILE, (int)ent.type);
        }
    }
    ASSERT_EQ_INT(3, count);
    ASSERT(saw_two);
    trashfs_closedir(&d);
}

static void test_readdir_32char_name(void) {
    fresh_volume();
    /* exactly 32 chars */
    const char *n32 = "abcdefghijklmnopqrstuvwxyz012345";
    ASSERT_EQ_INT(32, (int)strlen(n32));
    plant_file(g_region, n32, (const uint8_t*)"x", 1);
    TrashfsVolume vol; trashfs_mount(&vol, g_region, sizeof(g_region));
    TrashfsDir d; trashfs_opendir(&vol, &d);
    TrashfsDirent_Out ent; bool have = false;
    trashfs_readdir(&d, &ent, &have);
    ASSERT(have);
    ASSERT_EQ_INT(32, (int)ent.name_len);
    ASSERT_EQ_STR(n32, ent.name);
    trashfs_closedir(&d);
}

/* ============================================================ */

int main(void) {
    TEST_SUITE("trashfs (Phase 2: read path)");

    RUN(test_open_missing_fails);
    RUN(test_open_existing);
    RUN(test_open_leading_slash);

    RUN(test_read_small_file);
    RUN(test_read_eof_returns_zero);
    RUN(test_read_multiblock_indirect);

    RUN(test_lseek_set_and_read);
    RUN(test_lseek_cur_and_end);
    RUN(test_lseek_negative_fails);

    RUN(test_readdir_empty);
    RUN(test_readdir_lists_files);
    RUN(test_readdir_32char_name);

    return TEST_SUITE_RESULT();
}
