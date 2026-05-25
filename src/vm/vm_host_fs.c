/* ============================================================
 *  vm_host_fs.c — implementation of file syscalls on top of FatFs
 *
 *  Build dependencies:
 *    -Iinclude                          for our public headers
 *    -Ithird_party/fatfs/source         for ff.h
 *    -Ithird_party/fatfs                for ffconf.h
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm/vm_host_fs.h"
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_transport.h"
#include "vm/vm_loader.h"
#include "vm/vm_sched.h"
#include "vm/vm_system.h"
#include "vm/host_compat.h"

/* FatFs and POSIX <dirent.h> both define a type named DIR. We
 * include dirent.h here for host-mount directory listing; remap
 * FatFs's name to FFDIR via a tiny preprocessor dance so both
 * coexist. The remap only affects this translation unit. */
#define DIR FFDIR
#include "ff.h"
#undef DIR

#include "storage/trashfs.h"

#include <dirent.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* ============================================================
 *  FS serialization (preemptive backend)
 *
 *  This module keeps process-global state: the fd table (g_fds),
 *  the mount table, and FatFs's own internals. Under the cooperative
 *  scheduler one host thread touches all of it, so no locking is
 *  needed. Under the preemptive scheduler, peer VM tasks run in
 *  separate threads and can issue file syscalls concurrently — so we
 *  serialize every FS entry point with one coarse mutex (FS is not a
 *  hot path; correctness over parallelism). Cooperative builds compile
 *  these to nothing and pull in no pthread dependency.
 *
 *  NOTE: SYS_SPAWN_AND_WAIT must NOT hold this lock across its wait
 *  for the child — it locks only the child-ELF load (see the handler).
 * ============================================================ */
#if GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE
#include <pthread.h>
static pthread_mutex_t g_fs_mtx = PTHREAD_MUTEX_INITIALIZER;
static inline void fs_lock(void)   { pthread_mutex_lock(&g_fs_mtx); }
static inline void fs_unlock(void) { pthread_mutex_unlock(&g_fs_mtx); }
#else
static inline void fs_lock(void)   { }
static inline void fs_unlock(void) { }
#endif

/* ============================================================
 *  File descriptor table
 *
 *  Slots 0,1,2 are reserved for stdin/stdout/stderr (managed by
 *  vm_host_stdio.c). Our slots run from 3 to 3+MAX_FILES-1.
 *
 *  Each slot can be either a regular file (FIL) or an open
 *  directory (DIR). FatFs's FIL and DIR are different types so
 *  we tag each slot with a `kind` and union the storage.
 * ============================================================ */

#define FD_BASE    3
#define FD_LIMIT   (FD_BASE + VM_HOST_FS_MAX_FILES)

/* Maximum length of a resolved host path. Referenced both by the
 * FdSlot union (for SLOT_HOST_DIR's cached dir path) and by the
 * path-resolution code further down. */
#define VM_HOST_FS_MAX_PATH 256

typedef enum {
    SLOT_FREE = 0,
    SLOT_FILE,        /* FatFs file (u.file) */
    SLOT_DIR,         /* FatFs directory (u.dir) */
    SLOT_HOST_FILE,   /* Host-filesystem file (u.host) */
    SLOT_ROOT,        /* Synthetic root listing — yields mount names */
    SLOT_HOST_DIR,    /* Host-filesystem directory (u.host_dir) */
    SLOT_TRASH_FILE,  /* trashfs file (u.tfile) */
    SLOT_TRASH_DIR,   /* trashfs directory (u.tdir) */
} SlotKind;

typedef struct {
    SlotKind kind;
    bool     writable;    /* HOST/FATFS: was the mount writable at open? */
    union {
        FIL    file;
        FFDIR  dir;
        FILE  *host;       /* stdio FILE* for SLOT_HOST_FILE */
        struct {
            unsigned cursor;   /* index into the mount table */
        } root;
        struct {
            DIR *dir;          /* POSIX opendir handle */
            /* Cached resolved host path of the directory, used to
             * build per-entry full paths for stat lookups. Includes
             * a trailing '/'. */
            char path[VM_HOST_FS_MAX_PATH];
            size_t path_len;
        } host_dir;
        TrashfsFile tfile;     /* SLOT_TRASH_FILE */
        TrashfsDir  tdir;      /* SLOT_TRASH_DIR  */
    } u;
} FdSlot;

static FdSlot g_fds[VM_HOST_FS_MAX_FILES];

/* Allocate a free fd slot. Returns the fd (3..FD_LIMIT-1) or -1
 * if the table is full. */
static int alloc_fd(SlotKind kind) {
    for (unsigned i = 0; i < VM_HOST_FS_MAX_FILES; i++) {
        if (g_fds[i].kind == SLOT_FREE) {
            g_fds[i].kind = kind;
            return (int)(FD_BASE + i);
        }
    }
    return -1;
}

/* Convert a guest fd to a slot pointer. Returns NULL if the fd
 * is out of range or the slot is free. */
static FdSlot *slot_for(int fd, SlotKind expect_kind) {
    if (fd < FD_BASE || fd >= FD_LIMIT) return NULL;
    FdSlot *s = &g_fds[fd - FD_BASE];
    if (s->kind == SLOT_FREE) return NULL;
    if (expect_kind != SLOT_FREE && s->kind != expect_kind) return NULL;
    return s;
}

static void free_slot(FdSlot *s) {
    memset(s, 0, sizeof(*s));
    /* kind = SLOT_FREE = 0 from memset. */
}

/* ============================================================
 *  Path handling
 *
 *  Every absolute guest path must look like "/<name>/...".
 *  The <name> is the mount name and is looked up in the mount
 *  table; <...> is the path within that mount's backend. Paths
 *  that don't start with a registered mount name return -ENOENT.
 *
 *  Listing "/" returns a synthetic directory containing one
 *  entry per registered mount.
 *
 *  Mount kinds:
 *    HOST  — passthrough to a directory on the host's OS fs.
 *            Composes the resolved host path by appending the
 *            <...> portion to the mount's host_root. ".." escapes
 *            are rejected.
 *    FATFS — passes the resolved path to FatFs in volume-prefixed
 *            form ("N:/foo/bar"). The mount stores the FatFs
 *            volume number. Today's `tmpfs` and `sd` config types
 *            both map to FATFS internally; they differ in intent
 *            (volatile RAM vs. persistent block device on
 *            hardware), not in implementation on the dev host.
 * ============================================================ */

/* Mount table. M.3a supports up to VM_HOST_FS_MAX_MOUNTS entries
 * (default 8). The order of entries doesn't matter; lookup is by
 * name. */
typedef enum {
    MOUNT_KIND_FREE   = 0,    /* slot is empty (memset state) */
    MOUNT_KIND_HOST   = 1,
    MOUNT_KIND_FATFS  = 2,
    MOUNT_KIND_TRASHFS= 3,
} MountKind;

#define MOUNT_NAME_MAX 15    /* names fit in [A-Za-z0-9_-]{1,15} */

typedef struct {
    MountKind kind;
    char      name[MOUNT_NAME_MAX + 1];

    /* HOST fields */
    char    *host_root;       /* malloc'd, no trailing slash */
    size_t   host_root_len;
    bool     writable;        /* only meaningful for HOST */

    /* FATFS fields */
    uint8_t  fatfs_volume;    /* FatFs pdrv number */
    FATFS   *fatfs_struct;    /* host-owned, may be NULL */

    /* TRASHFS fields */
    TrashfsVolume *trashfs_vol; /* host-owned mounted volume */
} Mount;

static Mount    g_mounts[VM_HOST_FS_MAX_MOUNTS];
static unsigned g_mount_count = 0;

/* Which backend a path was routed to. */
typedef enum {
    PATH_BACKEND_FATFS = 0,
    PATH_BACKEND_HOST  = 1,
    PATH_BACKEND_TRASHFS = 2,
} PathBackend;

/* Lookup a mount by name. Returns NULL if not found. */
static const Mount *find_mount(const char *name) {
    for (unsigned i = 0; i < VM_HOST_FS_MAX_MOUNTS; i++) {
        if (g_mounts[i].kind != MOUNT_KIND_FREE &&
            strcmp(g_mounts[i].name, name) == 0) {
            return &g_mounts[i];
        }
    }
    return NULL;
}

/* Find a free slot. Returns NULL if the table is full. */
static Mount *find_free_mount_slot(void) {
    for (unsigned i = 0; i < VM_HOST_FS_MAX_MOUNTS; i++) {
        if (g_mounts[i].kind == MOUNT_KIND_FREE) return &g_mounts[i];
    }
    return NULL;
}

/* Validate a mount name: 1..15 chars, [A-Za-z0-9_-]. The naming
 * rule is restrictive on purpose — mount names appear in paths
 * the guest can pass to syscalls, and we want unambiguous
 * separator handling, no traversal trickery, and a name that
 * fits in a fixed-size buffer without dynamic alloc. */
