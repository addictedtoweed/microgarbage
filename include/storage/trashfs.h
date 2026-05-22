/* ============================================================
 *  trashfs.h — a small filesystem for internal RAM disks.
 *
 *  Purpose-built for always-resident RAM/PSRAM-backed storage,
 *  distinct from FatFs (which serves removable, PC-readable SD/USB
 *  media). See docs/trashfs-format.md for the full on-disk format.
 *
 *  Key parameters (LOCKED — see the spec):
 *    - 128-byte blocks, uint32 block indices.
 *    - 64-byte inodes: 8 direct + single + double + triple indirect.
 *    - 48-byte directory entries, 32-char names, flat namespace.
 *    - free-block bitmap; inode table sized as a fraction of volume.
 *    - little-endian on disk (matches RISC-V guest + host order).
 *
 *  This header covers Phase 1: format (mkfs) + mount + introspection.
 *  File operations (open/read/write/lseek/readdir/unlink) arrive in
 *  later phases.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef TRASHFS_H
#define TRASHFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Format constants -------------------------------------- */

#define TRASHFS_MAGIC          0x54524653u  /* "TRFS" little-endian */
#define TRASHFS_VERSION_MAJOR  1
#define TRASHFS_VERSION_MINOR  0

#define TRASHFS_BLOCK_SIZE     128u
#define TRASHFS_INODE_SIZE     64u
#define TRASHFS_DIRENT_SIZE    48u

#define TRASHFS_INODES_PER_BLOCK  (TRASHFS_BLOCK_SIZE / TRASHFS_INODE_SIZE)   /* 2 */
#define TRASHFS_DIRENTS_PER_BLOCK (TRASHFS_BLOCK_SIZE / TRASHFS_DIRENT_SIZE)  /* 2 */
#define TRASHFS_PTRS_PER_BLOCK    (TRASHFS_BLOCK_SIZE / 4u)                   /* 32 */

#define TRASHFS_DIRECT_PTRS    8u
#define TRASHFS_NAME_MAX       32u

/* Smallest volume we support: 16 KB (the spec's floor case).
 * Below this the metadata floor leaves too little for data. */
#define TRASHFS_MIN_BYTES      (16u * 1024u)

/* Inode-table sizing default: ~1 inode per 16 blocks, floored at 16.
 * (mkfs can override via an explicit hint.) */
#define TRASHFS_INODES_FLOOR   16u
#define TRASHFS_BLOCKS_PER_INODE 16u

/* Block pointer 0 means "none" (block 0 is always the superblock). */
#define TRASHFS_BLOCK_NONE     0u

/* Inode mode bits. */
#define TRASHFS_MODE_USED      0x0001u  /* inode is allocated         */
#define TRASHFS_MODE_DIR       0x0002u  /* directory (vs regular file)*/

/* Directory entry type. */
#define TRASHFS_TYPE_FILE      0u
#define TRASHFS_TYPE_DIR       1u

/* The root directory is always inode 0. */
#define TRASHFS_ROOT_INODE     0u

/* ---- Result codes ------------------------------------------ */

typedef enum {
    TRASHFS_OK = 0,
    TRASHFS_ERR_INVALID_ARG,    /* NULL / nonsensical parameters     */
    TRASHFS_ERR_TOO_SMALL,      /* region below TRASHFS_MIN_BYTES    */
    TRASHFS_ERR_BAD_MAGIC,      /* superblock magic mismatch         */
    TRASHFS_ERR_BAD_VERSION,    /* unsupported version               */
    TRASHFS_ERR_BAD_GEOMETRY,   /* superblock fields inconsistent    */
    TRASHFS_ERR_NO_SPACE,       /* ENOSPC (later phases)             */
    TRASHFS_ERR_NOT_FOUND,      /* (later phases)                    */
    TRASHFS_ERR_IO,             /* unexpected internal failure       */
} TrashfsResult;

/* ---- On-disk structures ------------------------------------ *
 *
 *  All little-endian. We use byte arrays + helpers rather than
 *  relying on struct packing for the on-disk image, but we also
 *  define packed structs for clarity and compile-time size checks.
 *  The implementation reads/writes via explicit little-endian
 *  accessors so it is portable regardless of host endianness and
 *  struct padding. */

