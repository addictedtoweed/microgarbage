/* ============================================================
 *  trashfs.h — a small filesystem for internal RAM disks.
 *
 *  Purpose-built for always-resident RAM/PSRAM-backed storage —
 *  a native, public-domain filesystem, not a FAT-compatible one
 *  (it is not meant for removable, PC-readable SD/USB media). See
 *  docs/trashfs-format.md for the full on-disk format.
 *
 *  Key parameters:
 *    - Block size is a build-time knob (TRASHFS_BLOCK_SIZE, default
 *      128 B; e.g. 512 B for volumes holding larger objects like guest
 *      ELFs). uint32 block indices.
 *    - 64-byte inodes (LOCKED): 8 direct + single + double + triple
 *      indirect. Packed BLOCK_SIZE/64 per block (2 @128, 8 @512).
 *    - 48-byte directory entries (LOCKED), 32-char names, hierarchical
 *      directories (mkdir/rmdir, nested paths, "." and "..").
 *    - free-block bitmap; inode table sized by bytes-of-volume, so the
 *      file budget stays constant across block sizes.
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

/* Block size — the one build-time geometry knob. Default 128 B suits
 * tiny RAM disks with many small files; 512 B cuts indirection and
 * per-file metadata for volumes holding larger objects (e.g. guest
 * ELFs). Must be a power of two and a multiple of the 64-byte inode
 * size (the static_asserts in trashfs.c enforce this). A volume
 * records its block size in the superblock and mount rejects a
 * mismatch, so a region formatted at one size won't mis-mount. */
#ifndef TRASHFS_BLOCK_SIZE
#define TRASHFS_BLOCK_SIZE     128u
#endif

#define TRASHFS_INODE_SIZE     64u
#define TRASHFS_DIRENT_SIZE    48u

#define TRASHFS_INODES_PER_BLOCK  (TRASHFS_BLOCK_SIZE / TRASHFS_INODE_SIZE)   /* 2 @128, 8 @512 */
#define TRASHFS_DIRENTS_PER_BLOCK (TRASHFS_BLOCK_SIZE / TRASHFS_DIRENT_SIZE)  /* 2 @128, 10 @512 */
#define TRASHFS_PTRS_PER_BLOCK    (TRASHFS_BLOCK_SIZE / 4u)                   /* 32 @128, 128 @512 */

#define TRASHFS_DIRECT_PTRS    8u
#define TRASHFS_NAME_MAX       32u

/* Smallest volume we support: 16 KB (the spec's floor case).
 * Below this the metadata floor leaves too little for data. */
#define TRASHFS_MIN_BYTES      (16u * 1024u)

/* Inode-table sizing default: one inode per TRASHFS_BYTES_PER_INODE of
 * volume, floored at TRASHFS_INODES_FLOOR. Sizing by BYTES (not blocks)
 * keeps the file budget constant across block sizes — a larger block
 * does NOT reduce how many files fit. 2048 reproduces the historical
 * "1 inode per 16 blocks" budget at the 128-byte default. (mkfs can
 * override the count via an explicit hint.) */
#define TRASHFS_INODES_FLOOR   16u
#ifndef TRASHFS_BYTES_PER_INODE
#define TRASHFS_BYTES_PER_INODE 2048u
#endif
/* Derived: blocks consumed per inode-budget unit (16 @128, 4 @512). */
#define TRASHFS_BLOCKS_PER_INODE (TRASHFS_BYTES_PER_INODE / TRASHFS_BLOCK_SIZE)

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
    TRASHFS_ERR_NO_SPACE,       /* ENOSPC                            */
    TRASHFS_ERR_NOT_FOUND,      /* a path component does not exist   */
    TRASHFS_ERR_IO,             /* unexpected internal failure       */
    TRASHFS_ERR_EXISTS,         /* target name already exists        */
    TRASHFS_ERR_NOT_EMPTY,      /* rmdir on a non-empty directory    */
    TRASHFS_ERR_NOT_DIR,        /* path component isn't a directory  */
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

/* ---- Phase 2 API: read-only path --------------------------- *
 *
 *  open (existing files only — creation is Phase 3), read, lseek,
 *  readdir, close. These walk the inode block list (direct ->
 *  single -> double -> triple indirect) to map a file offset to a
 *  data block. No allocation happens here.
 */

/* Whence values for trashfs_lseek (match POSIX SEEK_*). */
#define TRASHFS_SEEK_SET 0
#define TRASHFS_SEEK_CUR 1
#define TRASHFS_SEEK_END 2

/* Open flags. Phase 2 understood none (files had to exist). Phase 3
 * adds creation and truncation. */
#define TRASHFS_O_RDONLY 0x0000u
#define TRASHFS_O_CREAT  0x0001u   /* create the file if absent       */
#define TRASHFS_O_TRUNC  0x0002u   /* truncate to zero on open        */
#define TRASHFS_O_APPEND 0x0004u   /* position at EOF before writes   */