static bool valid_mount_name(const char *name) {
    if (!name) return false;
    size_t n = strlen(name);
    if (n == 0 || n > MOUNT_NAME_MAX) return false;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* Copy a null-terminated guest path into `out`. Returns 0 on
 * success, -errno on failure. Used by resolve_guest_path and
 * by the root-path detection in handle_openat. */
static int copy_guest_path(VmCpu *cpu, uint32_t guest_addr,
                           char *out, size_t cap) {
    size_t pos = 0;
    for (;;) {
        if (pos >= cap - 1) return -VM_ENAMETOOLONG;
        const char *b = vm_translate_read(cpu, guest_addr + (uint32_t)pos, 1);
        if (!b) return -VM_EFAULT;
        out[pos] = *b;
        if (*b == '\0') break;
        pos++;
    }
    return 0;
}

/* Returns true if `path` refers to the synthetic root listing.
 * Accepts "/", "/.", or empty (so guests written by humans
 * don't trip over edge cases). */
static bool is_root_path(const char *path) {
    if (path[0] == '\0') return true;
    if (path[0] == '/' && path[1] == '\0') return true;
    if (path[0] == '/' && path[1] == '.' && path[2] == '\0') return true;
    return false;
}

/* Resolve a guest path of the form "/<name>/<rest>" into
 * a host-usable absolute path, plus metadata about which backend
 * to route to.
 *
 * Inputs:
 *   cpu, guest_addr   the guest-side null-terminated path
 *   out, cap          buffer for the translated path
 *
 * Outputs (on success):
 *   *out_backend      PATH_BACKEND_HOST or PATH_BACKEND_FATFS
 *   *out_writable     true if the mount allows writes
 *
 * Returns 0 on success, or -errno on failure:
 *   -EFAULT          guest pointer not readable
 *   -ENAMETOOLONG    guest path too long, or translated path
 *                    overflows `out`
 *   -ENOENT          path doesn't start with /<name>/ or
 *                    <name> isn't a registered mount
 *   -EPERM           ".." escape attempt in a HOST mount path
 *   -EINVAL          buffer too small to be usable
 */
static int resolve_guest_path(VmCpu *cpu, uint32_t guest_addr,
                              char *out, size_t cap,
                              PathBackend *out_backend,
                              bool *out_writable,
                              const Mount **out_mount) {
    if (cap < 16) return -VM_EINVAL;
    if (out_mount) *out_mount = NULL;

    /* Stage 1: copy the raw guest path into a scratch buffer. */
    char raw[VM_HOST_FS_MAX_PATH];
    int cgr = copy_guest_path(cpu, guest_addr, raw, sizeof(raw));
    if (cgr < 0) return cgr;

    /* Stage 2: every path must start with '/'. */
    if (raw[0] != '/') {
        return -VM_ENOENT;
    }

    /* Stage 3: extract the mount name — chars after the leading
     * '/' up to the next '/' or end of string. Then look it up. */
    const char *mount_start = raw + 1;
    const char *mount_end = mount_start;
    while (*mount_end != '\0' && *mount_end != '/') mount_end++;
    size_t name_len = (size_t)(mount_end - mount_start);
    if (name_len == 0 || name_len > MOUNT_NAME_MAX) return -VM_ENOENT;

    char name[MOUNT_NAME_MAX + 1];
    memcpy(name, mount_start, name_len);
    name[name_len] = '\0';

    const Mount *m = find_mount(name);
    if (!m) return -VM_ENOENT;

    /* `rel` is the path WITHIN the mount, starting with '/' or
     * empty. "/foo" alone -> rel = "/" (mount root). */
    const char *rel = mount_end;
    if (*rel == '\0') rel = "/";   /* canonicalize "/foo" */

    /* Stage 4: backend-specific composition. */
    if (m->kind == MOUNT_KIND_HOST) {
        /* Reject .. components anywhere in rel. */
        if (strstr(rel, "/..") != NULL ||
            (rel[0] == '.' && rel[1] == '.' &&
             (rel[2] == '\0' || rel[2] == '/'))) {
            return -VM_EPERM;
        }

        size_t rel_len = strlen(rel);
        if (m->host_root_len + rel_len + 1 > cap) return -VM_ENAMETOOLONG;
        memcpy(out, m->host_root, m->host_root_len);
        memcpy(out + m->host_root_len, rel, rel_len + 1);   /* +null */

        *out_backend  = PATH_BACKEND_HOST;
        *out_writable = m->writable;
        if (out_mount) *out_mount = m;
        return 0;
    }

    if (m->kind == MOUNT_KIND_TRASHFS) {
        /* Flat namespace: the "path" is just the filename within the
         * mount (rel without its leading slash). The volume is
         * reached via *out_mount. Reject any subdirectory component
         * for now (no subdirs yet). */
        const char *fname = rel;
        if (*fname == '/') fname++;
        /* No nested paths in the flat namespace. */
        if (strchr(fname, '/') != NULL) return -VM_ENOENT;
        size_t fl = strlen(fname);
        if (fl + 1 > cap) return -VM_ENAMETOOLONG;
        memcpy(out, fname, fl + 1);   /* may be "" for the mount root */

        *out_backend  = PATH_BACKEND_TRASHFS;
        *out_writable = true;
        if (out_mount) *out_mount = m;
        return 0;
    }

    /* FATFS: emit "<volume>:<rel>". The volume number is one
     * digit (FF_VOLUMES <= 10 in our build). */
    size_t rel_len = strlen(rel);
    /* '<digit>' + ':' + rel + null = 2 + rel_len + 1 */
    if (rel_len + 3 > cap) return -VM_ENAMETOOLONG;
    out[0] = (char)('0' + m->fatfs_volume);
    out[1] = ':';
    memcpy(out + 2, rel, rel_len + 1);     /* includes null */

    *out_backend  = PATH_BACKEND_FATFS;
    *out_writable = true;     /* FatFs writability is per-mount-or-not;
                                 * for now all FatFs mounts are r/w. */
    if (out_mount) *out_mount = m;
    return 0;
}

/* ============================================================
 *  FRESULT → errno mapping
 * ============================================================ */

static int32_t fres_to_errno(FRESULT r) {
    switch (r) {
        case FR_OK:                  return 0;
        case FR_DISK_ERR:            return -VM_EIO;
        case FR_INT_ERR:             return -VM_EIO;
        case FR_NOT_READY:           return -VM_EIO;
        case FR_NO_FILE:             return -VM_ENOENT;
        case FR_NO_PATH:             return -VM_ENOENT;
        case FR_INVALID_NAME:        return -VM_ENAMETOOLONG;
        /* FatFs returns FR_INVALID_NAME both for syntactically bad
         * names and (more often, with LFN off) for names that don't
         * fit the 8.3 limit. EINVAL would be more accurate for the
         * former but ENAMETOOLONG is more useful for the latter,
         * which is what users actually hit. */
        case FR_DENIED:              return -VM_EPERM;
        case FR_EXIST:               return -VM_EEXIST;
        case FR_INVALID_OBJECT:      return -VM_EBADF;
        case FR_WRITE_PROTECTED:     return -VM_EROFS;
        case FR_INVALID_DRIVE:       return -VM_ENOENT;
        case FR_NOT_ENABLED:         return -VM_EIO;
        case FR_NO_FILESYSTEM:       return -VM_ENOENT;
        case FR_MKFS_ABORTED:        return -VM_EIO;
        case FR_TIMEOUT:             return -VM_ETIMEDOUT;
        case FR_LOCKED:              return -VM_EBUSY;
        case FR_NOT_ENOUGH_CORE:     return -VM_ENOMEM;
        case FR_TOO_MANY_OPEN_FILES: return -VM_EMFILE;
        case FR_INVALID_PARAMETER:   return -VM_EINVAL;
        default:                     return -VM_EIO;
    }
}

/* ============================================================
 *  Open-flag translation
 *
 *  Map our (Linux-compat) VM_O_* flags onto FatFs's FA_* mode
 *  bits. Reject combinations that don't make sense (e.g.,
 *  O_TRUNC without write access).
 * ============================================================ */

static int translate_open_flags(uint32_t flags, BYTE *out_mode) {
    BYTE mode = 0;
    uint32_t access = flags & VM_O_ACCMODE;

    if (access == VM_O_RDONLY)      mode |= FA_READ;
    else if (access == VM_O_WRONLY) mode |= FA_WRITE;
    else if (access == VM_O_RDWR)   mode |= FA_READ | FA_WRITE;
    else return -VM_EINVAL;

    if (flags & VM_O_CREAT) {
        if (flags & VM_O_EXCL)      mode |= FA_CREATE_NEW;
        else if (flags & VM_O_TRUNC) mode |= FA_CREATE_ALWAYS;
        else                         mode |= FA_OPEN_ALWAYS;
    } else {
        if (flags & VM_O_TRUNC)     return -VM_EINVAL;
        mode |= FA_OPEN_EXISTING;
    }

    /* APPEND we handle after-the-fact (f_lseek to end after open). */

    *out_mode = mode;
    return 0;
}

/* ============================================================
 *  Read/write/close shared with stdio
 *
 *  These are called either directly (when fs is installed and
 *  stdio isn't) or via the stdio delegate hook (when both are
 *  installed). They handle ONLY file fds (>= 3) — stdio fds are
 *  not our problem.
 *
 *  Dispatch is by slot kind: SLOT_FILE goes through FatFs's
 *  f_read/f_write/f_close, SLOT_HOST_FILE through native stdio's
 *  fread/fwrite/fclose.
 * ============================================================ */

/* Returns bytes read, -errno on error, or VM_HOST_FS_NOT_OURS if
 * fd is not a file fd (caller should fall back to other handling).
 * Internal alias VM_FS_NOT_OURS retained for in-file readability. */
#define VM_FS_NOT_OURS  VM_HOST_FS_NOT_OURS

static int32_t fs_read_fd(int fd, void *buf, uint32_t n) {
    if (fd < FD_BASE) return VM_FS_NOT_OURS;
    if (fd >= FD_LIMIT) return -VM_EBADF;
    FdSlot *s = &g_fds[fd - FD_BASE];

    if (s->kind == SLOT_FILE) {
        UINT br = 0;
        FRESULT r = f_read(&s->u.file, buf, n, &br);
        if (r != FR_OK) return fres_to_errno(r);
        return (int32_t)br;
    }
    if (s->kind == SLOT_HOST_FILE) {
        size_t br = fread(buf, 1, n, s->u.host);
        if (br < n && ferror(s->u.host)) return -VM_EIO;
        return (int32_t)br;
    }
    if (s->kind == SLOT_TRASH_FILE) {
        uint32_t got = 0;
        TrashfsResult r = trashfs_read(&s->u.tfile, buf, n, &got);
        if (r != TRASHFS_OK) return -VM_EIO;
        return (int32_t)got;
    }
    return -VM_EBADF;
}

static int32_t fs_write_fd(int fd, const void *buf, uint32_t n) {
    if (fd < FD_BASE) return VM_FS_NOT_OURS;
    if (fd >= FD_LIMIT) return -VM_EBADF;
    FdSlot *s = &g_fds[fd - FD_BASE];

    if (s->kind == SLOT_FILE) {
        UINT bw = 0;
        FRESULT r = f_write(&s->u.file, buf, n, &bw);
        if (r != FR_OK) return fres_to_errno(r);
        return (int32_t)bw;
    }
    if (s->kind == SLOT_HOST_FILE) {
        if (!s->writable) return -VM_EROFS;
        size_t bw = fwrite(buf, 1, n, s->u.host);
        if (bw < n) return -VM_EIO;
        return (int32_t)bw;
    }
    if (s->kind == SLOT_TRASH_FILE) {
        uint32_t wrote = 0;
        /* now=0: RTC not yet wired; timestamps stay 0 (see spec). */
        TrashfsResult r = trashfs_write(&s->u.tfile, buf, n, &wrote, 0);
        if (r == TRASHFS_ERR_NO_SPACE) return -VM_ENOSPC;
        if (r != TRASHFS_OK) return -VM_EIO;
        return (int32_t)wrote;
    }
    return -VM_EBADF;
}

static int32_t fs_close_fd(int fd) {
    if (fd < FD_BASE) return VM_FS_NOT_OURS;
    if (fd >= FD_LIMIT) return -VM_EBADF;
    FdSlot *s = &g_fds[fd - FD_BASE];
    if (s->kind == SLOT_FREE) return -VM_EBADF;

    int32_t result = 0;
    if (s->kind == SLOT_FILE) {
        FRESULT r = f_close(&s->u.file);
        if (r != FR_OK) result = fres_to_errno(r);
    } else if (s->kind == SLOT_DIR) {
        FRESULT r = f_closedir(&s->u.dir);
        if (r != FR_OK) result = fres_to_errno(r);
    } else if (s->kind == SLOT_HOST_FILE) {
        if (fclose(s->u.host) != 0) result = -VM_EIO;
    } else if (s->kind == SLOT_HOST_DIR) {
        if (closedir(s->u.host_dir.dir) != 0) result = -VM_EIO;
    } else if (s->kind == SLOT_TRASH_FILE) {
        trashfs_close(&s->u.tfile);
    } else if (s->kind == SLOT_TRASH_DIR) {
        trashfs_closedir(&s->u.tdir);
    } else if (s->kind == SLOT_ROOT) {
        /* No underlying resource; nothing to free beyond the slot. */
    }
    free_slot(s);
    return result;
}

/* Hook function-pointer types. Used both for the static fs_*_fd
 * functions below and for the vm_host_stdio_set_fs_hooks setter
 * we call at install time. Keep these definitions in sync with
 * the ones in vm_host_stdio.c. */
typedef int32_t (*vm_host_fs_read_hook_t)(int, void *, uint32_t);
typedef int32_t (*vm_host_fs_write_hook_t)(int, const void *, uint32_t);
typedef int32_t (*vm_host_fs_close_hook_t)(int);

/* ============================================================
 *  Syscall handlers
 * ============================================================ */

/* SYS_OPENAT
 *
 *   a0 = dirfd        (must be VM_AT_FDCWD = -100)
 *   a1 = path (guest ptr to null-terminated string)
 *   a2 = flags        (VM_O_*)
 *   a3 = mode         (unused; permissions don't apply to FAT)
 *   → a0 = fd on success, -errno on failure
 */
static void handle_openat(VmCpu *cpu, void *system) {
    (void)system;
    int32_t dirfd  = (int32_t)cpu->regs[VM_REG_A0];
    uint32_t path  = cpu->regs[VM_REG_A1];
    uint32_t flags = cpu->regs[VM_REG_A2];
    /* a3 = mode, ignored */

    if (dirfd != VM_AT_FDCWD) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
        return;
    }

    /* Special case: opening "/" (or "/." or "") as a directory
     * returns a synthetic SLOT_ROOT fd whose readdir yields one
     * entry per registered mount. Required so `ls /` works.
     * Opening "/" as a file is -EISDIR. */
    char raw_path[VM_HOST_FS_MAX_PATH];
    int cgr = copy_guest_path(cpu, path, raw_path, sizeof(raw_path));
    if (cgr < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)cgr;
        return;
    }
    if (is_root_path(raw_path)) {
        if (!(flags & VM_O_DIRECTORY)) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EISDIR;
            return;
        }
        if ((flags & VM_O_ACCMODE) != VM_O_RDONLY) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EISDIR;
            return;
        }
        int fd = alloc_fd(SLOT_ROOT);
        if (fd < 0) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EMFILE;
            return;
        }
        FdSlot *s = &g_fds[fd - FD_BASE];
        s->u.root.cursor = 0;
        cpu->regs[VM_REG_A0] = (uint32_t)fd;
        return;
    }

    char buf[VM_HOST_FS_MAX_PATH];
    PathBackend backend;
    bool writable;
    const Mount *mnt = NULL;
    int rp = resolve_guest_path(cpu, path, buf, sizeof(buf),
                                 &backend, &writable, &mnt);
    if (rp < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)rp;
        return;
    }

    /* ---- Host-filesystem path ---- */
    if (backend == PATH_BACKEND_HOST) {
        if (flags & VM_O_DIRECTORY) {
            /* Open directory on host fs via POSIX opendir. */
            if ((flags & VM_O_ACCMODE) != VM_O_RDONLY) {
                cpu->regs[VM_REG_A0] = (uint32_t)-VM_EISDIR;
                return;
            }
            DIR *d = opendir(buf);
            if (!d) {
                switch (errno) {
                    case ENOENT:  cpu->regs[VM_REG_A0] = (uint32_t)-VM_ENOENT;  break;
                    case ENOTDIR: cpu->regs[VM_REG_A0] = (uint32_t)-VM_ENOTDIR; break;
                    case EACCES:  cpu->regs[VM_REG_A0] = (uint32_t)-VM_EPERM;   break;
                    default:      cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;     break;
                }
                return;
            }
            int fd = alloc_fd(SLOT_HOST_DIR);
            if (fd < 0) {
                closedir(d);
                cpu->regs[VM_REG_A0] = (uint32_t)-VM_EMFILE;
                return;
            }
            FdSlot *s = &g_fds[fd - FD_BASE];
            s->writable = writable;
            s->u.host_dir.dir = d;
            /* Cache the resolved host path so readdir can stat
             * each entry for size. Include a trailing '/' so we
             * can just append d_name. If the resolved path
             * already ends with '/', don't double up. */
            size_t pl = strlen(buf);
            if (pl >= sizeof(s->u.host_dir.path) - 2) {
                closedir(d);
                free_slot(s);
                cpu->regs[VM_REG_A0] = (uint32_t)-VM_ENAMETOOLONG;
                return;
            }
            memcpy(s->u.host_dir.path, buf, pl);
            if (pl > 0 && s->u.host_dir.path[pl - 1] != '/' &&
                          s->u.host_dir.path[pl - 1] != '\\') {
                s->u.host_dir.path[pl++] = '/';
            }
            s->u.host_dir.path[pl] = '\0';
            s->u.host_dir.path_len = pl;
            cpu->regs[VM_REG_A0] = (uint32_t)fd;
            return;
        }
        /* Reject write-class opens against a read-only mount. */
        uint32_t access = flags & VM_O_ACCMODE;
        bool wants_write = (access != VM_O_RDONLY) ||
                           (flags & (VM_O_CREAT | VM_O_TRUNC | VM_O_APPEND));
        if (wants_write && !writable) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EROFS;
            return;
        }

        /* Translate VM_O_* flags to stdio fopen mode string. */
        const char *mode_str = NULL;
        if (access == VM_O_RDONLY)         mode_str = "rb";
        else if (flags & VM_O_APPEND)      mode_str = "ab+";
        else if (flags & VM_O_TRUNC)       mode_str = "wb+";
        else if (flags & VM_O_CREAT)       mode_str = "ab+";  /* close to OPEN_ALWAYS */
        else if (access == VM_O_WRONLY)    mode_str = "rb+";
        else                               mode_str = "rb+";

        int fd = alloc_fd(SLOT_HOST_FILE);
        if (fd < 0) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EMFILE;
            return;
        }
        FdSlot *s = &g_fds[fd - FD_BASE];
        s->writable = writable;
        FILE *f = fopen(buf, mode_str);
        if (!f) {
            free_slot(s);
            switch (errno) {
                case ENOENT:  cpu->regs[VM_REG_A0] = (uint32_t)-VM_ENOENT;  break;
                case EACCES:  cpu->regs[VM_REG_A0] = (uint32_t)-VM_EPERM;   break;
                case EEXIST:  cpu->regs[VM_REG_A0] = (uint32_t)-VM_EEXIST;  break;
                case EISDIR:  cpu->regs[VM_REG_A0] = (uint32_t)-VM_EISDIR;  break;
                case ENOTDIR: cpu->regs[VM_REG_A0] = (uint32_t)-VM_ENOTDIR; break;
                default:      cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;     break;
            }
            return;
        }
        s->u.host = f;
        cpu->regs[VM_REG_A0] = (uint32_t)fd;
        return;
    }

    /* ---- trashfs path ---- */
    if (backend == PATH_BACKEND_TRASHFS) {
        TrashfsVolume *vol = mnt ? mnt->trashfs_vol : NULL;
        if (!vol) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO; return; }

        if (flags & VM_O_DIRECTORY) {
            if ((flags & VM_O_ACCMODE) != VM_O_RDONLY) {
                cpu->regs[VM_REG_A0] = (uint32_t)-VM_EISDIR;
                return;
            }
            int fd = alloc_fd(SLOT_TRASH_DIR);
            if (fd < 0) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EMFILE; return; }
            FdSlot *s = &g_fds[fd - FD_BASE];
            /* buf is the resolved mount-relative path; trashfs_opendir is
             * now path-aware (it used to ignore it and always open root). */
            TrashfsResult tr = trashfs_opendir(vol, buf, &s->u.tdir);
            if (tr != TRASHFS_OK) {
                free_slot(s);
                int e = (tr == TRASHFS_ERR_NOT_FOUND) ? -VM_ENOENT
                      : (tr == TRASHFS_ERR_NOT_DIR)   ? -VM_ENOTDIR
                      : -VM_EIO;
                cpu->regs[VM_REG_A0] = (uint32_t)e;
                return;
            }
            cpu->regs[VM_REG_A0] = (uint32_t)fd;
            return;
        }

        /* Map VM_O_* to trashfs flags. */
        uint32_t tflags = 0;
        if (flags & VM_O_CREAT)  tflags |= TRASHFS_O_CREAT;
        if (flags & VM_O_TRUNC)  tflags |= TRASHFS_O_TRUNC;
        if (flags & VM_O_APPEND) tflags |= TRASHFS_O_APPEND;

        int fd = alloc_fd(SLOT_TRASH_FILE);
        if (fd < 0) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EMFILE; return; }
        FdSlot *s = &g_fds[fd - FD_BASE];
        TrashfsResult tr = trashfs_open(vol, buf, tflags, &s->u.tfile);
        if (tr != TRASHFS_OK) {
            free_slot(s);
            int e = (tr == TRASHFS_ERR_NOT_FOUND) ? -VM_ENOENT
                  : (tr == TRASHFS_ERR_NO_SPACE)  ? -VM_ENOSPC
                  : (tr == TRASHFS_ERR_INVALID_ARG) ? -VM_EINVAL
                  : -VM_EIO;
            cpu->regs[VM_REG_A0] = (uint32_t)e;
            return;
        }
        cpu->regs[VM_REG_A0] = (uint32_t)fd;
        return;
    }

    /* ---- FatFs path ---- */

    if (flags & VM_O_DIRECTORY) {
        /* Open directory for readdir. Flags other than O_DIRECTORY
         * + O_RDONLY are rejected — you can't write to a dir. */
        if ((flags & VM_O_ACCMODE) != VM_O_RDONLY) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EISDIR;
            return;
        }
        int fd = alloc_fd(SLOT_DIR);
        if (fd < 0) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EMFILE;
            return;
        }
        FdSlot *s = &g_fds[fd - FD_BASE];
        FRESULT r = f_opendir(&s->u.dir, buf);
        if (r != FR_OK) {
            free_slot(s);
            cpu->regs[VM_REG_A0] = (uint32_t)fres_to_errno(r);
            return;
        }
        cpu->regs[VM_REG_A0] = (uint32_t)fd;
        return;
    }

    /* Regular file open. */
    BYTE mode = 0;
    int rc = translate_open_flags(flags, &mode);
    if (rc < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)rc;
        return;
    }

    int fd = alloc_fd(SLOT_FILE);
    if (fd < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EMFILE;
        return;
    }
    FdSlot *s = &g_fds[fd - FD_BASE];
    FRESULT r = f_open(&s->u.file, buf, mode);
    if (r != FR_OK) {
        free_slot(s);
        cpu->regs[VM_REG_A0] = (uint32_t)fres_to_errno(r);
        return;
    }

    /* If O_APPEND was requested, seek to end. */
    if (flags & VM_O_APPEND) {
        FSIZE_t end = f_size(&s->u.file);
        FRESULT sr = f_lseek(&s->u.file, end);
        if (sr != FR_OK) {
            f_close(&s->u.file);
            free_slot(s);
            cpu->regs[VM_REG_A0] = (uint32_t)fres_to_errno(sr);
            return;
        }
    }

    cpu->regs[VM_REG_A0] = (uint32_t)fd;
}