/* Superblock — block 0, 128 bytes. See spec for field meanings. */
typedef struct {
    uint32_t magic;          /* 0  TRASHFS_MAGIC                    */
    uint16_t version_major;  /* 4                                   */
    uint16_t version_minor;  /* 6                                   */
    uint32_t block_size;     /* 8  must equal TRASHFS_BLOCK_SIZE    */
    uint32_t total_blocks;   /* 12 region_bytes / block_size        */
    uint32_t bitmap_start;   /* 16 block index of the free bitmap   */
    uint32_t bitmap_blocks;  /* 20                                  */
    uint32_t inode_start;    /* 24 block index of the inode table   */
    uint32_t inode_blocks;   /* 28                                  */
    uint32_t inode_count;    /* 32 inode_blocks * INODES_PER_BLOCK  */
    uint32_t data_start;     /* 36 first data block                 */
    uint32_t root_inode;     /* 40 = TRASHFS_ROOT_INODE (0)         */
    uint32_t free_blocks;    /* 44 cached free data-block count     */
    uint32_t free_inodes;    /* 48 cached free inode count          */
    uint32_t flags;          /* 52 reserved (0)                     */
    uint32_t created;        /* 56 mkfs time (Unix sec; 0 no RTC)   */
    /* 60..127 reserved (zeroed) */
} TrashfsSuperblock;

/* Inode — 64 bytes, 2 per block. */
typedef struct {
    uint16_t mode;                       /* 0  TRASHFS_MODE_*       */
    uint16_t links;                      /* 2  reference count      */
    uint32_t size;                       /* 4  bytes                */
    uint32_t created;                    /* 8  Unix sec             */
    uint32_t modified;                   /* 12 Unix sec             */
    uint32_t direct[TRASHFS_DIRECT_PTRS];/* 16 8 direct block ptrs  */
    uint32_t single;                     /* 48 single-indirect ptr  */
    uint32_t dbl;                        /* 52 double-indirect ptr  */
    uint32_t triple;                     /* 56 triple-indirect ptr  */
    uint32_t reserved;                   /* 60 reserved (0)         */
} TrashfsInode;

/* Directory entry — 48 bytes, 2 per block. */
typedef struct {
    uint32_t inode;                      /* 0  0 = empty/deleted    */
    uint8_t  type;                       /* 4  TRASHFS_TYPE_*       */
    uint8_t  name_len;                   /* 5  1..32                */
    char     name[TRASHFS_NAME_MAX];     /* 6  not NUL-terminated   */
    uint8_t  pad[10];                    /* 38 zeroed -> 48 total   */
} TrashfsDirent;

/* ---- Mounted-volume handle --------------------------------- *
 *
 *  Holds the region pointer and a decoded copy of the superblock
 *  geometry, so hot paths don't re-read the on-disk superblock. */
typedef struct {
    uint8_t  *region;        /* base of the volume bytes            */
    uint32_t  region_bytes;  /* total region size                   */
    /* decoded geometry (mirrors the superblock) */
    uint32_t  total_blocks;
    uint32_t  bitmap_start;
    uint32_t  bitmap_blocks;
    uint32_t  inode_start;
    uint32_t  inode_blocks;
    uint32_t  inode_count;
    uint32_t  data_start;
    uint32_t  free_blocks;
    uint32_t  free_inodes;
    bool      mounted;
} TrashfsVolume;

/* ---- Phase 1 API: format + mount + introspection ----------- */

/* Format (mkfs) a region as an empty trashfs volume.
 *
 *   region        base of RAM/PSRAM to format
 *   region_bytes  size; must be a multiple of TRASHFS_BLOCK_SIZE
 *                 and >= TRASHFS_MIN_BYTES
 *   inode_hint    desired inode count, or 0 for the default formula
 *                 (clamped to what fits the volume)
 *   now           current Unix time for the superblock 'created'
 *                 field (pass 0 if no RTC)
 *
 *  Lays out superblock, zeroed free bitmap (with metadata blocks
 *  marked allocated), zeroed inode table, and an empty root
 *  directory (inode 0). On success the region is a valid volume
 *  that mount() will accept. Does not leave the volume mounted.
 */
TrashfsResult trashfs_format(uint8_t *region, uint32_t region_bytes,
                             uint32_t inode_hint, uint32_t now);

/* Mount a previously formatted region. Validates the superblock
 * (magic, version, geometry) and rebuilds the free caches by
 * scanning the bitmap and inode table (self-healing if a cached
 * count was left stale). Fills *vol. */
TrashfsResult trashfs_mount(TrashfsVolume *vol,
                            uint8_t *region, uint32_t region_bytes);

/* Introspection (valid after mount). */
uint32_t trashfs_total_blocks(const TrashfsVolume *vol);
uint32_t trashfs_free_blocks(const TrashfsVolume *vol);
uint32_t trashfs_inode_count(const TrashfsVolume *vol);
uint32_t trashfs_free_inodes(const TrashfsVolume *vol);

#ifdef __cplusplus
}
#endif

#endif /* TRASHFS_H */