/* An open file handle. Bound to a mounted volume + an inode. */
typedef struct {
    TrashfsVolume *vol;
    uint32_t inode;     /* inode number of the open file        */
    uint32_t size;      /* cached file size                     */
    uint32_t pos;       /* current read/write position          */
    bool     is_dir;    /* opened as a directory?               */
    bool     open;
} TrashfsFile;

/* A directory iteration handle (for readdir). */
typedef struct {
    TrashfsVolume *vol;
    uint32_t inode;     /* the directory's inode                */
    uint32_t size;      /* directory size in bytes              */
    uint32_t pos;       /* byte offset of the next entry to scan */
    bool     open;
} TrashfsDir;

/* What readdir yields to the caller. */
typedef struct {
    char     name[TRASHFS_NAME_MAX + 1]; /* NUL-terminated for ease */
    uint8_t  name_len;
    uint8_t  type;      /* TRASHFS_TYPE_FILE / TRASHFS_TYPE_DIR  */
    uint32_t inode;
    uint32_t size;      /* file size in bytes                    */
} TrashfsDirent_Out;

/* Open a file by path. Paths are '/'-separated; a leading '/' is
 * optional (resolution always starts at the root), and "." / ".."
 * components are honored ("/" or "" resolves to the root directory).
 * Intermediate components must be existing directories
 * (TRASHFS_ERR_NOT_FOUND / TRASHFS_ERR_NOT_DIR otherwise). With
 * TRASHFS_O_CREAT a missing FILE is created in its parent directory
 * (the parent must exist — open does not mkdir intermediate dirs);
 * without it, a missing file returns TRASHFS_ERR_NOT_FOUND.
 * TRASHFS_O_TRUNC frees the file's blocks and resets size to 0.
 * Fills *f. */
TrashfsResult trashfs_open(TrashfsVolume *vol, const char *name,
                           uint32_t flags, TrashfsFile *f);

/* Create a directory at `path`. The parent directory must already
 * exist (no recursive mkdir -p). Returns TRASHFS_ERR_EXISTS if the
 * name is taken, TRASHFS_ERR_NOT_FOUND/NOT_DIR if the parent path is
 * bad, TRASHFS_ERR_NO_SPACE if out of inodes/blocks. `now` stamps the
 * created/modified time (0 if no RTC). */
TrashfsResult trashfs_mkdir(TrashfsVolume *vol, const char *path, uint32_t now);

/* Remove an EMPTY directory at `path`. Returns TRASHFS_ERR_NOT_DIR if
 * `path` is a file, TRASHFS_ERR_NOT_EMPTY if it still has entries,
 * TRASHFS_ERR_NOT_FOUND if absent, TRASHFS_ERR_INVALID_ARG for the
 * root. Frees the directory's inode + blocks and clears its entry in
 * the parent. */
TrashfsResult trashfs_rmdir(TrashfsVolume *vol, const char *path);

/* Read up to n bytes at the current position. Returns the byte count
 * via *out_read (0 at EOF). Unallocated blocks within the file (holes)
 * read as zero. Advances the position. */
TrashfsResult trashfs_read(TrashfsFile *f, void *buf, uint32_t n,
                           uint32_t *out_read);

/* Write up to n bytes at the current position, allocating blocks as
 * needed (growing the file and its indirect structure) and writing in
 * place within the existing size. Returns bytes written via
 * *out_written. On insufficient space, allocates what it can and
 * returns a short count with TRASHFS_OK; if nothing could be written
 * because the volume is full, returns TRASHFS_ERR_NO_SPACE. Updates
 * size (if grown) and the modified timestamp. */
TrashfsResult trashfs_write(TrashfsFile *f, const void *buf, uint32_t n,
                            uint32_t *out_written, uint32_t now);

/* Reposition. Returns the new absolute position via *out_pos. Seeking
 * past EOF is allowed (reads there return 0 / EOF until Phase 3's
 * write can extend the file). */
TrashfsResult trashfs_lseek(TrashfsFile *f, int32_t off, int whence,
                            uint32_t *out_pos);

/* Close a file handle. */
TrashfsResult trashfs_close(TrashfsFile *f);

/* Remove a FILE by path, freeing its data blocks (and indirect blocks)
 * and its inode, and clearing its directory entry. Returns
 * TRASHFS_ERR_NOT_FOUND if absent, TRASHFS_ERR_NOT_DIR if the path
 * resolves to a directory (use trashfs_rmdir for those). */
TrashfsResult trashfs_unlink(TrashfsVolume *vol, const char *name);

/* Open the directory at `path` for iteration ("/" or "" = root).
 * Returns TRASHFS_ERR_NOT_FOUND if absent, TRASHFS_ERR_NOT_DIR if the
 * path is a file. */
TrashfsResult trashfs_opendir(TrashfsVolume *vol, const char *path,
                              TrashfsDir *d);

/* Yield the next directory entry. *out_have is set to true if an
 * entry was produced, false at end-of-directory. */
TrashfsResult trashfs_readdir(TrashfsDir *d, TrashfsDirent_Out *ent,
                              bool *out_have);

/* Close a directory handle. */
TrashfsResult trashfs_closedir(TrashfsDir *d);

#ifdef __cplusplus
}
#endif

#endif /* TRASHFS_H */