/* SYS_CLOSE
 *
 *   a0 = fd
 *   → a0 = 0 on success, -errno on failure
 */
static void handle_close(VmCpu *cpu, void *system) {
    (void)system;
    int fd = (int)cpu->regs[VM_REG_A0];
    int32_t r = fs_close_fd(fd);
    if (r == VM_FS_NOT_OURS) {
        /* fd 0/1/2 — close is a no-op success. */
        if (fd >= 0 && fd <= 2) {
            cpu->regs[VM_REG_A0] = 0;
        } else {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF;
        }
        return;
    }
    cpu->regs[VM_REG_A0] = (uint32_t)r;
}

/* SYS_LSEEK
 *
 *   a0 = fd
 *   a1 = offset (signed 32-bit; we don't support >2GB files anyway)
 *   a2 = whence (SEEK_SET / SEEK_CUR / SEEK_END)
 *   → a0 = new file position, or -errno
 */
static void handle_lseek(VmCpu *cpu, void *system) {
    (void)system;
    int fd      = (int)cpu->regs[VM_REG_A0];
    int32_t off = (int32_t)cpu->regs[VM_REG_A1];
    uint32_t whence = cpu->regs[VM_REG_A2];

    if (fd < FD_BASE || fd >= FD_LIMIT) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF;
        return;
    }
    FdSlot *s = &g_fds[fd - FD_BASE];

    /* Host-filesystem fd: route through native fseek/ftell. */
    if (s->kind == SLOT_HOST_FILE) {
        int w;
        switch (whence) {
            case VM_SEEK_SET: w = SEEK_SET; break;
            case VM_SEEK_CUR: w = SEEK_CUR; break;
            case VM_SEEK_END: w = SEEK_END; break;
            default:
                cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
                return;
        }
        if (fseek(s->u.host, off, w) != 0) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
            return;
        }
        long pos = ftell(s->u.host);
        if (pos < 0) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;
            return;
        }
        cpu->regs[VM_REG_A0] = (uint32_t)pos;
        return;
    }

    /* trashfs fd path. */
    if (s->kind == SLOT_TRASH_FILE) {
        int w;
        switch (whence) {
            case VM_SEEK_SET: w = TRASHFS_SEEK_SET; break;
            case VM_SEEK_CUR: w = TRASHFS_SEEK_CUR; break;
            case VM_SEEK_END: w = TRASHFS_SEEK_END; break;
            default:
                cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
                return;
        }
        uint32_t newpos = 0;
        TrashfsResult r = trashfs_lseek(&s->u.tfile, off, w, &newpos);
        if (r != TRASHFS_OK) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
            return;
        }
        cpu->regs[VM_REG_A0] = newpos;
        return;
    }

    /* FatFs fd path. */
    if (s->kind != SLOT_FILE) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF;
        return;
    }

    FSIZE_t base;
    switch (whence) {
        case VM_SEEK_SET: base = 0; break;
        case VM_SEEK_CUR: base = f_tell(&s->u.file); break;
        case VM_SEEK_END: base = f_size(&s->u.file); break;
        default:
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
            return;
    }

    /* Resulting position must be non-negative. */
    if (off < 0 && (FSIZE_t)(-off) > base) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
        return;
    }
    FSIZE_t new_pos = base + (FSIZE_t)off;

    FRESULT r = f_lseek(&s->u.file, new_pos);
    if (r != FR_OK) {
        cpu->regs[VM_REG_A0] = (uint32_t)fres_to_errno(r);
        return;
    }
    cpu->regs[VM_REG_A0] = (uint32_t)new_pos;
}

