# trashfs — on-disk format specification (draft)

A small, purpose-built filesystem for the **internal RAM disk** (and
FMC/PSRAM-mapped RAM disks). It is deliberately *not* FAT-compatible,
so it is not for removable, PC-readable media (SD/USB) — bring your
own FAT driver if you need that. trashfs is for always-resident,
never-removed storage where FAT's 512-byte sectors, 8.3 names, and
LFN code size are pure overhead.

Status: **format LOCKED, not yet implemented.** All design decisions
are settled (see "Resolved design decisions" below). This document is
the authoritative on-disk layout reference; implementation follows it.

## Goals and decisions (locked)

- **Block size is a build-time knob (`TRASHFS_BLOCK_SIZE`, default
  128 B)** — 128 is 4× finer than FAT's 512, so small files and log
  records waste little; 512 cuts indirection and per-file metadata for
  volumes holding larger objects (e.g. guest ELFs). Must be a power of
  two and a multiple of the 64-byte inode size. The block size is
  recorded in the superblock and mount rejects a mismatch. (Inode and
  dirent sizes stay locked at 64 and 48 bytes.)
- **uint32 block indices** — 4 GB addressable (2^32 × ... no: 2^32
  blocks would be huge; the practical ceiling is volume size). Chosen
  over uint16 because real FMC SDRAM parts (e.g. 256 Mbit = 32 MB)
  already exceed the 8 MB a uint16 block index would reach.
- **inode + multi-level block list**: 8 direct + single + double +
  triple indirect. Big-file capability is *lazy* — tiny files use
  only direct pointers; the indirect machinery costs nothing until a
  file actually grows into it.
- **inode table sized as a fraction of the volume**, not fixed, so the
  same format works on a 16 KB internal disk and a 32 MB SDRAM disk.
  An optional mkfs hint overrides the default for known workloads.
- **created + modified timestamps**, 32-bit Unix seconds. The RTC
  fetch is stubbed initially (returns 0); wired to the platform RTC
  later.
- **free-block bitmap**, 1 bit/block.
- **flat namespace** to start (single directory). The directory-entry
  type field leaves room to add subdirectories later without a format
  break.
- **ENOSPC when full** — no filesystem-level ring buffering. Log
  wraparound is the application's job (pre-allocate a fixed-size file,
  write records in place).
- **lseek + in-place overwrite** fully supported (the pre-allocate
  idiom needs it). Block allocation is stable: once a file owns a
  block, in-place writes only change block *contents*.
