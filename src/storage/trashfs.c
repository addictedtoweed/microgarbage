/* ============================================================
 *  trashfs.c — Phase 1: format (mkfs) + mount.
 *
 *  See include/storage/trashfs.h and docs/trashfs-format.md.
 *
 *  On-disk values are little-endian and accessed through explicit
 *  byte helpers, so this code is correct regardless of host
 *  endianness or struct padding. We never overlay a struct on the
 *  raw bytes for I/O; the typed structs in the header are for
 *  clarity and size checks only.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "storage/trashfs.h"

#include <string.h>

/* ---- compile-time format checks ---------------------------- */
/* These guard the LOCKED format constants. C11 _Static_assert. */
_Static_assert(TRASHFS_BLOCK_SIZE == 128, "block size locked at 128");
_Static_assert(TRASHFS_INODE_SIZE == 64, "inode size locked at 64");
_Static_assert(TRASHFS_DIRENT_SIZE == 48, "dirent size locked at 48");
_Static_assert(TRASHFS_INODES_PER_BLOCK == 2, "2 inodes per block");
_Static_assert(TRASHFS_DIRENTS_PER_BLOCK == 2, "2 dirents per block");
_Static_assert(TRASHFS_PTRS_PER_BLOCK == 32, "32 pointers per block");

/* ---- little-endian byte accessors -------------------------- */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}
static void wr16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

/* Byte address of a block within the region. */
static uint8_t *block_ptr(uint8_t *region, uint32_t block) {
    return region + (size_t)block * TRASHFS_BLOCK_SIZE;
}

/* ---- geometry computation ---------------------------------- *
 *
 *  Given total_blocks and an optional inode hint, compute the
 *  region layout: bitmap blocks, inode blocks, data start. Shared
 *  by format() and (for validation) conceptually by mount().
 */
typedef struct {
    uint32_t total_blocks;
    uint32_t bitmap_start;
    uint32_t bitmap_blocks;
    uint32_t inode_start;
    uint32_t inode_blocks;
    uint32_t inode_count;
    uint32_t data_start;
} TrashfsGeom;

static uint32_t ceil_div(uint32_t a, uint32_t b) {
    return (a + b - 1u) / b;
}

/* Compute geometry. Returns false if the volume can't fit even the
 * minimum metadata + at least one data block. */
static bool compute_geometry(uint32_t total_blocks, uint32_t inode_hint,
                             TrashfsGeom *g) {
    g->total_blocks = total_blocks;

    /* Superblock occupies block 0. Bitmap follows at block 1. */
    g->bitmap_start = 1u;
    /* One bit per block in the whole volume. */
    uint32_t bitmap_bytes = ceil_div(total_blocks, 8u);
    g->bitmap_blocks = ceil_div(bitmap_bytes, TRASHFS_BLOCK_SIZE);
    if (g->bitmap_blocks == 0u) g->bitmap_blocks = 1u;

    /* Inode count: hint if given, else ~1 per 16 blocks, floored. */
    uint32_t icount = inode_hint;
    if (icount == 0u) {
        icount = total_blocks / TRASHFS_BLOCKS_PER_INODE;
        if (icount < TRASHFS_INODES_FLOOR) icount = TRASHFS_INODES_FLOOR;
    }
    /* Round up to a whole number of inode blocks. */
    g->inode_blocks = ceil_div(icount, TRASHFS_INODES_PER_BLOCK);
    g->inode_count  = g->inode_blocks * TRASHFS_INODES_PER_BLOCK;

    g->inode_start = g->bitmap_start + g->bitmap_blocks;
    g->data_start  = g->inode_start + g->inode_blocks;

    /* Need at least one data block (for the root directory). */
    if (g->data_start >= total_blocks) return false;

    return true;
}

/* ---- bitmap helpers ---------------------------------------- */

static void bitmap_set(uint8_t *region, const TrashfsGeom *g, uint32_t block) {
    uint8_t *bm = block_ptr(region, g->bitmap_start);
    bm[block >> 3] |= (uint8_t)(1u << (block & 7u));
}

/* ---- format (mkfs) ----------------------------------------- */

