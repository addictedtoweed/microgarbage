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
/* The inode and dirent sizes are LOCKED format constants (64-byte
 * inodes, 48-byte dirents holding a 32-char name). The block size is a
 * build-time knob (TRASHFS_BLOCK_SIZE); the rest of the geometry
 * derives from it, so instead of pinning exact values we assert the
 * invariants that derivation depends on. C11 _Static_assert. */
_Static_assert(TRASHFS_INODE_SIZE == 64, "inode size locked at 64");
_Static_assert(TRASHFS_DIRENT_SIZE == 48, "dirent size locked at 48");
_Static_assert((TRASHFS_BLOCK_SIZE & (TRASHFS_BLOCK_SIZE - 1u)) == 0u,
               "block size must be a power of two");
_Static_assert(TRASHFS_BLOCK_SIZE % TRASHFS_INODE_SIZE == 0u,
               "inodes must pack evenly into a block");
_Static_assert(TRASHFS_BLOCK_SIZE % 4u == 0u,
               "block must hold a whole number of 4-byte block pointers");
_Static_assert(TRASHFS_BLOCK_SIZE >= TRASHFS_DIRENT_SIZE,
               "block must hold at least one directory entry");
_Static_assert(TRASHFS_BYTES_PER_INODE % TRASHFS_BLOCK_SIZE == 0u,
               "bytes-per-inode must be a multiple of the block size");
_Static_assert(TRASHFS_BLOCKS_PER_INODE >= 1u,
               "bytes-per-inode must be >= the block size");

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