/* SYS_MKDIRAT
 *
 *   a0 = dirfd (must be VM_AT_FDCWD)
 *   a1 = path
 *   a2 = mode (ignored)
 *   → a0 = 0 on success, -errno on failure
 */
static void handle_mkdirat(VmCpu *cpu, void *system) {
    (void)system;
    int32_t dirfd = (int32_t)cpu->regs[VM_REG_A0];
    uint32_t path = cpu->regs[VM_REG_A1];

    if (dirfd != VM_AT_FDCWD) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
        return;
    }

    char buf[VM_HOST_FS_MAX_PATH];
    PathBackend backend;
    bool writable;
    const Mount *mnt = NULL;
    int p = resolve_guest_path(cpu, path, buf, sizeof(buf),
                               &backend, &writable, &mnt);
    if (p < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)p;
        return;
    }

    if (backend == PATH_BACKEND_HOST) {
        /* The /host passthrough is a read-only view. */
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EROFS;
        return;
    }

    if (backend == PATH_BACKEND_TRASHFS) {
        TrashfsVolume *vol = mnt ? mnt->trashfs_vol : NULL;
        if (!vol) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO; return; }
        TrashfsResult r = trashfs_mkdir(vol, buf, 0);
        int e = (r == TRASHFS_OK)             ? 0
              : (r == TRASHFS_ERR_EXISTS)     ? -VM_EEXIST
              : (r == TRASHFS_ERR_NOT_FOUND)  ? -VM_ENOENT
              : (r == TRASHFS_ERR_NOT_DIR)    ? -VM_ENOTDIR
              : (r == TRASHFS_ERR_NO_SPACE)   ? -VM_ENOSPC
              : (r == TRASHFS_ERR_INVALID_ARG)? -VM_EINVAL
              : -VM_EIO;
        cpu->regs[VM_REG_A0] = (uint32_t)e;
        return;
    }

    FRESULT r = f_mkdir(buf);
    cpu->regs[VM_REG_A0] = (uint32_t)fres_to_errno(r);
}

