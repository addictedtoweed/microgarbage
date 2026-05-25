/* ============================================================
 *  vm_host_fs.h — file syscalls for guest VMs
 *
 *  Adds POSIX-shaped file operations (open, close, read, write,
 *  lseek, mkdir, unlink, readdir) to the VM's ECALL surface,
 *  backed by Elm Chan's FatFs on the host. Combined with
 *  trashdrive_fatfs this gives a guest a real read/write
 *  filesystem inside a host-managed RAM region.
 *
 *  ---------------------------------------------------------------
 *  Why bother
 *  ---------------------------------------------------------------
 *
 *  Two reasons:
 *
 *  1. Guests can persist work between runs (within the same
 *     trashdrive instance) and structure data into files/dirs
 *     rather than ad-hoc memory layouts. A "shell" guest with
 *     ls/mkdir/cat/rm becomes possible.
 *
 *  2. The syscall numbers match Linux's RISC-V generic ABI, so
 *     a guest built against picolibc (or any other libc whose
 *     file-IO wrappers emit standard Linux numbers) gets file
 *     access for free, with no per-syscall glue.
 *
 *  ---------------------------------------------------------------
 *  Mount namespace
 *  ---------------------------------------------------------------
 *
 *  Every path the guest passes to a file syscall MUST start with
 *  "/<name>/". The "<name>" segment selects a registered
 *  mount; the rest is the path within that mount's backend.
 *
 *  Bare absolute paths like "/foo.txt" are REJECTED with -ENOENT.
 *  This is deliberate: with a single anonymous root, a script
 *  written for one host can land on a different volume on another
 *  host without warning. Strict /<name>/ paths force the
 *  call site to commit to a backend.
 *
 *  Relative paths are passed to FatFs (when the cwd is on a FatFs
 *  mount) or to the host's fopen (when it's a HOST mount). The
 *  guest controls cwd via its own logic; this module just resolves
 *  whatever absolute path the syscall is given. (For HOST mounts,
 *  relative cwds are not currently supported — the shell rewrites
 *  relative paths into /<name>/<rel> before passing them.)
 *
 *  ---------------------------------------------------------------
 *  Backends
 *  ---------------------------------------------------------------
 *
 *  Two backend kinds are implemented:
 *
 *  HOST    — passthrough to a real directory on the host machine.
 *            Read-only by default; writability is a per-mount flag.
 *            ".." escapes are rejected.
 *
 *  TRASHFS — backed by a mounted trashfs volume (the native RAM
 *            disk). The mount table stores a pointer to a host-owned
 *            TrashfsVolume the caller has already formatted+mounted.
 *
 *  ---------------------------------------------------------------
 *  Installation
 *  ---------------------------------------------------------------
 *
 *      VmSystem sys;
 *      vm_system_init(&sys, &cfg);
 *      vm_host_install_stdio(&sys);     // for fds 0,1,2
 *      vm_host_install_fs(&sys);        // for fds 3+
 *
 *      // Set up at least one mount before guests run. Example: a
 *      // trashfs RAM disk as /td0 and a read-only host passthrough:
 *      static uint8_t       g_region[128 * 1024];
 *      static TrashfsVolume g_vol;
 *      trashfs_format(g_region, sizeof g_region, 0, 0);
 *      trashfs_mount(&g_vol, g_region, sizeof g_region);
 *      vm_host_fs_mount_trashfs("td0", &g_vol);
 *      vm_host_fs_mount_host   ("host", "./host_files", false);
 *
 *      vm_system_load_vm(...);
 *      vm_system_run(...);
 *
 *  The order matters: install_fs registers a delegate that
 *  install_stdio's read/write/close handlers use to route fds
 *  >= 3. Mounts can be added before or after install_fs.
 *
 *  ---------------------------------------------------------------
 *  Syscall numbers
 *  ---------------------------------------------------------------
 *
 *  See vm_ecall.h. All file syscalls live in the Linux-compat
 *  range (numbers <256) so they match what stock libc wrappers
 *  emit.
 *
 *      SYS_OPENAT    (56)   open or create a file/dir
 *      SYS_CLOSE     (57)   close a fd
 *      SYS_LSEEK     (62)   seek within an open file
 *      SYS_READ      (63)   read from fd (stdio or file)
 *      SYS_WRITE     (64)   write to fd (stdio or file)
 *      SYS_MKDIRAT   (34)   create a directory
 *      SYS_UNLINKAT  (35)   remove a file or directory
 *      SYS_READDIR   (120)  read one directory entry
 *
 *  ---------------------------------------------------------------
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_VM_HOST_FS_H
#define MICROGARBAGE_VM_HOST_FS_H

#include "vm/vm_system.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  Flag and constant definitions
 *
 *  These mirror the Linux generic ABI so the guest can use
 *  values straight out of <fcntl.h> if it has picolibc, or
 *  hard-code them otherwise. We define them here too so guests
 *  without a libc can include this header (or a copy).
 * ============================================================ */