/* Pointer to inode i's 64 bytes within the region. */
static uint8_t *inode_ptr(TrashfsVolume *vol, uint32_t i) {
    uint32_t blk = vol->inode_start + (i / TRASHFS_INODES_PER_BLOCK);
    uint32_t off = (i % TRASHFS_INODES_PER_BLOCK) * TRASHFS_INODE_SIZE;
    return block_ptr(vol->region, blk) + off;
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

/* ============================================================
 *  Phase 3 support: block + inode allocation.
 * ============================================================ */

static bool bitmap_test_v(TrashfsVolume *vol, uint32_t block) {
    const uint8_t *bm = block_ptr(vol->region, vol->bitmap_start);
    return (bm[block >> 3] >> (block & 7u)) & 1u;
}
static void bitmap_set_v(TrashfsVolume *vol, uint32_t block) {
    uint8_t *bm = block_ptr(vol->region, vol->bitmap_start);
    bm[block >> 3] |= (uint8_t)(1u << (block & 7u));
}
static void bitmap_clear_v(TrashfsVolume *vol, uint32_t block) {
    uint8_t *bm = block_ptr(vol->region, vol->bitmap_start);
    bm[block >> 3] &= (uint8_t)~(1u << (block & 7u));
}

/* Allocate one free data block. Marks it used, zeroes its contents
 * (important: indirect-pointer blocks must start all-zero = all
 * "none"), decrements free_blocks. Returns TRASHFS_BLOCK_NONE (0) if
 * the volume is full. */
static uint32_t alloc_block_v(TrashfsVolume *vol) {
    for (uint32_t b = vol->data_start; b < vol->total_blocks; b++) {
        if (!bitmap_test_v(vol, b)) {
            bitmap_set_v(vol, b);
            memset(block_ptr(vol->region, b), 0, TRASHFS_BLOCK_SIZE);
            if (vol->free_blocks > 0) vol->free_blocks--;
            return b;
        }
    }
    return TRASHFS_BLOCK_NONE;
}

/* Free a data block: clear its bit, bump free_blocks. */
static void free_block_v(TrashfsVolume *vol, uint32_t block) {
    if (block == TRASHFS_BLOCK_NONE) return;
    if (block < vol->data_start || block >= vol->total_blocks) return;
    if (bitmap_test_v(vol, block)) {
        bitmap_clear_v(vol, block);
        vol->free_blocks++;
    }
}

/* Allocate a free inode. Returns its number, or UINT32_MAX if none. */
static uint32_t alloc_inode_v(TrashfsVolume *vol) {
    for (uint32_t i = 0; i < vol->inode_count; i++) {
        uint8_t *in = inode_ptr(vol, i);
        if ((rd16(in + 0) & TRASHFS_MODE_USED) == 0u) {
            if (vol->free_inodes > 0) vol->free_inodes--;
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

/* Growth counterpart to map_lbn: return the physical block for a
 * file's logical block 'lbn', allocating it AND any missing indirect
 * pointer blocks along the way. Returns TRASHFS_BLOCK_NONE on ENOSPC
 * (any required allocation failing). The inode pointer 'inode' is a
 * live pointer into the region, so writes through it persist. */
static uint32_t bmap_alloc(TrashfsVolume *vol, uint8_t *inode, uint32_t lbn) {
    const uint32_t P = TRASHFS_PTRS_PER_BLOCK;

    /* Helper: ensure a pointer slot at 'slot_ptr' names an allocated
     * block; if it's none, allocate one and store it. Returns the
     * block, or NONE on ENOSPC. */
    /* (Inlined below per level since slot locations differ.) */

    /* Direct */
    if (lbn < TRASHFS_DIRECT_PTRS) {
        uint8_t *slot = inode + 16u + lbn * 4u;
        uint32_t b = rd32(slot);
        if (b == TRASHFS_BLOCK_NONE) {
            b = alloc_block_v(vol);
            if (b == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(slot, b);
        }
        return b;
    }
    lbn -= TRASHFS_DIRECT_PTRS;

    /* Single indirect */
    if (lbn < P) {
        uint8_t *sp = inode + 48u;
        uint32_t single = rd32(sp);
        if (single == TRASHFS_BLOCK_NONE) {
            single = alloc_block_v(vol);
            if (single == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(sp, single);
        }
        uint8_t *slot = block_ptr(vol->region, single) + lbn * 4u;
        uint32_t b = rd32(slot);
        if (b == TRASHFS_BLOCK_NONE) {
            b = alloc_block_v(vol);
            if (b == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(slot, b);
        }
        return b;
    }
    lbn -= P;

    /* Double indirect */
    if (lbn < P * P) {
        uint8_t *dp = inode + 52u;
        uint32_t dbl = rd32(dp);
        if (dbl == TRASHFS_BLOCK_NONE) {
            dbl = alloc_block_v(vol);
            if (dbl == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(dp, dbl);
        }
        uint8_t *l1slot = block_ptr(vol->region, dbl) + (lbn / P) * 4u;
        uint32_t l1 = rd32(l1slot);
        if (l1 == TRASHFS_BLOCK_NONE) {
            l1 = alloc_block_v(vol);
            if (l1 == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(l1slot, l1);
        }
        uint8_t *slot = block_ptr(vol->region, l1) + (lbn % P) * 4u;
        uint32_t b = rd32(slot);
        if (b == TRASHFS_BLOCK_NONE) {
            b = alloc_block_v(vol);
            if (b == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(slot, b);
        }
        return b;
    }
    lbn -= P * P;

    /* Triple indirect */
    if (lbn < P * P * P) {
        uint8_t *tp = inode + 56u;
        uint32_t triple = rd32(tp);
        if (triple == TRASHFS_BLOCK_NONE) {
            triple = alloc_block_v(vol);
            if (triple == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(tp, triple);
        }
        uint8_t *l1slot = block_ptr(vol->region, triple) + (lbn / (P * P)) * 4u;
        uint32_t l1 = rd32(l1slot);
        if (l1 == TRASHFS_BLOCK_NONE) {
            l1 = alloc_block_v(vol);
            if (l1 == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(l1slot, l1);
        }
        uint8_t *l2slot = block_ptr(vol->region, l1) + ((lbn / P) % P) * 4u;
        uint32_t l2 = rd32(l2slot);
        if (l2 == TRASHFS_BLOCK_NONE) {
            l2 = alloc_block_v(vol);
            if (l2 == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(l2slot, l2);
        }
        uint8_t *slot = block_ptr(vol->region, l2) + (lbn % P) * 4u;
        uint32_t b = rd32(slot);
        if (b == TRASHFS_BLOCK_NONE) {
            b = alloc_block_v(vol);
            if (b == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
            wr32(slot, b);
        }
        return b;
    }

    return TRASHFS_BLOCK_NONE;   /* beyond max file size */
}

/* ============================================================
 *  Phase 2: read-only path.
 * ============================================================ */

/* Read pointer slot 'idx' from a block of 32 uint32 pointers. */
static uint32_t ptr_in_block(TrashfsVolume *vol, uint32_t block, uint32_t idx) {
    if (block == TRASHFS_BLOCK_NONE) return TRASHFS_BLOCK_NONE;
    const uint8_t *p = block_ptr(vol->region, block);
    return rd32(p + idx * 4u);
}

/* Map a file's logical block number (offset / BLOCK_SIZE) to its
 * physical block, walking direct -> single -> double -> triple
 * indirect. Returns TRASHFS_BLOCK_NONE for an unallocated block
 * (a hole, or beyond what's been allocated). Read-only: never
 * allocates. */
static uint32_t map_lbn(TrashfsVolume *vol, const uint8_t *inode, uint32_t lbn) {
    const uint32_t P = TRASHFS_PTRS_PER_BLOCK;            /* 32 */

    /* Direct: 0 .. 7 */
    if (lbn < TRASHFS_DIRECT_PTRS) {
        return rd32(inode + 16u + lbn * 4u);
    }
    lbn -= TRASHFS_DIRECT_PTRS;

    /* Single indirect: next P blocks */
    if (lbn < P) {
        uint32_t single = rd32(inode + 48u);
        return ptr_in_block(vol, single, lbn);
    }
    lbn -= P;

    /* Double indirect: next P*P blocks */
    if (lbn < P * P) {
        uint32_t dbl = rd32(inode + 52u);
        uint32_t l1  = ptr_in_block(vol, dbl, lbn / P);     /* which 2nd-level block */
        return ptr_in_block(vol, l1, lbn % P);
    }
    lbn -= P * P;

    /* Triple indirect: next P*P*P blocks */
    if (lbn < P * P * P) {
        uint32_t triple = rd32(inode + 56u);
        uint32_t l1 = ptr_in_block(vol, triple, lbn / (P * P));
        uint32_t l2 = ptr_in_block(vol, l1, (lbn / P) % P);
        return ptr_in_block(vol, l2, lbn % P);
    }

    /* Beyond the maximum addressable file size. */
    return TRASHFS_BLOCK_NONE;
}

/* If `off` lands in a position where a TRASHFS_DIRENT_SIZE-byte slot
 * would straddle a block boundary, advance to the next block so the
 * slot sits cleanly within one block. Returns the (possibly bumped)
 * offset.
 *
 * Why this exists: TRASHFS_BLOCK_SIZE isn't necessarily a multiple
 * of TRASHFS_DIRENT_SIZE (default 128 / 48 = 2.66 dirents per block).
 * Writing a dirent across a block boundary would corrupt the next
 * file's data — bytes that look like "dirent padding" land inside a
 * data block. The directory's logical size includes the skipped
 * tail-gap bytes, so the dir is sparse in the gaps (which is fine —
 * every iterator below skips them via this same helper). */
static inline uint32_t dirent_align(uint32_t off) {
    uint32_t pos = off % TRASHFS_BLOCK_SIZE;
    if (pos + TRASHFS_DIRENT_SIZE > TRASHFS_BLOCK_SIZE) {
        off += TRASHFS_BLOCK_SIZE - pos;
    }
    return off;
}

/* Scan the root directory for an entry named (name,len). On match,
 * returns true and fills *out_inode / *out_type. */
/* Find an entry by name within directory inode `dir_ino`. */
static bool dir_find_in(TrashfsVolume *vol, uint32_t dir_ino,
                        const char *name, uint32_t len,
                        uint32_t *out_inode, uint8_t *out_type) {
    const uint8_t *dn = inode_ptr(vol, dir_ino);
    uint32_t dsize = rd32(dn + 4u);

    for (uint32_t off = 0; off + TRASHFS_DIRENT_SIZE <= dsize; ) {
        off = dirent_align(off);
        if (off + TRASHFS_DIRENT_SIZE > dsize) break;
        uint32_t lbn = off / TRASHFS_BLOCK_SIZE;
        uint32_t blk = map_lbn(vol, dn, lbn);
        if (blk == TRASHFS_BLOCK_NONE) { off += TRASHFS_DIRENT_SIZE; continue; }
        const uint8_t *e = block_ptr(vol->region, blk)
                         + (off % TRASHFS_BLOCK_SIZE);
        uint32_t einode = rd32(e + 0);
        if (einode != 0u) {
            uint8_t  etype = e[4];
            uint8_t  elen  = e[5];
            if (elen == len && memcmp(e + 6, name, len) == 0) {
                if (out_inode) *out_inode = einode;
                if (out_type)  *out_type  = etype;
                return true;
            }
        }
        off += TRASHFS_DIRENT_SIZE;
    }
    return false;
}

/* True if directory inode `dir_ino` has no live entries. */
static bool dir_is_empty(TrashfsVolume *vol, uint32_t dir_ino) {
    const uint8_t *dn = inode_ptr(vol, dir_ino);
    uint32_t dsize = rd32(dn + 4u);
    for (uint32_t off = 0; off + TRASHFS_DIRENT_SIZE <= dsize; ) {
        off = dirent_align(off);
        if (off + TRASHFS_DIRENT_SIZE > dsize) break;
        uint32_t lbn = off / TRASHFS_BLOCK_SIZE;
        uint32_t blk = map_lbn(vol, dn, lbn);
        if (blk != TRASHFS_BLOCK_NONE) {
            const uint8_t *e = block_ptr(vol->region, blk)
                             + (off % TRASHFS_BLOCK_SIZE);
            if (rd32(e + 0) != 0u) return false;   /* a live entry */
        }
        off += TRASHFS_DIRENT_SIZE;
    }
    return true;
}

/* Find an entry by name in `dir_ino`, clear its slot, and return the
 * inode it referenced (and its type) via out-params. Does NOT touch
 * the referenced inode itself — the caller frees it. Returns true if
 * an entry was found+cleared. */
static bool dir_clear_entry_in(TrashfsVolume *vol, uint32_t dir_ino,
                               const char *name, uint32_t len,
                               uint32_t *out_inode, uint8_t *out_type) {
    uint8_t *dn = inode_ptr(vol, dir_ino);
    uint32_t dsize = rd32(dn + 4u);
    for (uint32_t off = 0; off + TRASHFS_DIRENT_SIZE <= dsize; ) {
        off = dirent_align(off);
        if (off + TRASHFS_DIRENT_SIZE > dsize) break;
        uint32_t lbn = off / TRASHFS_BLOCK_SIZE;
        uint32_t blk = map_lbn(vol, dn, lbn);
        if (blk == TRASHFS_BLOCK_NONE) { off += TRASHFS_DIRENT_SIZE; continue; }
        uint8_t *e = block_ptr(vol->region, blk) + (off % TRASHFS_BLOCK_SIZE);
        uint32_t einode = rd32(e + 0);
        if (einode != 0u && e[5] == len && memcmp(e + 6, name, len) == 0) {
            if (out_inode) *out_inode = einode;
            if (out_type)  *out_type  = e[4];
            memset(e, 0, TRASHFS_DIRENT_SIZE);     /* inode=0 -> free slot */
            return true;
        }
        off += TRASHFS_DIRENT_SIZE;
    }
    return false;
}

/* ---- path resolution --------------------------------------- *
 *
 *  Paths are '/'-separated; a leading '/' is optional. "." is skipped,
 *  ".." ascends (clamped at root). Intermediate components must be
 *  existing directories. */

#define TRASHFS_MAX_PATH_COMPS  40u   /* tokenizer cap (deep enough)  */
#define TRASHFS_PATH_DEPTH      40u   /* ".." ascend stack            */

/* Walk the first `count` components (from the tokenized arrays) as
 * directory steps, starting at root. If allow_last_nondir is false
 * every step must resolve to a directory; if true the FINAL step may
 * be a file (used to resolve a full path to its target). Returns the
 * resulting inode + type. */
static TrashfsResult path_step(TrashfsVolume *vol,
                               const char *const *cs, const uint32_t *cl,
                               uint32_t count, bool allow_last_nondir,
                               uint32_t *out_inode, uint8_t *out_type) {
    uint32_t stack[TRASHFS_PATH_DEPTH];
    uint32_t sp = 0;
    stack[0] = TRASHFS_ROOT_INODE;
    uint32_t cur = TRASHFS_ROOT_INODE;
    uint8_t  cur_type = TRASHFS_TYPE_DIR;

    for (uint32_t i = 0; i < count; i++) {
        const char *nm = cs[i];
        uint32_t len = cl[i];
        bool last = (i + 1u == count);

        if (len == 1u && nm[0] == '.') continue;                 /* "."  */
        if (len == 2u && nm[0] == '.' && nm[1] == '.') {         /* ".." */
            if (sp > 0) sp--;
            cur = stack[sp];
            cur_type = TRASHFS_TYPE_DIR;
            continue;
        }

        uint32_t ino; uint8_t type;
        if (!dir_find_in(vol, cur, nm, len, &ino, &type))
            return TRASHFS_ERR_NOT_FOUND;
        if (!last || !allow_last_nondir) {
            if (type != TRASHFS_TYPE_DIR) return TRASHFS_ERR_NOT_DIR;
            if (sp + 1u >= TRASHFS_PATH_DEPTH) return TRASHFS_ERR_INVALID_ARG;
            stack[++sp] = ino;
        }
        cur = ino;
        cur_type = type;
    }

    if (out_inode) *out_inode = cur;
    if (out_type)  *out_type  = cur_type;
    return TRASHFS_OK;
}

/* Tokenize `path` into component pointer/length arrays. Returns the
 * count via *out_n, or an error (name too long / too many comps). */
static TrashfsResult path_tokenize(const char *path,
                                   const char **cs, uint32_t *cl,
                                   uint32_t *out_n) {
    uint32_t n = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        uint32_t len = (uint32_t)(p - s);
        if (len > TRASHFS_NAME_MAX) return TRASHFS_ERR_INVALID_ARG;
        if (n >= TRASHFS_MAX_PATH_COMPS) return TRASHFS_ERR_INVALID_ARG;
        cs[n] = s; cl[n] = len; n++;
    }
    *out_n = n;
    return TRASHFS_OK;
}

/* Resolve a full path to its target inode + type ("/" or "" = root). */
static TrashfsResult resolve_full(TrashfsVolume *vol, const char *path,
                                  uint32_t *out_inode, uint8_t *out_type) {
    if (!path) return TRASHFS_ERR_INVALID_ARG;
    const char *cs[TRASHFS_MAX_PATH_COMPS]; uint32_t cl[TRASHFS_MAX_PATH_COMPS];
    uint32_t n = 0;
    TrashfsResult r = path_tokenize(path, cs, cl, &n);
    if (r != TRASHFS_OK) return r;
    return path_step(vol, cs, cl, n, true, out_inode, out_type);
}

/* Resolve all but the final component to a parent directory inode, and
 * return the final component as the leaf name. The leaf must be a real
 * name (not "", "." or ".."). */
static TrashfsResult resolve_parent(TrashfsVolume *vol, const char *path,
                                    uint32_t *out_parent,
                                    const char **out_leaf, uint32_t *out_leaf_len) {
    if (!path) return TRASHFS_ERR_INVALID_ARG;
    const char *cs[TRASHFS_MAX_PATH_COMPS]; uint32_t cl[TRASHFS_MAX_PATH_COMPS];
    uint32_t n = 0;
    TrashfsResult r = path_tokenize(path, cs, cl, &n);
    if (r != TRASHFS_OK) return r;
    if (n == 0) return TRASHFS_ERR_INVALID_ARG;          /* "/" has no leaf */

    const char *leaf = cs[n - 1]; uint32_t llen = cl[n - 1];
    if (llen == 0 ||
        (llen == 1 && leaf[0] == '.') ||
        (llen == 2 && leaf[0] == '.' && leaf[1] == '.'))
        return TRASHFS_ERR_INVALID_ARG;                  /* not a nameable leaf */

    uint8_t ptype;
    r = path_step(vol, cs, cl, n - 1u, false, out_parent, &ptype);
    if (r != TRASHFS_OK) return r;
    *out_leaf = leaf; *out_leaf_len = llen;
    return TRASHFS_OK;
}

TrashfsResult trashfs_read(TrashfsFile *f, void *buf, uint32_t n,
                           uint32_t *out_read) {
    if (!f || !f->open || !buf) return TRASHFS_ERR_INVALID_ARG;
    if (out_read) *out_read = 0;

    const uint8_t *inode = inode_ptr(f->vol, f->inode);
    uint8_t *dst = (uint8_t *)buf;
    uint32_t done = 0;

    while (done < n && f->pos < f->size) {
        uint32_t lbn   = f->pos / TRASHFS_BLOCK_SIZE;
        uint32_t boff  = f->pos % TRASHFS_BLOCK_SIZE;
        uint32_t avail = TRASHFS_BLOCK_SIZE - boff;          /* in this block */
        uint32_t left_in_file = f->size - f->pos;
        uint32_t want = n - done;
        uint32_t chunk = avail;
        if (chunk > want) chunk = want;
        if (chunk > left_in_file) chunk = left_in_file;

        uint32_t blk = map_lbn(f->vol, inode, lbn);
        if (blk == TRASHFS_BLOCK_NONE) {
            /* Hole: reads as zeros. */
            memset(dst + done, 0, chunk);
        } else {
            const uint8_t *src = block_ptr(f->vol->region, blk) + boff;
            memcpy(dst + done, src, chunk);
        }
        done    += chunk;
        f->pos  += chunk;
    }

    if (out_read) *out_read = done;
    return TRASHFS_OK;
}

TrashfsResult trashfs_lseek(TrashfsFile *f, int32_t off, int whence,
                            uint32_t *out_pos) {
    if (!f || !f->open) return TRASHFS_ERR_INVALID_ARG;

    int64_t base;
    switch (whence) {
        case TRASHFS_SEEK_SET: base = 0; break;
        case TRASHFS_SEEK_CUR: base = (int64_t)f->pos; break;
        case TRASHFS_SEEK_END: base = (int64_t)f->size; break;
        default: return TRASHFS_ERR_INVALID_ARG;
    }
    int64_t np = base + (int64_t)off;
    if (np < 0) return TRASHFS_ERR_INVALID_ARG;
    /* Seeking past EOF is permitted (sparse / future-write); we cap
     * at UINT32_MAX defensively. */
    if (np > (int64_t)0xFFFFFFFFu) return TRASHFS_ERR_INVALID_ARG;
    f->pos = (uint32_t)np;
    if (out_pos) *out_pos = f->pos;
    return TRASHFS_OK;
}

TrashfsResult trashfs_close(TrashfsFile *f) {
    if (!f) return TRASHFS_ERR_INVALID_ARG;
    f->open = false;
    return TRASHFS_OK;
}

/* ---- directory iteration ----------------------------------- */

TrashfsResult trashfs_opendir(TrashfsVolume *vol, const char *path,
                              TrashfsDir *d) {
    if (!vol || !vol->mounted || !path || !d) return TRASHFS_ERR_INVALID_ARG;

    uint32_t ino = 0; uint8_t type = 0;
    TrashfsResult r = resolve_full(vol, path, &ino, &type);
    if (r != TRASHFS_OK) return r;
    if (type != TRASHFS_TYPE_DIR) return TRASHFS_ERR_NOT_DIR;

    const uint8_t *dn = inode_ptr(vol, ino);
    memset(d, 0, sizeof(*d));
    d->vol   = vol;
    d->inode = ino;
    d->size  = rd32(dn + 4u);
    d->pos   = 0;
    d->open  = true;
    return TRASHFS_OK;
}

TrashfsResult trashfs_readdir(TrashfsDir *d, TrashfsDirent_Out *ent,
                              bool *out_have) {
    if (!d || !d->open || !ent) return TRASHFS_ERR_INVALID_ARG;
    if (out_have) *out_have = false;

    const uint8_t *dirnode = inode_ptr(d->vol, d->inode);

    while (d->pos + TRASHFS_DIRENT_SIZE <= d->size) {
        /* Skip block-tail gaps (see dirent_align). */
        d->pos = dirent_align(d->pos);
        if (d->pos + TRASHFS_DIRENT_SIZE > d->size) break;
        uint32_t off = d->pos;
        d->pos += TRASHFS_DIRENT_SIZE;

        uint32_t lbn = off / TRASHFS_BLOCK_SIZE;
        uint32_t blk = map_lbn(d->vol, dirnode, lbn);
        if (blk == TRASHFS_BLOCK_NONE) continue;
        const uint8_t *e = block_ptr(d->vol->region, blk)
                         + (off % TRASHFS_BLOCK_SIZE);
        uint32_t einode = rd32(e + 0);
        if (einode == 0u) continue;   /* empty/deleted slot — skip */

        uint8_t etype = e[4];
        uint8_t elen  = e[5];
        if (elen > TRASHFS_NAME_MAX) elen = TRASHFS_NAME_MAX; /* defensive */
        memcpy(ent->name, e + 6, elen);
        ent->name[elen] = '\0';
        ent->name_len = elen;
        ent->type  = etype;
        ent->inode = einode;
        ent->size  = rd32(inode_ptr(d->vol, einode) + 4u);

        if (out_have) *out_have = true;
        return TRASHFS_OK;
    }
    /* End of directory — out_have stays false. */
    return TRASHFS_OK;
}

TrashfsResult trashfs_closedir(TrashfsDir *d) {
    if (!d) return TRASHFS_ERR_INVALID_ARG;
    d->open = false;
    return TRASHFS_OK;
}

/* ============================================================
 *  Phase 3: write path — allocation, growth, in-place, unlink.
 * ============================================================ */

/* Free every data + indirect block referenced by an inode, walking
 * all four pointer levels. Used by truncate (O_TRUNC) and unlink.
 * Leaves all pointer slots zeroed and size 0; caller sets the inode's
 * other fields (or frees the inode). */
static void free_all_blocks(TrashfsVolume *vol, uint8_t *inode) {
    const uint32_t P = TRASHFS_PTRS_PER_BLOCK;

    /* Direct */
    for (uint32_t i = 0; i < TRASHFS_DIRECT_PTRS; i++) {
        uint8_t *slot = inode + 16u + i * 4u;
        free_block_v(vol, rd32(slot));
        wr32(slot, TRASHFS_BLOCK_NONE);
    }

    /* Single indirect */
    {
        uint32_t single = rd32(inode + 48u);
        if (single != TRASHFS_BLOCK_NONE) {
            uint8_t *sb = block_ptr(vol->region, single);
            for (uint32_t i = 0; i < P; i++) free_block_v(vol, rd32(sb + i * 4u));
            free_block_v(vol, single);
        }
        wr32(inode + 48u, TRASHFS_BLOCK_NONE);
    }

    /* Double indirect */
    {
        uint32_t dbl = rd32(inode + 52u);
        if (dbl != TRASHFS_BLOCK_NONE) {
            uint8_t *db = block_ptr(vol->region, dbl);
            for (uint32_t i = 0; i < P; i++) {
                uint32_t l1 = rd32(db + i * 4u);
                if (l1 == TRASHFS_BLOCK_NONE) continue;
                uint8_t *l1b = block_ptr(vol->region, l1);
                for (uint32_t j = 0; j < P; j++) free_block_v(vol, rd32(l1b + j * 4u));
                free_block_v(vol, l1);
            }
            free_block_v(vol, dbl);
        }
        wr32(inode + 52u, TRASHFS_BLOCK_NONE);
    }

    /* Triple indirect */
    {
        uint32_t triple = rd32(inode + 56u);
        if (triple != TRASHFS_BLOCK_NONE) {
            uint8_t *tb = block_ptr(vol->region, triple);
            for (uint32_t i = 0; i < P; i++) {
                uint32_t l1 = rd32(tb + i * 4u);
                if (l1 == TRASHFS_BLOCK_NONE) continue;
                uint8_t *l1b = block_ptr(vol->region, l1);
                for (uint32_t j = 0; j < P; j++) {
                    uint32_t l2 = rd32(l1b + j * 4u);
                    if (l2 == TRASHFS_BLOCK_NONE) continue;
                    uint8_t *l2b = block_ptr(vol->region, l2);
                    for (uint32_t k = 0; k < P; k++) free_block_v(vol, rd32(l2b + k * 4u));
                    free_block_v(vol, l2);
                }
                free_block_v(vol, l1);
            }
            free_block_v(vol, triple);
        }
        wr32(inode + 56u, TRASHFS_BLOCK_NONE);
    }
}

TrashfsResult trashfs_write(TrashfsFile *f, const void *buf, uint32_t n,
                            uint32_t *out_written, uint32_t now) {
    if (!f || !f->open || !buf) return TRASHFS_ERR_INVALID_ARG;
    if (out_written) *out_written = 0;
    if (f->is_dir) return TRASHFS_ERR_INVALID_ARG;

    uint8_t *inode = inode_ptr(f->vol, f->inode);
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t done = 0;

    while (done < n) {
        uint32_t lbn  = f->pos / TRASHFS_BLOCK_SIZE;
        uint32_t boff = f->pos % TRASHFS_BLOCK_SIZE;
        uint32_t room = TRASHFS_BLOCK_SIZE - boff;
        uint32_t want = n - done;
        uint32_t chunk = (want < room) ? want : room;

        uint32_t blk = bmap_alloc(f->vol, inode, lbn);
        if (blk == TRASHFS_BLOCK_NONE) {
            /* ENOSPC. If we've written nothing at all, report it; else
             * a short write is success with a short count. */
            break;
        }
        memcpy(block_ptr(f->vol->region, blk) + boff, src + done, chunk);
        done   += chunk;
        f->pos += chunk;

        /* Grow size if we wrote past the old end. */
        if (f->pos > f->size) f->size = f->pos;
    }

    /* Persist size + modified to the inode. */
    wr32(inode + 4u, f->size);
    wr32(inode + 12u, now);

    if (out_written) *out_written = done;
    if (done == 0 && n > 0) return TRASHFS_ERR_NO_SPACE;
    return TRASHFS_OK;
}

/* ---- directory entry insertion / removal ------------------- */

/* Find a free directory slot (inode==0) in the root, or grow the root
 * directory by one entry-slot, returning a pointer to the 48-byte
 * slot. Returns NULL on ENOSPC. *grew is set if the dir size grew. */
static uint8_t *dir_alloc_dirent_in(TrashfsVolume *vol, uint32_t dir_ino,
                                    bool *grew) {
    uint8_t *dn = inode_ptr(vol, dir_ino);
    uint32_t dsize = rd32(dn + 4u);
    *grew = false;

    /* Reuse a freed slot first. */
    for (uint32_t off = 0; off + TRASHFS_DIRENT_SIZE <= dsize; ) {
        off = dirent_align(off);
        if (off + TRASHFS_DIRENT_SIZE > dsize) break;
        uint32_t lbn = off / TRASHFS_BLOCK_SIZE;
        uint32_t blk = map_lbn(vol, dn, lbn);
        if (blk != TRASHFS_BLOCK_NONE) {
            uint8_t *e = block_ptr(vol->region, blk) + (off % TRASHFS_BLOCK_SIZE);
            if (rd32(e + 0) == 0u) return e;   /* free slot */
        }
        off += TRASHFS_DIRENT_SIZE;
    }

    /* Append a new slot at the end, allocating a block if needed.
     * If dsize ends in a tail-gap (no room for one more dirent in the
     * last block), advance to the next block start; the gap bytes are
     * logically part of the dir's size but never read. */
    uint32_t off = dirent_align(dsize);
    uint32_t lbn = off / TRASHFS_BLOCK_SIZE;
    uint32_t blk = bmap_alloc(vol, dn, lbn);
    if (blk == TRASHFS_BLOCK_NONE) return NULL;
    uint8_t *e = block_ptr(vol->region, blk) + (off % TRASHFS_BLOCK_SIZE);
    wr32(dn + 4u, off + TRASHFS_DIRENT_SIZE);
    *grew = true;
    return e;
}

TrashfsResult trashfs_open(TrashfsVolume *vol, const char *name,
                           uint32_t flags, TrashfsFile *f) {
    if (!vol || !vol->mounted || !name || !f) return TRASHFS_ERR_INVALID_ARG;

    /* Resolve the parent directory; the leaf is the file name. */
    uint32_t parent = 0; const char *nm = NULL; uint32_t len = 0;
    TrashfsResult pr = resolve_parent(vol, name, &parent, &nm, &len);
    if (pr != TRASHFS_OK) return pr;

    uint32_t ino = 0; uint8_t type = 0;
    bool found = dir_find_in(vol, parent, nm, len, &ino, &type);

    if (!found) {
        if (!(flags & TRASHFS_O_CREAT)) return TRASHFS_ERR_NOT_FOUND;

        /* Create: allocate an inode + a directory entry in the parent. */
        uint32_t newino = alloc_inode_v(vol);
        if (newino == 0xFFFFFFFFu) return TRASHFS_ERR_NO_SPACE;

        bool grew = false;
        uint8_t *slot = dir_alloc_dirent_in(vol, parent, &grew);
        if (!slot) {
            /* Roll back the inode reservation. */
            vol->free_inodes++;   /* alloc_inode_v decremented it */
            return TRASHFS_ERR_NO_SPACE;
        }

        /* Initialize the new inode as an empty regular file. */
        uint8_t *in = inode_ptr(vol, newino);
        memset(in, 0, TRASHFS_INODE_SIZE);
        wr16(in + 0, (uint16_t)TRASHFS_MODE_USED);   /* regular file */
        wr16(in + 2, 1u);                            /* links */
        /* size 0, timestamps 0, all pointers 0 (from memset) */

        /* Fill the directory entry. */
        wr32(slot + 0, newino);
        slot[4] = TRASHFS_TYPE_FILE;
        slot[5] = (uint8_t)len;
        memcpy(slot + 6, nm, len);
        /* pad bytes: zero them defensively (a reused slot may have old data) */
        memset(slot + 6 + len, 0, TRASHFS_DIRENT_SIZE - 6u - len);

        ino  = newino;
        type = TRASHFS_TYPE_FILE;
    }

    const uint8_t *inode = inode_ptr(vol, ino);
    memset(f, 0, sizeof(*f));
    f->vol    = vol;
    f->inode  = ino;
    f->is_dir = (type == TRASHFS_TYPE_DIR);
    f->open   = true;

    if ((flags & TRASHFS_O_TRUNC) && !f->is_dir) {
        uint8_t *in = inode_ptr(vol, ino);
        free_all_blocks(vol, in);
        wr32(in + 4u, 0u);          /* size = 0 */
        f->size = 0;
    } else {
        f->size = rd32(inode + 4u);
    }

    f->pos = (flags & TRASHFS_O_APPEND) ? f->size : 0u;
    return TRASHFS_OK;
}

TrashfsResult trashfs_unlink(TrashfsVolume *vol, const char *name) {
    if (!vol || !vol->mounted || !name) return TRASHFS_ERR_INVALID_ARG;

    uint32_t parent = 0; const char *nm = NULL; uint32_t len = 0;
    TrashfsResult pr = resolve_parent(vol, name, &parent, &nm, &len);
    if (pr != TRASHFS_OK) return pr;

    /* Peek the entry's type first: unlink is for files only. */
    uint32_t ino; uint8_t type;
    if (!dir_find_in(vol, parent, nm, len, &ino, &type))
        return TRASHFS_ERR_NOT_FOUND;
    if (type == TRASHFS_TYPE_DIR) return TRASHFS_ERR_NOT_DIR;  /* use rmdir */

    /* Clear the entry, then free the file's blocks + inode. */
    (void)dir_clear_entry_in(vol, parent, nm, len, NULL, NULL);
    uint8_t *in = inode_ptr(vol, ino);
    free_all_blocks(vol, in);
    memset(in, 0, TRASHFS_INODE_SIZE);   /* mode=0 -> inode free */
    vol->free_inodes++;
    return TRASHFS_OK;
}

TrashfsResult trashfs_mkdir(TrashfsVolume *vol, const char *path, uint32_t now) {
    if (!vol || !vol->mounted || !path) return TRASHFS_ERR_INVALID_ARG;

    uint32_t parent = 0; const char *nm = NULL; uint32_t len = 0;
    TrashfsResult pr = resolve_parent(vol, path, &parent, &nm, &len);
    if (pr != TRASHFS_OK) return pr;

    if (dir_find_in(vol, parent, nm, len, NULL, NULL))
        return TRASHFS_ERR_EXISTS;

    uint32_t newino = alloc_inode_v(vol);
    if (newino == 0xFFFFFFFFu) return TRASHFS_ERR_NO_SPACE;

    bool grew = false;
    uint8_t *slot = dir_alloc_dirent_in(vol, parent, &grew);
    if (!slot) { vol->free_inodes++; return TRASHFS_ERR_NO_SPACE; }

    /* New empty directory: MODE_USED|MODE_DIR, size 0 (no data block
     * until an entry is added — readdir of a size-0 dir yields nothing). */
    uint8_t *in = inode_ptr(vol, newino);
    memset(in, 0, TRASHFS_INODE_SIZE);
    wr16(in + 0, (uint16_t)(TRASHFS_MODE_USED | TRASHFS_MODE_DIR));
    wr16(in + 2, 1u);              /* links */
    wr32(in + 8u, now);            /* created  */
    wr32(in + 12u, now);           /* modified */

    wr32(slot + 0, newino);
    slot[4] = TRASHFS_TYPE_DIR;
    slot[5] = (uint8_t)len;
    memcpy(slot + 6, nm, len);
    memset(slot + 6 + len, 0, TRASHFS_DIRENT_SIZE - 6u - len);
    return TRASHFS_OK;
}

TrashfsResult trashfs_rmdir(TrashfsVolume *vol, const char *path) {
    if (!vol || !vol->mounted || !path) return TRASHFS_ERR_INVALID_ARG;

    /* Resolve the target itself — must be a directory, and not root. */
    uint32_t ino = 0; uint8_t type = 0;
    TrashfsResult r = resolve_full(vol, path, &ino, &type);
    if (r != TRASHFS_OK) return r;
    if (type != TRASHFS_TYPE_DIR) return TRASHFS_ERR_NOT_DIR;
    if (ino == TRASHFS_ROOT_INODE) return TRASHFS_ERR_INVALID_ARG;
    if (!dir_is_empty(vol, ino)) return TRASHFS_ERR_NOT_EMPTY;

    /* Resolve the parent + leaf to clear its entry. */
    uint32_t parent = 0; const char *nm = NULL; uint32_t len = 0;
    r = resolve_parent(vol, path, &parent, &nm, &len);
    if (r != TRASHFS_OK) return r;

    if (!dir_clear_entry_in(vol, parent, nm, len, NULL, NULL))
        return TRASHFS_ERR_NOT_FOUND;
    uint8_t *in = inode_ptr(vol, ino);
    free_all_blocks(vol, in);            /* the dir's (empty) data blocks */
    memset(in, 0, TRASHFS_INODE_SIZE);
    vol->free_inodes++;
    return TRASHFS_OK;
}