/* SYS_UNLINKAT
 *
 *   a0 = dirfd (must be VM_AT_FDCWD)
 *   a1 = path
 *   a2 = flags (0 or VM_AT_REMOVEDIR; FatFs's f_unlink handles both)
 *   → a0 = 0 on success, -errno on failure
 */
static void handle_unlinkat(VmCpu *cpu, void *system) {
    (void)system;
    int32_t  dirfd = (int32_t)cpu->regs[VM_REG_A0];
    uint32_t path  = cpu->regs[VM_REG_A1];
    uint32_t flags = cpu->regs[VM_REG_A2];

    if (dirfd != VM_AT_FDCWD) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
        return;
    }

    char buf[VM_HOST_FS_MAX_PATH];
    PathBackend backend;
    bool writable;
    const Mount *mnt = NULL;
    int p = resolve_guest_path(cpu, path, buf, sizeof(buf),
                               &backend, &writable, &mnt);
    if (p < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)p;
        return;
    }

    if (backend == PATH_BACKEND_HOST) {
        /* Host passthrough unlink isn't supported (read-only view). */
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EROFS;
        return;
    }

    if (backend == PATH_BACKEND_TRASHFS) {
        TrashfsVolume *vol = mnt ? mnt->trashfs_vol : NULL;
        if (!vol) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO; return; }
        /* AT_REMOVEDIR -> rmdir (dirs only); otherwise unlink (files
         * only — trashfs_unlink refuses a directory). */
        TrashfsResult r = (flags & VM_AT_REMOVEDIR)
                        ? trashfs_rmdir(vol, buf)
                        : trashfs_unlink(vol, buf);
        int e = (r == TRASHFS_OK)             ? 0
              : (r == TRASHFS_ERR_NOT_FOUND)  ? -VM_ENOENT
              : (r == TRASHFS_ERR_NOT_DIR)    ? -VM_ENOTDIR
              : (r == TRASHFS_ERR_NOT_EMPTY)  ? -VM_ENOTEMPTY
              : (r == TRASHFS_ERR_INVALID_ARG)? -VM_EINVAL
              : -VM_EIO;
        cpu->regs[VM_REG_A0] = (uint32_t)e;
        return;
    }

    /* FatFs's f_unlink works for both files and empty directories;
     * the AT_REMOVEDIR flag distinction doesn't apply. */
    FRESULT r = f_unlink(buf);
    cpu->regs[VM_REG_A0] = (uint32_t)fres_to_errno(r);
}

/* SYS_READDIR
 *
 *   a0 = fd (must be an open directory fd from openat with O_DIRECTORY)
 *   a1 = pointer to VmDirent in guest memory
 *   → a0 = 0 on success (entry filled in)
 *       1 if no more entries (signal end of directory)
 *       -errno on error
 */
static void handle_readdir(VmCpu *cpu, void *system) {
    (void)system;
    int fd            = (int)cpu->regs[VM_REG_A0];
    uint32_t dirent_p = cpu->regs[VM_REG_A1];

    /* slot_for with SLOT_FREE means "any kind"; we then branch. */
    FdSlot *s = slot_for(fd, SLOT_FREE);
    if (!s) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF;
        return;
    }

    VmDirent *out = vm_translate_write(cpu, dirent_p, sizeof(VmDirent));
    if (!out) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
        return;
    }

    if (s->kind == SLOT_ROOT) {
        /* Walk forward through the mount table, returning one
         * entry per registered (non-free) mount. */
        while (s->u.root.cursor < VM_HOST_FS_MAX_MOUNTS) {
            unsigned i = s->u.root.cursor++;
            if (g_mounts[i].kind == MOUNT_KIND_FREE) continue;
            memset(out, 0, sizeof(VmDirent));
            out->type = VM_DT_DIR;
            out->size = 0;
            size_t name_max = sizeof(out->name) - 1;
            strncpy(out->name, g_mounts[i].name, name_max);
            out->name[name_max] = '\0';
            cpu->regs[VM_REG_A0] = 0;
            return;
        }
        cpu->regs[VM_REG_A0] = 1;   /* end of directory */
        return;
    }

    if (s->kind == SLOT_HOST_DIR) {
        errno = 0;
        struct dirent *de = readdir(s->u.host_dir.dir);
        if (!de) {
            if (errno == 0) {
                cpu->regs[VM_REG_A0] = 1;   /* end */
            } else {
                cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;
            }
            return;
        }
        /* Skip "." and ".." for the guest's view of the mount —
         * neither is meaningful in a sandbox. */
        while (de && (strcmp(de->d_name, ".") == 0 ||
                       strcmp(de->d_name, "..") == 0)) {
            errno = 0;
            de = readdir(s->u.host_dir.dir);
        }
        if (!de) {
            if (errno == 0) cpu->regs[VM_REG_A0] = 1;
            else            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;
            return;
        }
        memset(out, 0, sizeof(VmDirent));
        /* Determine type. d_type is available on Linux but not
         * always reliable; fall back to noting "unknown" as REG. */
#ifdef DT_DIR
        if (de->d_type == DT_DIR)      out->type = VM_DT_DIR;
        else if (de->d_type == DT_REG) out->type = VM_DT_REG;
        else                           out->type = VM_DT_REG;
#else
        out->type = VM_DT_REG;
#endif
        /* Stat the entry to fill in size. Build the full path
         * via the cached dir prefix + d_name. Truncated paths
         * just leave size at 0. */
        size_t dn_len = strlen(de->d_name);
        if (s->u.host_dir.path_len + dn_len + 1 <=
            sizeof(s->u.host_dir.path)) {
            char tmp[VM_HOST_FS_MAX_PATH];
            memcpy(tmp, s->u.host_dir.path, s->u.host_dir.path_len);
            memcpy(tmp + s->u.host_dir.path_len, de->d_name, dn_len);
            tmp[s->u.host_dir.path_len + dn_len] = '\0';
            struct stat st;
            if (stat(tmp, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    out->type = VM_DT_DIR;
                    out->size = 0;
                } else if (S_ISREG(st.st_mode)) {
                    out->type = VM_DT_REG;
                    out->size = (uint32_t)st.st_size;
                }
            }
        }
        /* Bounded copy: filenames > 63 chars get truncated, but
         * the destination is always null-terminated. */
        size_t name_max = sizeof(out->name) - 1;
        size_t n = strlen(de->d_name);
        if (n > name_max) n = name_max;
        memcpy(out->name, de->d_name, n);
        out->name[n] = '\0';
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    if (s->kind == SLOT_TRASH_DIR) {
        TrashfsDirent_Out e;
        bool have = false;
        TrashfsResult r = trashfs_readdir(&s->u.tdir, &e, &have);
        if (r != TRASHFS_OK) {
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;
            return;
        }
        if (!have) {
            cpu->regs[VM_REG_A0] = 1;   /* end of directory */
            return;
        }
        memset(out, 0, sizeof(VmDirent));
        out->type = (e.type == TRASHFS_TYPE_DIR) ? VM_DT_DIR : VM_DT_REG;
        out->size = e.size;
        size_t name_max = sizeof(out->name) - 1;
        size_t n = e.name_len;
        if (n > name_max) n = name_max;
        memcpy(out->name, e.name, n);
        out->name[n] = '\0';
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    if (s->kind != SLOT_DIR) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF;
        return;
    }

    FILINFO fi;
    FRESULT r = f_readdir(&s->u.dir, &fi);
    if (r != FR_OK) {
        cpu->regs[VM_REG_A0] = (uint32_t)fres_to_errno(r);
        return;
    }

    /* End-of-directory: FatFs signals this by returning an entry
     * whose name starts with '\0'. */
    if (fi.fname[0] == '\0') {
        cpu->regs[VM_REG_A0] = 1;
        return;
    }

    /* Fill in the VmDirent. fi.fname is at most 12 bytes with
     * LFN off; copy with bound. */
    memset(out, 0, sizeof(VmDirent));
    if (fi.fattrib & AM_DIR) {
        out->type = VM_DT_DIR;
        out->size = 0;
    } else {
        out->type = VM_DT_REG;
        out->size = (uint32_t)fi.fsize;
    }
    size_t name_max = sizeof(out->name) - 1;
    strncpy(out->name, fi.fname, name_max);
    out->name[name_max] = '\0';

    cpu->regs[VM_REG_A0] = 0;
}

