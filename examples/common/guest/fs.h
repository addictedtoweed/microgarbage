/* ============================================================
 *  fs.h — guest file & directory API
 *
 *  Thin, freestanding wrappers over the host's filesystem syscalls
 *  (SYS_OPENAT / READ / WRITE / CLOSE / LSEEK / READDIR / MKDIRAT /
 *  UNLINKAT). The mini-libc <stdio.h> already covers buffered file
 *  content via FILE* (fopen/fread/fwrite); this header adds the
 *  lower-level fd access plus the things stdio doesn't do — directory
 *  listing, mkdir, and remove.
 *
 *  Paths resolve against the guest's mount namespace, e.g.
 *  "/host/song.wav" or "/td0/notes.txt".
 *
 *  Every call returns >= 0 on success or a NEGATIVE errno on failure
 *  (the host negates the POSIX errno: -E_NOENT, -E_PERM, ...).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef GUEST_FS_H
#define GUEST_FS_H

#include "vm_runtime.h"   /* SYS_*, _vm_sysN */
#include <stdint.h>

/* ---- open() flags (guarded so they coexist with any libc) ---- */
#ifndef O_RDONLY
#define O_RDONLY     0x000
#define O_WRONLY     0x001
#define O_RDWR       0x002
#define O_CREAT      0x040
#define O_EXCL       0x080
#define O_TRUNC      0x200
#define O_APPEND     0x400
#define O_DIRECTORY  0x10000
#endif

#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

#define FS_AT_FDCWD      (-100)   /* resolve relative to the cwd        */
#define FS_AT_REMOVEDIR  0x200    /* unlinkat flag: remove a directory  */

/* Dirent type tags (FsDirent.type). */
#define FS_DT_REG 0   /* regular file */
#define FS_DT_DIR 1   /* directory    */

/* Directory entry — layout matches the host's VmDirent. */
typedef struct {
    unsigned type;       /* FS_DT_REG / FS_DT_DIR */
    unsigned size;       /* file size in bytes (regular files)         */
    char     name[64];   /* NUL-terminated entry name                  */
} FsDirent;

/* ---- file descriptors ---- */

/* Open `path` with `flags` (O_*). Returns an fd >= 0, or -errno. */
static inline int fs_open(const char *path, int flags) {
    return (int)_vm_sys4(SYS_OPENAT, (uint32_t)FS_AT_FDCWD,
                         (uint32_t)(uintptr_t)path, (uint32_t)flags, 0);
}

/* Open for writing, creating + truncating. Returns an fd or -errno. */
static inline int fs_create(const char *path) {
    return (int)_vm_sys4(SYS_OPENAT, (uint32_t)FS_AT_FDCWD,
                         (uint32_t)(uintptr_t)path,
                         O_WRONLY | O_CREAT | O_TRUNC, 0);
}

/* Read up to `n` bytes into `buf`. Returns bytes read (0 = EOF) or -errno. */
static inline int fs_read(int fd, void *buf, unsigned n) {
    return (int)_vm_sys3(SYS_READ, (uint32_t)fd, (uint32_t)(uintptr_t)buf, n);
}

/* Write `n` bytes from `buf`. Returns bytes written or -errno. */
static inline int fs_write(int fd, const void *buf, unsigned n) {
    return (int)_vm_sys3(SYS_WRITE, (uint32_t)fd, (uint32_t)(uintptr_t)buf, n);
}

/* Reposition. `whence` is SEEK_SET/CUR/END. Returns the new offset or -errno. */
static inline int fs_lseek(int fd, int offset, int whence) {
    return (int)_vm_sys3(SYS_LSEEK, (uint32_t)fd, (uint32_t)offset, (uint32_t)whence);
}

/* Close an fd. Returns 0 or -errno. */
static inline int fs_close(int fd) {
    return (int)_vm_sys1(SYS_CLOSE, (uint32_t)fd);
}

/* ---- directory + namespace ops ---- */

/* Create a directory. Returns 0 or -errno. */
static inline int fs_mkdir(const char *path) {
    return (int)_vm_sys3(SYS_MKDIRAT, (uint32_t)FS_AT_FDCWD,
                         (uint32_t)(uintptr_t)path, 0);
}

/* Remove a file. Returns 0 or -errno. */
static inline int fs_remove(const char *path) {
    return (int)_vm_sys3(SYS_UNLINKAT, (uint32_t)FS_AT_FDCWD,
                         (uint32_t)(uintptr_t)path, 0);
}

/* Remove an (empty) directory. Returns 0 or -errno. */
static inline int fs_rmdir(const char *path) {
    return (int)_vm_sys3(SYS_UNLINKAT, (uint32_t)FS_AT_FDCWD,
                         (uint32_t)(uintptr_t)path, FS_AT_REMOVEDIR);
}

/* Open a directory for iteration. Returns an fd >= 0, or -errno. */
static inline int fs_opendir(const char *path) {
    return fs_open(path, O_RDONLY | O_DIRECTORY);
}

/* Read the next entry into *out. Returns 0 (entry filled), 1 (end of
 * directory), or -errno. Typical loop:
 *     int d = fs_opendir("/host");
 *     FsDirent e;
 *     while (fs_readdir(d, &e) == 0) { ... use e.name / e.type ... }
 *     fs_closedir(d);
 */
static inline int fs_readdir(int fd, FsDirent *out) {
    return (int)_vm_sys2(SYS_READDIR, (uint32_t)fd, (uint32_t)(uintptr_t)out);
}

/* Close a directory fd opened with fs_opendir. Returns 0 or -errno. */
static inline int fs_closedir(int fd) {
    return fs_close(fd);
}

#endif /* GUEST_FS_H */