/* dirfd value meaning "use AT_FDCWD" — the only dirfd value our
 * *at-style syscalls accept (since the VM has no concept of a
 * current working directory). */
#define VM_AT_FDCWD               (-100)

/* openat flags. Bit layout matches Linux generic. */
#define VM_O_RDONLY              0x000
#define VM_O_WRONLY              0x001
#define VM_O_RDWR                0x002
#define VM_O_ACCMODE             0x003   /* mask for the above */
#define VM_O_CREAT               0x040
#define VM_O_EXCL                0x080
#define VM_O_TRUNC               0x200
#define VM_O_APPEND              0x400
#define VM_O_DIRECTORY        0x10000

/* lseek whence values. */
#define VM_SEEK_SET   0   /* offset from start of file */
#define VM_SEEK_CUR   1   /* offset from current position */
#define VM_SEEK_END   2   /* offset from end of file */

/* unlinkat flags. */
#define VM_AT_REMOVEDIR     0x200   /* unlinkat acts like rmdir */

/* readdir entry layout. The guest passes a pointer to one of
 * these structs to SYS_READDIR; the host fills it in.
 *
 * Type values: VM_DT_REG (regular file), VM_DT_DIR (directory).
 * Other values are reserved for future use.
 *
 * `size` is meaningful only for regular files (DT_REG); for
 * directories it's 0.
 *
 * `name` is null-terminated. With FF_USE_LFN=0 (our default
 * config), names are at most 12 chars (8.3 + null), so the
 * 64-byte buffer has lots of slack for future LFN support. */
#define VM_DT_REG     0
#define VM_DT_DIR     1
#define VM_DT_OTHER   2

typedef struct {
    uint32_t type;
    uint32_t size;
    char     name[64];
} VmDirent;

/* ============================================================
 *  Install
 * ============================================================ */

/* Register the file syscall handlers on `sys`'s ECALL router.
 *
 * Pre-conditions:
 *   - sys is a valid VmSystem (vm_system_init done).
 *   - FatFs has been set up: a TrashDrive (or other block device)
 *     is registered with trash_fatfs_register, and f_mount has
 *     been called on the volume the guest will access. install_fs
 *     does NOT do these for you — they're the host's responsibility.
 *
 * Returns true on success, false if any handler registration
 * fails (typically because a handler is already installed for
 * one of these slots).
 *
 * On success:
 *   - Registers handlers for SYS_OPENAT, SYS_CLOSE, SYS_LSEEK,
 *     SYS_MKDIRAT, SYS_UNLINKAT, SYS_READDIR.
 *   - Hooks into the stdio bridge (if installed) so SYS_READ/
 *     SYS_WRITE/SYS_CLOSE on fds >= 3 are routed here. If stdio
 *     is not yet installed, install it AFTER fs and it'll pick
 *     up the hook automatically.
 *   - Initializes the file descriptor table; existing stdio fds
 *     (0/1/2) are untouched.
 */
bool vm_host_install_fs(VmSystem *sys);

/* Close any open files and reset the fd table. Safe to call
 * multiple times. Called automatically on process exit if you
 * use vm_host_install_fs_atexit (see below). */
void vm_host_fs_reset(void);

/* Convenience: like vm_host_install_fs but also registers an
 * atexit handler that closes any leaked file handles. Useful
 * for shell-style applications where the user may Ctrl-C out
 * with files open. */
bool vm_host_install_fs_atexit(VmSystem *sys);

/* ============================================================
 *  Mount table
 *
 *  Every path the guest passes to a file syscall must start with
 *  "/<name>/". The <name> looks up an entry in this
 *  table; the rest of the path is the location within the
 *  mount's backend. Two backend kinds are supported:
 *
 *    HOST    — passes through to a real directory on the host's
 *              OS filesystem. ".." escapes are rejected. Read-only
 *              by default; pass writable=true to allow creates.
 *
 *    TRASHFS — backed by a mounted trashfs volume (the native RAM
 *              disk). The mount stores a pointer to the host-owned
 *              TrashfsVolume the caller has already mounted.
 *
 *  Names must match [A-Za-z0-9_-]{1,15} and be unique across
 *  the table. There's no enforced order; mounts are looked up
 *  by name. The table size cap is VM_HOST_FS_MAX_MOUNTS.
 *
 *  mount_* / unmount_* calls don't touch the underlying backend or
 *  open files. Closing open fds on unmount is the caller's
 *  responsibility — the cleanest sequence is unload_all_vms() ->
 *  unmount_all().
 * ============================================================ */

#ifndef VM_HOST_FS_MAX_MOUNTS
#define VM_HOST_FS_MAX_MOUNTS 8
#endif

/* Register a host-directory passthrough mount.
 *
 *   name      Mount point — guest sees this as /<name>/.
 *             Limited to [A-Za-z0-9_-]{1,15}.
 *   root      Absolute host path to a directory. The string is
 *             copied internally; caller can free after.
 *   writable  If true, allows O_WRONLY/O_CREAT/O_TRUNC on files
 *             under this mount. If false, all writes return
 *             -EROFS.
 *
 * Returns true on success; false on invalid name, full table,
 * duplicate name, or root not a directory. */