/* ============================================================
 *  Mount table API
 * ============================================================ */

/* Strip trailing slashes (forward and back) from a path. Leaves
 * at least one character; "/" stays "/". Returns the trimmed
 * length. Operates on a fresh copy so the caller's buffer is
 * untouched. */
static size_t copy_and_strip_trailing_slashes(const char *src,
                                              char *dst, size_t cap) {
    size_t len = strlen(src);
    if (len + 1 > cap) return 0;
    memcpy(dst, src, len);
    while (len > 1 && (dst[len - 1] == '/' || dst[len - 1] == '\\')) len--;
    dst[len] = '\0';
    return len;
}

bool vm_host_fs_mount_host(const char *name, const char *root,
                           bool writable) {
    if (!valid_mount_name(name) || !root) return false;
    if (find_mount(name)) return false;     /* duplicate */

    /* Validate that the directory actually exists. */
    struct stat st;
    if (stat(root, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;

    Mount *m = find_free_mount_slot();
    if (!m) return false;                   /* table full */

    /* Copy the root path with trailing slash stripped. */
    char tmp[VM_HOST_FS_MAX_PATH];
    size_t rlen = copy_and_strip_trailing_slashes(root, tmp, sizeof(tmp));
    if (rlen == 0) return false;

    char *root_copy = malloc(rlen + 1);
    if (!root_copy) return false;
    memcpy(root_copy, tmp, rlen + 1);

    /* Commit. Note: name length already validated in valid_mount_name. */
    memset(m, 0, sizeof(*m));
    m->kind = MOUNT_KIND_HOST;
    memcpy(m->name, name, strlen(name) + 1);
    m->host_root     = root_copy;
    m->host_root_len = rlen;
    m->writable      = writable;
    g_mount_count++;
    return true;
}

bool vm_host_fs_mount_fatfs(const char *name, uint8_t pdrv,
                            void *fatfs) {
    if (!valid_mount_name(name)) return false;
    if (find_mount(name)) return false;
    /* FF_VOLUMES upper bound — we can't easily import that here
     * without dragging ffconf into the header. Trust the caller
     * for now; FatFs will refuse pdrv values out of range when
     * we hand it the volume-prefixed path. */
    if (pdrv > 9) return false;             /* keeps the prefix one digit */

    Mount *m = find_free_mount_slot();
    if (!m) return false;

    memset(m, 0, sizeof(*m));
    m->kind = MOUNT_KIND_FATFS;
    memcpy(m->name, name, strlen(name) + 1);
    m->fatfs_volume = pdrv;
    m->fatfs_struct = (FATFS *)fatfs;
    m->writable     = true;     /* FatFs mounts are always r/w for now */
    g_mount_count++;
    return true;
}

bool vm_host_fs_mount_trashfs(const char *name, void *vol) {
    if (!valid_mount_name(name)) return false;
    if (find_mount(name)) return false;
    if (!vol) return false;

    Mount *m = find_free_mount_slot();
    if (!m) return false;

    memset(m, 0, sizeof(*m));
    m->kind        = MOUNT_KIND_TRASHFS;
    memcpy(m->name, name, strlen(name) + 1);
    m->trashfs_vol = (TrashfsVolume *)vol;
    m->writable    = true;
    g_mount_count++;
    return true;
}

bool vm_host_fs_unmount(const char *name) {
    for (unsigned i = 0; i < VM_HOST_FS_MAX_MOUNTS; i++) {
        if (g_mounts[i].kind != MOUNT_KIND_FREE &&
            strcmp(g_mounts[i].name, name) == 0) {
            if (g_mounts[i].kind == MOUNT_KIND_HOST) {
                free(g_mounts[i].host_root);
            }
            memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
            g_mount_count--;
            return true;
        }
    }
    return false;
}

void vm_host_fs_unmount_all(void) {
    for (unsigned i = 0; i < VM_HOST_FS_MAX_MOUNTS; i++) {
        if (g_mounts[i].kind == MOUNT_KIND_HOST) {
            free(g_mounts[i].host_root);
        }
        memset(&g_mounts[i], 0, sizeof(g_mounts[i]));
    }
    g_mount_count = 0;
}

unsigned vm_host_fs_mount_count(void) {
    return g_mount_count;
}

/* ============================================================
 *  Spawn configuration and handler
 * ============================================================ */

/* Per-spawned-VM data region size. The shell example might want
 * a larger value than 16 KB if it spawns guests that allocate a
 * lot; smaller embedded systems might dial it down. */
static uint32_t g_spawn_data_size = 16 * 1024;

bool vm_host_fs_set_spawn_data_size(uint32_t bytes) {
    if (bytes < 4096 || (bytes & 4095) != 0) return false;
    g_spawn_data_size = bytes;
    return true;
}

uint32_t vm_host_fs_get_spawn_data_size(void) {
    return g_spawn_data_size;
}

/* Load an entire file into a freshly-malloc'd buffer. Works for
 * both FatFs and host-filesystem paths. Returns:
 *   - On success: a malloc'd buffer; *out_size set; caller frees.
 *   - On failure: NULL; out_errno set to a positive errno.
 *
 * The path string passed in is the already-resolved host-side
 * path (e.g., "0:/foo.elf" for FatFs or "C:/host_files/foo.elf"
 * for the host mount).
 */
static uint8_t *slurp_file(const char *path, PathBackend backend,
                           const Mount *mnt,
                           size_t *out_size, int32_t *out_err) {
    if (backend == PATH_BACKEND_HOST) {
        FILE *f = fopen(path, "rb");
        if (!f) { *out_err = VM_ENOENT; return NULL; }
        if (fseek(f, 0, SEEK_END) != 0) { fclose(f); *out_err = VM_EIO; return NULL; }
        long sz = ftell(f);
        if (sz < 0) { fclose(f); *out_err = VM_EIO; return NULL; }
        if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); *out_err = VM_EIO; return NULL; }
        uint8_t *buf = malloc((size_t)sz);
        if (!buf) { fclose(f); *out_err = VM_ENOMEM; return NULL; }
        size_t r = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        if (r != (size_t)sz) { free(buf); *out_err = VM_EIO; return NULL; }
        *out_size = (size_t)sz;
        return buf;
    } else if (backend == PATH_BACKEND_TRASHFS) {
        TrashfsVolume *vol = mnt ? mnt->trashfs_vol : NULL;
        if (!vol) { *out_err = VM_EIO; return NULL; }
        TrashfsFile tf;
        TrashfsResult tr = trashfs_open(vol, path, 0, &tf);
        if (tr != TRASHFS_OK) {
            *out_err = (tr == TRASHFS_ERR_NOT_FOUND) ? VM_ENOENT : VM_EIO;
            return NULL;
        }
        size_t sz = tf.size;
        uint8_t *buf = malloc(sz ? sz : 1);
        if (!buf) { trashfs_close(&tf); *out_err = VM_ENOMEM; return NULL; }
        uint32_t got = 0;
        tr = trashfs_read(&tf, buf, (uint32_t)sz, &got);
        trashfs_close(&tf);
        if (tr != TRASHFS_OK || got != sz) { free(buf); *out_err = VM_EIO; return NULL; }
        *out_size = sz;
        return buf;
    } else {
        FIL f;
        FRESULT r = f_open(&f, path, FA_READ);
        if (r != FR_OK) { *out_err = -fres_to_errno(r); return NULL; }
        FSIZE_t sz = f_size(&f);
        uint8_t *buf = malloc((size_t)sz);
        if (!buf) { f_close(&f); *out_err = VM_ENOMEM; return NULL; }
        UINT br;
        r = f_read(&f, buf, (UINT)sz, &br);
        f_close(&f);
        if (r != FR_OK || br != sz) { free(buf); *out_err = VM_EIO; return NULL; }
        *out_size = (size_t)sz;
        return buf;
    }
}