TrashfsResult trashfs_format(uint8_t *region, uint32_t region_bytes,
                             uint32_t inode_hint, uint32_t now) {
    if (!region) return TRASHFS_ERR_INVALID_ARG;
    if (region_bytes % TRASHFS_BLOCK_SIZE != 0u) return TRASHFS_ERR_INVALID_ARG;
    if (region_bytes < TRASHFS_MIN_BYTES) return TRASHFS_ERR_TOO_SMALL;

    uint32_t total_blocks = region_bytes / TRASHFS_BLOCK_SIZE;

    TrashfsGeom g;
    if (!compute_geometry(total_blocks, inode_hint, &g)) {
        return TRASHFS_ERR_BAD_GEOMETRY;
    }

    /* Zero the whole region first: clears bitmap, inode table, and
     * data. After this every inode is free (mode=0) and every block
     * is marked free in the bitmap. We then mark metadata + the root
     * directory's first data block as allocated. */
    memset(region, 0, region_bytes);

    /* Mark blocks 0..data_start as allocated (superblock, bitmap,
     * inode table). These are never part of the free data pool. */
    for (uint32_t b = 0; b < g.data_start; b++) {
        bitmap_set(region, &g, b);
    }

    /* The root directory (inode 0) gets one data block now, so an
     * empty volume already has a valid (empty) root. That block is
     * the first data block. */
    uint32_t root_block = g.data_start;
    bitmap_set(region, &g, root_block);

    /* Root inode: a used directory, size 0 (no entries yet), with
     * its first direct pointer at root_block. */
    uint8_t *inode0 = block_ptr(region, g.inode_start); /* inode 0 at table start */
    wr16(inode0 + 0, (uint16_t)(TRASHFS_MODE_USED | TRASHFS_MODE_DIR));
    wr16(inode0 + 2, 1u);          /* links */
    wr32(inode0 + 4, 0u);          /* size: empty directory */
    wr32(inode0 + 8, now);         /* created */
    wr32(inode0 + 12, now);        /* modified */
    wr32(inode0 + 16, root_block); /* direct[0] */
    /* remaining direct[1..7], single, dbl, triple, reserved stay 0 */

    /* Free counts: data blocks not used by metadata or the root
     * block; inodes minus the root. */
    uint32_t used_blocks = g.data_start + 1u; /* metadata + root block */
    uint32_t free_blocks = total_blocks - used_blocks;
    uint32_t free_inodes = g.inode_count - 1u; /* root consumes inode 0 */

    /* Superblock (block 0). */
    uint8_t *sb = block_ptr(region, 0);
    wr32(sb + 0, TRASHFS_MAGIC);
    wr16(sb + 4, (uint16_t)TRASHFS_VERSION_MAJOR);
    wr16(sb + 6, (uint16_t)TRASHFS_VERSION_MINOR);
    wr32(sb + 8, TRASHFS_BLOCK_SIZE);
    wr32(sb + 12, total_blocks);
    wr32(sb + 16, g.bitmap_start);
    wr32(sb + 20, g.bitmap_blocks);
    wr32(sb + 24, g.inode_start);
    wr32(sb + 28, g.inode_blocks);
    wr32(sb + 32, g.inode_count);
    wr32(sb + 36, g.data_start);
    wr32(sb + 40, TRASHFS_ROOT_INODE);
    wr32(sb + 44, free_blocks);
    wr32(sb + 48, free_inodes);
    wr32(sb + 52, 0u);             /* flags */
    wr32(sb + 56, now);            /* created */
    /* 60..127 already zeroed by memset */

    return TRASHFS_OK;
}

/* ---- mount + validate + cache rebuild ---------------------- */

/* Count set bits in the bitmap that fall in the data region, to
 * rebuild free_blocks; and count used inodes, to rebuild free_inodes.
 * This makes the caches self-healing. */