bool vm_host_fs_mount_host(const char *name,
                           const char *root,
                           bool writable);

/* Mount a trashfs volume under <name>. The 'vol' argument is a
 * pointer to a mounted TrashfsVolume (passed as void* to avoid
 * pulling storage/trashfs.h into this header). Stored as-is; the
 * volume must outlive this mount. Guests reach it through the same
 * openat/read/write/lseek/readdir/unlinkat ABI as any other mount —
 * trashfs is just a third backend behind this seam.
 *
 * Returns true on success; false on invalid name, full table,
 * duplicate name, or NULL volume. */
bool vm_host_fs_mount_trashfs(const char *name, void *vol);

/* Remove a mount by name. Returns true if the mount existed and
 * was removed; false if no such mount. Doesn't touch the
 * underlying backend (the trashfs volume stays mounted; the host
 * directory is left alone). */
bool vm_host_fs_unmount(const char *name);

/* Tear down all mounts. Doesn't touch the underlying backends. */
void vm_host_fs_unmount_all(void);

/* Query the number of currently-registered mounts. */
unsigned vm_host_fs_mount_count(void);

/* ============================================================
 *  Spawn (run another VM from inside the running shell)
 *
 *  SYS_SPAWN_AND_WAIT lets a guest load and run another ELF as
 *  a new VM in the same system, then wait for it to halt.
 *
 *  The spawned VM is loaded with VM_BACKING_COPY_RAM — its code
 *  and rodata segments are copied out of the source file into
 *  fresh RAM from the host's bump arena. This means:
 *
 *    - The ELF file's storage doesn't need to be contiguous or
 *      stable. FatFs-on-trashdrive (scattered sectors) and host-
 *      filesystem-on-disk (entirely outside our address space)
 *      both work fine — the bytes are copied as they're read.
 *
 *    - Each spawn consumes RAM from the bump arena. Plan for
 *      ~20-30 KB per spawned VM (code + rodata + data region).
 *      The arena is sized by vm_system's local_storage_size.
 *
 *  The parent VM is paused while the spawned VM runs — its
 *  scheduler slot stays in place but its quantum doesn't tick.
 *  When the spawned VM halts (SYS_EXIT), control returns to the
 *  parent and the syscall returns the exit code.
 *
 *  The spawned VM shares the parent's stdio. Anything it writes
 *  to fd 1 appears on the user's terminal. This matches the
 *  "foreground process" model from a shell — the spawned VM is
 *  conceptually the only thing running until it exits.
 *
 *  Configuration: vm_host_fs_set_spawn_data_size() controls how
 *  much data-region RAM each spawned VM gets. Default is 16 KB.
 * ============================================================ */

/* Set the data-region size (in bytes) given to each spawned VM.
 * Must be at least 4 KB and a multiple of 4 KB. Returns true on
 * success. The new size applies to subsequent spawns; in-flight
 * spawns are unaffected. */
bool vm_host_fs_set_spawn_data_size(uint32_t bytes);

/* Get the currently-configured spawn data-region size. */
uint32_t vm_host_fs_get_spawn_data_size(void);

/* ============================================================
 *  Diagnostics
 *
 *  Mostly useful for tests. May be removed/changed without
 *  notice in future versions.
 * ============================================================ */

/* Number of currently-open files (not counting stdio fds 0/1/2). */
unsigned vm_host_fs_open_count(void);

/* Maximum simultaneous open files supported by this module.
 * Compile-time fixed at VM_HOST_FS_MAX_FILES (default 16). */
unsigned vm_host_fs_max_files(void);

/* ============================================================
 *  File-fd routing for alternate transports
 *
 *  Hosts that install their own SYS_READ/SYS_WRITE/SYS_CLOSE
 *  handlers (instead of using vm_host_stdio's) should call these
 *  for fds >= 3. They route to FatFs or to the host-passthrough
 *  mount as appropriate.
 *
 *  Returns:
 *    > 0   bytes transferred (read/write only)
 *    = 0   end of file (read), zero-length op accepted
 *    < 0   -errno on failure
 *
 *  The default vm_host_stdio bridge calls these internally;
 *  you only need them if you've REPLACED that bridge.
 * ============================================================ */

int32_t vm_host_fs_route_read (int fd, void *buf, uint32_t n);
int32_t vm_host_fs_route_write(int fd, const void *buf, uint32_t n);
int32_t vm_host_fs_route_close(int fd);

/* As above, but returns a sentinel (VM_HOST_FS_NOT_OURS) when
 * the fd is not in the file-fd range (i.e., < 3). Caller is
 * expected to handle stdio routing itself in that case. */
#define VM_HOST_FS_NOT_OURS  (-12345678)

#ifndef VM_HOST_FS_MAX_FILES
#define VM_HOST_FS_MAX_FILES 16
#endif

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_VM_HOST_FS_H */