/* SYS_SPAWN_AND_WAIT
 *
 *   a0 = path (guest ptr to null-terminated string)
 *   → a0 = exit code of spawned VM (0..255 from its SYS_EXIT),
 *          or -errno on failure
 *
 * Loads the file at `path` as a new VM in the same VmSystem, then
 * BLOCKS the parent on the child and returns to the scheduler — the
 * spawn is ASYNCHRONOUS (see "Round V" below). The host's main
 * vm_system_step loop runs the child alongside every other VM, so
 * spawning a long-lived program in one session does NOT freeze the
 * others (this is what makes concurrent multi-session apps work). The
 * parent resumes with the child's exit code when the child is reaped
 * (vm_system_reap_halted_children). From the parent guest's view the
 * call still looks synchronous: it returns only once the child exits.
 */
static void handle_spawn_and_wait(VmCpu *cpu, void *system) {
    VmSystem *sys = (VmSystem *)system;
    uint32_t path_addr = cpu->regs[VM_REG_A0];

    char buf[VM_HOST_FS_MAX_PATH];
    PathBackend backend;
    bool writable;
    const Mount *mnt = NULL;

    /* Lock the FS only for the path resolve + ELF load. We must drop
     * it before parking on the child (below), or the child — which
     * may itself do file I/O — would deadlock waiting for this lock. */
    fs_lock();
    int rp = resolve_guest_path(cpu, path_addr, buf, sizeof(buf),
                                 &backend, &writable, &mnt);
    if (rp < 0) {
        fs_unlock();
        cpu->regs[VM_REG_A0] = (uint32_t)rp;
        return;
    }

    /* Slurp the whole ELF into RAM. */
    size_t elf_size = 0;
    int32_t err = 0;
    uint8_t *elf = slurp_file(buf, backend, mnt, &elf_size, &err);
    fs_unlock();
    if (!elf) {
        cpu->regs[VM_REG_A0] = (uint32_t)(-err);
        return;
    }

    /* Determine the spawn data region size for the child.
     * Prefer sys->config.spawn_data_kb (the new path); fall back
     * to the deprecated global if the host didn't migrate. */
    uint32_t spawn_data_bytes = (uint32_t)sys->config.spawn_data_kb * 1024u;
    if (spawn_data_bytes == 0) {
        spawn_data_bytes = g_spawn_data_size;
    }

    /* Load as a new VM. VM_BACKING_COPY_RAM means the loader
     * copies the bytes it needs out of our buffer, so we can
     * free the buffer after vm_system_load_vm returns.
     *
     * Per-allocation freeing via the slab means we can unload
     * the child cleanly on halt — see vm_system_unload_vm at
     * the bottom of this handler. */
    VmLoadVmResult lr = vm_system_load_vm(sys, elf, elf_size,
                                          spawn_data_bytes,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    free(elf);

    if (lr.code != VM_SYS_OK) {
        /* Load failure: nothing committed — vm_system_load_vm
         * does its own cleanup on partial failure. Just report
         * and return. */
        int32_t e;
        switch (lr.code) {
            case VM_SYS_ERR_FULL:             e = VM_EAGAIN;  break;
            case VM_SYS_ERR_NO_ARENA_SPACE:   e = VM_ENOMEM;  break;
            case VM_SYS_ERR_INVALID_ARG:      e = VM_EINVAL;  break;
            default:                          e = VM_EIO;     break;
        }
        cpu->regs[VM_REG_A0] = (uint32_t)(-e);
        return;
    }

    /* Round V: spawn is now ASYNCHRONOUS. We register the child as
     * a normal scheduler VM and BLOCK THE PARENT on it, then return
     * to the scheduler. The host's main vm_system_step loop runs the
     * child alongside every other session — so spawning a long-lived
     * TUI program in one session no longer freezes the others.
     *
     * Lifecycle:
     *   - here: load child, inherit transport, park parent
     *           (BLOCK_ON_CHILD, block_child_vm = child id)
     *   - main loop: scheduler runs child + all siblings
     *   - child halts: vm_system_reap_halted_children() delivers the
     *     child's exit code to the parent's a0, wakes the parent,
     *     clears the child's transport binding, and unloads the child
     *
     * (The old design pumped the child in a nested loop right here,
     * which commandeered the host run loop and starved all other
     * VMs for the child's entire lifetime — the multi-session freeze.)
     */
    VmCpu *child = sys->vms[lr.assigned_vm_id];
    if (!child) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;
        return;
    }

    /* The child inherits the parent's transport binding so its TUI
     * canvas output reaches the same client. Cleared when the child
     * is reaped (see vm_system_reap_halted_children). */
    VmHostTransport *parent_t = vm_host_get_transport_for_vm(cpu->vm_id);
    if (parent_t) {
        vm_host_set_transport_for_vm((uint16_t)lr.assigned_vm_id, parent_t);
    }

#if GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE
    /* Preemptive: the child is its own scheduler task (registered at
     * runtime by vm_system_load_vm). Wait for it HERE, in the parent's
     * own task thread. Set block_child_vm first, then the marker, so the
     * child's halt-reap sees a consistent pair. Check the child's halted
     * flag before each park — a child that exits before we park is still
     * reaped (no lost wake); sticky presched_block covers the park/wake
     * race. The parent reads the exit code from the child, then unloads
     * it (the child touches nothing after waking us). */
    cpu->block_child_vm = (uint16_t)lr.assigned_vm_id;
    cpu->block_reason   = BLOCK_ON_CHILD;
    {
        VmPreCtx *pc = (VmPreCtx *)sys->ops.ctx;
        for (;;) {
            VmCpu *kid = sys->vms[lr.assigned_vm_id];
            if (!kid || kid->halted) {
                int32_t code;
                if (!kid) {
                    code = -(int32_t)VM_EIO;
                } else {
                    bool crashed = (kid->trap_cause >= TRAP_ILLEGAL_INSTR &&
                                    kid->trap_cause <= TRAP_INSTR_MISALIGNED);
                    code = crashed ? -(int32_t)VM_EIO
                                   : (int32_t)(kid->regs[VM_REG_A0] & 0xff);
                }
                cpu->regs[VM_REG_A0] = (uint32_t)code;
                break;
            }
            presched_block(pc->sched);
        }
    }
    cpu->block_reason   = BLOCK_NONE;
    cpu->block_child_vm = UINT16_MAX;
    vm_system_unload_vm(sys, (uint16_t)lr.assigned_vm_id);
#else
    /* Cooperative: park the parent on the child. The scheduler moves the
     * parent out of the ready set; the reap path (vm_system_step) wakes
     * it with the exit code in a0. We do NOT set a0 here. */
    cpu->block_reason   = BLOCK_ON_CHILD;
    cpu->block_child_vm = (uint16_t)lr.assigned_vm_id;
#endif
}


/* ============================================================
 *  TTY control handler
 *
 *  Thin wrapper around vm_host_stdio_set_raw_mode. Lives here
 *  (not in vm_host_stdio.c) because it needs to be registered
 *  separately from the unconditional stdio syscalls — only
 *  guests that opt into the fs/spawn handler chain get TTY
 *  control.
 * ============================================================ */

extern bool vm_host_stdio_set_raw_mode(bool enable);