- Designed so an **ftruncate-style preallocate** (allocate N blocks,
  set size, don't touch contents) drops in later with no format
  change. Not implemented now (no ABI syscall for it yet).
- Slots in behind the existing `vm_host_fs.c` backend seam as a third
  backend (`PATH_BACKEND_TRASHFS`) — **no guest ABI change**. Guests
  keep calling openat/read/write/lseek/readdir/unlinkat.

## All integers are little-endian

To match RISC-V guest byte order and the host platforms; no byte
swapping on read/write.

## Region layout

A trashfs volume is a flat byte region (RAM or PSRAM). From offset 0:

```
+-------------------+  block 0
|   Superblock      |  1 block (128 B). Magic, geometry, region
|                   |  offsets, root-dir inode, free count.
+-------------------+  block 1
|   Free bitmap     |  ceil(total_blocks / 8 / 128) blocks.
|                   |  1 bit per block; bit set = allocated.
+-------------------+
|   Inode table     |  inode_blocks blocks (see sizing formula).
|                   |  Fixed-size inodes, packed.
+-------------------+
|   Data blocks     |  the rest. Holds file data, directory data,
|                   |  and indirect-pointer blocks, all 128 B each.
+-------------------+  block total_blocks-1
```

The superblock records the start block and length of each region, so
`trashfs_mount` is fully data-driven — it never assumes offsets, it
reads them. That also lets the sizing formula change in future
versions without breaking the mount path.

## Superblock (block 0, 128 bytes)

```
offset size  field
0      4     magic            0x54524653  ("TRFS")
4      2     version_major    1
6      2     version_minor    0
8      4     block_size       128 (bytes); validated on mount
12     4     total_blocks     volume_bytes / block_size
16     4     bitmap_start     block index of the free bitmap (=1)
20     4     bitmap_blocks    blocks the bitmap occupies
24     4     inode_start      block index of the inode table
28     4     inode_blocks     blocks the inode table occupies
32     4     inode_count      total inodes (inode_blocks * inodes/block)
36     4     data_start       first data block index
40     4     root_inode       inode number of the root directory (=0)
44     4     free_blocks      cached count of free data blocks
48     4     free_inodes      cached count of free inodes
52     4     flags            reserved (0)
56     4     created          volume mkfs time (Unix sec; 0 if no RTC)
60     68    reserved         zeroed; room for future fields
```

`free_blocks`/`free_inodes` are caches for fast "is there room"; the
bitmap and inode table are the source of truth and are re-scanned on
mount to rebuild the caches (cheap, and self-healing if a cache was
left stale by an interrupted op).

## Inode (64 bytes, 2 per block)

```
offset size  field
0      2     mode      type + perms. bit layout:
                         bit 0    in use (0 = free inode)
                         bit 1    directory (vs regular file)
                         bits 2.. reserved (future perms/flags)
2      2     links     reference count (1 for a normal file; room
                       for hardlinks later, though flat ns won't use)
4      4     size      file size in bytes (uint32; max 4 GB)
8      4     created   Unix seconds (0 until RTC wired)
12     4     modified  Unix seconds (updated on write/truncate)
16     4 x 8 direct[8] 8 direct block pointers -> 8*128 = 1 KB
48     4     single    -> block of 32 ptrs -> 32*128 = 4 KB
52     4     double    -> 32 blocks of 32 ptrs -> 128 KB
56     4     triple    -> 32*32*32 blocks -> 4 MB
60     4     reserved  zeroed
```

A null block pointer is the value 0 (block 0 is the superblock, never
a data block, so 0 is unambiguously "none").

**Max file size:** 1 KB (direct) + 4 KB (single) + 128 KB (double) +
4 MB (triple) = **4,198,400 bytes (~4.0 MB)** per file. On volumes
smaller than a given indirect level's reach, those pointers simply
stay null — no overhead. On a 16 KB volume the whole disk is smaller
than the single-indirect reach, so only direct + maybe single ever
get used; double/triple are dormant capability.

A block of pointers holds 128 / 4 = **32 uint32 pointers**.

## Directory entry (48 bytes, 2 per block — LOCKED)

The root directory is just a regular inode (root_inode) whose data
blocks hold a packed array of directory entries.

```
offset size  field
0      4     inode      inode number; 0 = empty/deleted slot
4      1     type       0 = regular file, 1 = directory (future)
5      1     name_len   1..32
6      32    name       UTF-8, NOT NUL-terminated; name_len gives length
38     10    reserved   zeroed; padding to 48 + room for future fields
```

48 bytes per entry, so **2 entries per 128-byte directory block**.
Full **32-character** names stored inline (the goal), with 10 bytes of
zeroed padding/reserve rounding the entry to 48.

The 16 bytes of overhead per entry beyond a hypothetical minimal entry
only costs *directory* blocks, never file data: a directory holding
100 files is 100 x 48 = 4800 bytes (~38 blocks), trivial on any volume
large enough to hold 100 files.

## Free-block bitmap

One bit per block in the whole volume (including metadata blocks, so
block indices line up 1:1 with bit positions — simpler than only
mapping data blocks). Bit set = allocated. Allocation scans for the
first clear bit at/after a rotating hint; freeing clears the bit and
bumps `free_blocks`.

Size: `ceil(total_blocks / 8)` bytes, rounded up to whole blocks.
- 16 KB volume = 128 blocks -> 16 bytes -> 1 block.
- 32 MB volume = 262,144 blocks -> 32,768 bytes -> 256 blocks (0.1%).

## Inode-table sizing formula

The table scales with volume so tiny disks aren't starved and big
disks aren't capped. Default:

```
inode_count = max(16, total_blocks / 16)
inode_blocks = ceil(inode_count * 64 / 128) = ceil(inode_count / 2)
```

i.e. roughly **one inode per 16 data blocks**, floored at 16 inodes.
mkfs accepts an optional `inode_count` hint to override (a logging
disk holding 3 huge files asks for few inodes and reclaims the space;
a config disk with many tiny files asks for more).

Worked examples:

| volume | total_blocks | inodes (default) | inode_blocks | bitmap_blocks | metadata total | data blocks | % usable |
|--------|--------------|------------------|--------------|---------------|----------------|-------------|----------|
| 16 KB  | 128          | 16 (floor)       | 8            | 1             | 1+1+8 = 10     | 118         | ~92%     |
| 32 KB  | 256          | 16               | 8            | 1             | 10             | 246         | ~96%     |
| 64 KB  | 512          | 32               | 16           | 1             | 18             | 494         | ~96%     |
| 1 MB   | 8192         | 512              | 256          | 1             | 258            | 7934        | ~97%     |
| 32 MB  | 262144       | 16384            | 8192         | 256           | 8449           | 253695      | ~97%     |

The 16 KB floor case is the tightest at ~92% usable, which still
guarantees ≥16 files — the agreed balanced default.

## Operations the format must serve (the backend API)

trashfs implements the function set `vm_host_fs.c` calls into for a
read/write backend:

```
trashfs_mount(vol, region, region_bytes)      validate superblock, rebuild caches
trashfs_format(vol, region, region_bytes, hint)  mkfs: lay out regions
trashfs_open(vol, path, flags) -> handle      O_CREAT/O_TRUNC/O_APPEND honored
trashfs_read(h, buf, n) -> bytes
trashfs_write(h, buf, n) -> bytes             grows file (allocates blocks) or
                                              writes in place within size
trashfs_lseek(h, off, whence) -> pos
trashfs_close(h)
trashfs_readdir(dirhandle, &entry) -> 1/0/err
trashfs_unlink(vol, path)                     free blocks + inode, clear dir slot
trashfs_mkdir(vol, path)                      (deferred until subdirs exist)
```

In-place write (offset < size) walks the block list to the right
block and overwrites; growth (offset >= size) allocates new blocks,
appends them to the list (direct -> single -> double -> triple as
needed), and bumps size + modified.

## Resolved design decisions

1. **Directory entry size:** 48-byte entries, true 32-char names
   (2 per block). Locked.
2. **Block-pointer 0 = "none":** confirmed. Block 0 is always the
   superblock and can never be a data block, so a pointer value of 0
   unambiguously means "no block" — no separate valid flag needed.
3. **No per-file app-flags byte.** An app that needs to tag a file
   stores the tag inside its own file; the filesystem stays unopinion-
   ated. If a real need for inode-level app metadata ever appears, it
   can be carved from the inode `reserved` field (offset 60) — mkfs
   zeroes it, so old volumes read as "no flags," fully backward-
   compatible with no format version bump.
4. **Per-file metadata:** name, size, type, created, modified. No
   more (timestamps already cover the logging use case).

## Deliberately deferred (no format impact)

- Subdirectories (type field reserved; flat namespace for now).
- ftruncate/preallocate syscall (format is ready; ABI add later).
- RTC wiring (timestamp fields present; fetch stubbed).
- Crash consistency / journaling — a RAM disk is volatile, so power-
  loss durability is moot; if trashfs ever backs battery-backed PSRAM
  and that matters, it's a future version bump.