static void rebuild_caches(TrashfsVolume *vol) {
    /* Free data blocks: scan bitmap over [data_start, total_blocks). */
    uint8_t *bm = block_ptr(vol->region, vol->bitmap_start);
    uint32_t free_blocks = 0;
    for (uint32_t b = vol->data_start; b < vol->total_blocks; b++) {
        uint8_t bit = (uint8_t)(bm[b >> 3] >> (b & 7u)) & 1u;
        if (!bit) free_blocks++;
    }
    vol->free_blocks = free_blocks;

    /* Free inodes: scan inode table for mode&USED == 0. */
    uint32_t free_inodes = 0;
    for (uint32_t i = 0; i < vol->inode_count; i++) {
        uint32_t blk = vol->inode_start + (i / TRASHFS_INODES_PER_BLOCK);
        uint32_t off = (i % TRASHFS_INODES_PER_BLOCK) * TRASHFS_INODE_SIZE;
        uint8_t *inode = block_ptr(vol->region, blk) + off;
        uint16_t mode = rd16(inode + 0);
        if ((mode & TRASHFS_MODE_USED) == 0u) free_inodes++;
    }
    vol->free_inodes = free_inodes;
}

TrashfsResult trashfs_mount(TrashfsVolume *vol,
                            uint8_t *region, uint32_t region_bytes) {
    if (!vol || !region) return TRASHFS_ERR_INVALID_ARG;
    if (region_bytes < TRASHFS_MIN_BYTES) return TRASHFS_ERR_TOO_SMALL;

    memset(vol, 0, sizeof(*vol));

    const uint8_t *sb = block_ptr(region, 0);
    if (rd32(sb + 0) != TRASHFS_MAGIC) return TRASHFS_ERR_BAD_MAGIC;

    uint16_t vmaj = rd16(sb + 4);
    if (vmaj != TRASHFS_VERSION_MAJOR) return TRASHFS_ERR_BAD_VERSION;

    uint32_t block_size   = rd32(sb + 8);
    uint32_t total_blocks = rd32(sb + 12);
    uint32_t bitmap_start = rd32(sb + 16);
    uint32_t bitmap_blocks= rd32(sb + 20);
    uint32_t inode_start  = rd32(sb + 24);
    uint32_t inode_blocks = rd32(sb + 28);
    uint32_t inode_count  = rd32(sb + 32);
    uint32_t data_start   = rd32(sb + 36);
    uint32_t root_inode   = rd32(sb + 40);

    /* Geometry sanity: block size matches, regions are ordered and
     * in-bounds, root inode is 0, inode_count consistent with blocks. */
    if (block_size != TRASHFS_BLOCK_SIZE) return TRASHFS_ERR_BAD_GEOMETRY;
    if (total_blocks != region_bytes / TRASHFS_BLOCK_SIZE)
        return TRASHFS_ERR_BAD_GEOMETRY;
    if (bitmap_start != 1u) return TRASHFS_ERR_BAD_GEOMETRY;
    if (inode_start != bitmap_start + bitmap_blocks)
        return TRASHFS_ERR_BAD_GEOMETRY;
    if (data_start != inode_start + inode_blocks)
        return TRASHFS_ERR_BAD_GEOMETRY;
    if (data_start >= total_blocks) return TRASHFS_ERR_BAD_GEOMETRY;
    if (inode_count != inode_blocks * TRASHFS_INODES_PER_BLOCK)
        return TRASHFS_ERR_BAD_GEOMETRY;
    if (root_inode != TRASHFS_ROOT_INODE) return TRASHFS_ERR_BAD_GEOMETRY;

    vol->region       = region;
    vol->region_bytes = region_bytes;
    vol->total_blocks = total_blocks;
    vol->bitmap_start = bitmap_start;
    vol->bitmap_blocks= bitmap_blocks;
    vol->inode_start  = inode_start;
    vol->inode_blocks = inode_blocks;
    vol->inode_count  = inode_count;
    vol->data_start   = data_start;

    rebuild_caches(vol);
    vol->mounted = true;
    return TRASHFS_OK;
}

/* ---- introspection ----------------------------------------- */

uint32_t trashfs_total_blocks(const TrashfsVolume *vol) {
    return vol ? vol->total_blocks : 0u;
}
uint32_t trashfs_free_blocks(const TrashfsVolume *vol) {
    return vol ? vol->free_blocks : 0u;
}
uint32_t trashfs_inode_count(const TrashfsVolume *vol) {
    return vol ? vol->inode_count : 0u;
}
uint32_t trashfs_free_inodes(const TrashfsVolume *vol) {
    return vol ? vol->free_inodes : 0u;
}