/* SYS_TTY_SET_RAW
 *   a0 = enable (0 = cooked, nonzero = raw)
 *   → a0 = 0 on success, -ENOTTY if stdin isn't a tty
 *
 * Routing matters here. A guest in a multi-session deployment (e.g.
 * each TCP client gets its own shell VM) must only affect ITS OWN
 * transport. If we naively called vm_host_stdio_set_raw_mode(), that
 * helper consults the DEFAULT transport — which is NULL in TCP mode —
 * and then falls through to raw-mode the HOST's own console (fd 0).
 * That had a nasty side effect: the moment a connected shell asked
 * for raw mode, the host's PowerShell/cmd console lost
 * ENABLE_PROCESSED_INPUT, so Ctrl-C stopped generating a console
 * CTRL_C_EVENT (it became a literal 0x03 byte) and the host could no
 * longer be interrupted — but only AFTER a client connected. We
 * therefore route through the calling VM's bound transport first;
 * for a TCP session that's the socket transport, whose set_raw is a
 * harmless no-op (a socket has no line discipline). Only a genuine
 * single-session stdio guest (no per-VM transport, no default) falls
 * through to the real console, which is correct for that case. */
static void handle_tty_set_raw(VmCpu *cpu, void *system) {
    (void)system;
    bool enable = (cpu->regs[VM_REG_A0] != 0);

    VmHostTransport *vt = vm_host_get_transport_for_vm(cpu->vm_id);
    if (vt) {
        /* This VM owns a transport. If it can set raw, use it; if it
         * has no set_raw (or it's a no-op), that's success too — the
         * guest's request is satisfied for that medium. We must NOT
         * fall through to the host console for a transport-bound VM. */
        bool ok = vt->set_raw ? (vt->set_raw(vt, enable) >= 0) : true;
        cpu->regs[VM_REG_A0] = ok ? 0 : (uint32_t)-((int32_t)VM_EIO);
        return;
    }

    if (vm_host_stdio_set_raw_mode(enable)) {
        cpu->regs[VM_REG_A0] = 0;
    } else {
        /* ENOTTY (25 on Linux) — we don't have a VM_ENOTTY define
         * but the contract documents "not a tty". Use EIO as the
         * closest existing value. */
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EIO);
    }
}

/* ============================================================
 *  Wire-up with vm_host_stdio
 *
 *  vm_host_stdio's read/write/close handlers delegate fd >= 3
 *  to us. We don't depend on stdio's internals — we just call
 *  the setter it exposes for installing function-pointer hooks.
 *
 *  Declared here as extern. vm_host_stdio.c provides the
 *  definition. If you link vm_host_fs.c WITHOUT vm_host_stdio.c
 *  the link will fail — that's fine because file syscalls
 *  without stdio doesn't make much sense (no way to print
 *  errors, no fds 0/1/2 wired up).
 * ============================================================ */

extern void vm_host_stdio_set_fs_hooks(vm_host_fs_read_hook_t,
                                       vm_host_fs_write_hook_t,
                                       vm_host_fs_close_hook_t);

/* ============================================================
 *  Install / reset
 * ============================================================ */

static void vm_host_fs_close_all(void) {
    for (unsigned i = 0; i < VM_HOST_FS_MAX_FILES; i++) {
        if (g_fds[i].kind == SLOT_FILE) f_close(&g_fds[i].u.file);
        else if (g_fds[i].kind == SLOT_DIR) f_closedir(&g_fds[i].u.dir);
        else if (g_fds[i].kind == SLOT_HOST_FILE && g_fds[i].u.host)
            fclose(g_fds[i].u.host);
        g_fds[i].kind = SLOT_FREE;
    }
}

void vm_host_fs_reset(void) {
    vm_host_fs_close_all();
    vm_host_fs_unmount_all();
    vm_host_stdio_set_fs_hooks(NULL, NULL, NULL);
}

/* Locking trampolines: serialize each FS entry point under the
 * preemptive backend (no-op under cooperative). The handlers and the
 * fs_*_fd hooks are left untouched — wrapping them here keeps their
 * many early-returns simple and the lock scope obviously correct.
 * SYS_SPAWN_AND_WAIT and SYS_TTY_SET_RAW are deliberately NOT wrapped:
 * spawn locks only its load (it parks waiting for the child, which must
 * be able to take the FS lock), and tty_set_raw touches no FS state. */
#define FS_LK(name) static void lk_##name(VmCpu *c, void *s) { \
    fs_lock(); name(c, s); fs_unlock(); }
FS_LK(handle_openat)
FS_LK(handle_close)
FS_LK(handle_lseek)
FS_LK(handle_mkdirat)
FS_LK(handle_unlinkat)
FS_LK(handle_readdir)
#undef FS_LK

static int32_t lk_fs_read_fd(int fd, void *buf, uint32_t n) {
    fs_lock(); int32_t r = fs_read_fd(fd, buf, n); fs_unlock(); return r;
}
static int32_t lk_fs_write_fd(int fd, const void *buf, uint32_t n) {
    fs_lock(); int32_t r = fs_write_fd(fd, buf, n); fs_unlock(); return r;
}
static int32_t lk_fs_close_fd(int fd) {
    fs_lock(); int32_t r = fs_close_fd(fd); fs_unlock(); return r;
}

bool vm_host_install_fs(VmSystem *sys) {
    if (!sys || !sys->ecall_router) return false;

    /* Register all the fs syscalls (via locking trampolines). If any
     * fails, roll back. */
    if (!vm_ecall_register(sys->ecall_router, SYS_OPENAT,   lk_handle_openat))   goto fail;
    if (!vm_ecall_register(sys->ecall_router, SYS_CLOSE,    lk_handle_close))    goto fail_openat;
    if (!vm_ecall_register(sys->ecall_router, SYS_LSEEK,    lk_handle_lseek))    goto fail_close;
    if (!vm_ecall_register(sys->ecall_router, SYS_MKDIRAT,  lk_handle_mkdirat))  goto fail_lseek;
    if (!vm_ecall_register(sys->ecall_router, SYS_UNLINKAT, lk_handle_unlinkat)) goto fail_mkdirat;
    if (!vm_ecall_register(sys->ecall_router, SYS_READDIR,  lk_handle_readdir))  goto fail_unlinkat;
    if (!vm_ecall_register(sys->ecall_router, SYS_SPAWN_AND_WAIT,
                                                            handle_spawn_and_wait)) goto fail_readdir;
    if (!vm_ecall_register(sys->ecall_router, SYS_TTY_SET_RAW,
                                                            handle_tty_set_raw))   goto fail_spawn;

    /* Wire up stdio's hooks so fd >= 3 routes here (also locked). */
    vm_host_stdio_set_fs_hooks(lk_fs_read_fd, lk_fs_write_fd, lk_fs_close_fd);

    return true;

fail_spawn:    vm_ecall_unregister(sys->ecall_router, SYS_SPAWN_AND_WAIT);
fail_readdir:  vm_ecall_unregister(sys->ecall_router, SYS_READDIR);
fail_unlinkat: vm_ecall_unregister(sys->ecall_router, SYS_UNLINKAT);
fail_mkdirat:  vm_ecall_unregister(sys->ecall_router, SYS_MKDIRAT);
fail_lseek:    vm_ecall_unregister(sys->ecall_router, SYS_LSEEK);
fail_close:    vm_ecall_unregister(sys->ecall_router, SYS_CLOSE);
fail_openat:   vm_ecall_unregister(sys->ecall_router, SYS_OPENAT);
fail:
    return false;
}

bool vm_host_install_fs_atexit(VmSystem *sys) {
    if (!vm_host_install_fs(sys)) return false;
    atexit(vm_host_fs_close_all);
    return true;
}

unsigned vm_host_fs_open_count(void) {
    unsigned n = 0;
    for (unsigned i = 0; i < VM_HOST_FS_MAX_FILES; i++) {
        if (g_fds[i].kind != SLOT_FREE) n++;
    }
    return n;
}

unsigned vm_host_fs_max_files(void) {
    return VM_HOST_FS_MAX_FILES;
}

/* ============================================================
 *  File-fd routing for alternate transports
 *
 *  These let hosts that install their OWN SYS_READ/SYS_WRITE/
 *  SYS_CLOSE handlers — instead of using vm_host_stdio's
 *  handle_read/handle_write/handle_close — delegate file-fd
 *  operations to us. The 05_shell example's pipe transport
 *  uses these.
 *
 *  The behavior matches the static fs_*_fd helpers above, which
 *  is also what gets called via vm_host_stdio's hook plumbing
 *  when the stdio bridge is in use. The public name avoids
 *  any confusion about being a "hook"-flavored back door.
 *
 *  Note: the static helpers return a sentinel VM_FS_NOT_OURS
 *  for fd < FD_BASE; the public function uses the same value
 *  (re-exported via the header as VM_HOST_FS_NOT_OURS) so
 *  callers don't need an internal include.
 * ============================================================ */

int32_t vm_host_fs_route_read(int fd, void *buf, uint32_t n) {
    fs_lock(); int32_t r = fs_read_fd(fd, buf, n); fs_unlock(); return r;
}

int32_t vm_host_fs_route_write(int fd, const void *buf, uint32_t n) {
    fs_lock(); int32_t r = fs_write_fd(fd, buf, n); fs_unlock(); return r;
}

int32_t vm_host_fs_route_close(int fd) {
    fs_lock(); int32_t r = fs_close_fd(fd); fs_unlock(); return r;
}
