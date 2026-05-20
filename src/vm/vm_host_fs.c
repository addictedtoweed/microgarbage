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
#include "vm/vm_loader.h"
#include "vm/vm_system.h"

#include "ff.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <sched.h>

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

typedef enum {
    SLOT_FREE = 0,
    SLOT_FILE,        /* FatFs file (u.file) */
    SLOT_DIR,         /* FatFs directory (u.dir) */
    SLOT_HOST_FILE,   /* Host-filesystem file (u.host) */
} SlotKind;

typedef struct {
    SlotKind kind;
    union {
        FIL   file;
        DIR   dir;
        FILE *host;     /* stdio FILE* for SLOT_HOST_FILE */
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
 *  Guests use POSIX-ish paths starting with "/". FatFs uses
 *  volume-prefixed paths like "0:/foo". We rewrite leading "/"
 *  to "0:/" so the two conventions interoperate.
 *
 *  If the guest path already starts with "<digit>:" or has no
 *  leading "/", we pass it through unchanged — letting advanced
 *  users address specific volumes if FF_VOLUMES > 1.
 * ============================================================ */

#define MAX_PATH 256

/* Where /host/... maps to on the real host filesystem. NULL
 * means the mount is disabled — paths starting with /host/
 * return -ENOENT. */
static char    *g_host_fs_root      = NULL;
static size_t   g_host_fs_root_len  = 0;
static bool     g_host_fs_writable  = false;

/* Which backend a path is destined for after translation. */
typedef enum {
    PATH_BACKEND_FATFS = 0,   /* hand to f_open/f_mkdir/etc.        */
    PATH_BACKEND_HOST  = 1,   /* hand to native fopen/mkdir/etc.   */
} PathBackend;

/* Resolve a guest path into a host-usable absolute path.
 *
 * Three classes:
 *
 *   "/host/..."     -> PATH_BACKEND_HOST  ; resolved against g_host_fs_root
 *                                           with .. escapes rejected
 *   "<digit>:/..."  -> PATH_BACKEND_FATFS ; passes through unchanged
 *   "/..."          -> PATH_BACKEND_FATFS ; prefixed with "0:"
 *   else            -> PATH_BACKEND_FATFS ; pass-through (FatFs will reject
 *                                           if FF_FS_RPATH is off)
 *
 * Returns 0 on success and sets *out_backend, or -errno on
 * failure. The translated path is written into `out` (cap bytes).
 */
static int resolve_guest_path(VmCpu *cpu, uint32_t guest_addr,
                              char *out, size_t cap,
                              PathBackend *out_backend) {
    if (cap < 16) return -VM_EINVAL;

    /* Stage 1: copy the raw guest path into a scratch buffer.
     * We do this without prefix-injection so we can inspect it
     * cleanly. */
    char raw[MAX_PATH];
    size_t raw_pos = 0;
    for (;;) {
        if (raw_pos >= sizeof(raw) - 1) return -VM_ENAMETOOLONG;
        const char *b = vm_translate_read(cpu, guest_addr + (uint32_t)raw_pos, 1);
        if (!b) return -VM_EFAULT;
        raw[raw_pos] = *b;
        if (*b == '\0') break;
        raw_pos++;
    }

    /* Stage 2: dispatch by prefix. */

    /* (a) /host/... — host filesystem passthrough */
    if (strncmp(raw, "/host/", 6) == 0 || strcmp(raw, "/host") == 0) {
        if (!g_host_fs_root) return -VM_ENOENT;

        /* Path under the mount: skip the "/host" prefix. The
         * remainder (which starts with "/" or is empty) gets
         * appended to g_host_fs_root. */
        const char *rel = raw + 5;   /* skip "/host" */
        if (*rel == '\0') rel = "/"; /* "/host" alone means the root dir */

        /* Reject .. components anywhere in the remainder. Even
         * a single "/.." attempt suggests an escape attempt; we
         * don't try to normalize and verify in-bounds. */
        if (strstr(rel, "/..") != NULL ||
            (rel[0] == '.' && rel[1] == '.' && (rel[2] == '\0' || rel[2] == '/'))) {
            return -VM_EPERM;
        }

        /* Compose: g_host_fs_root + rel. Both have a leading/trailing
         * "/" arrangement we need to handle carefully. Root is
         * guaranteed to NOT end in "/" (we strip it on configure);
         * rel is guaranteed to START with "/". */
        size_t rel_len = strlen(rel);
        if (g_host_fs_root_len + rel_len + 1 > cap) return -VM_ENAMETOOLONG;
        memcpy(out, g_host_fs_root, g_host_fs_root_len);
        memcpy(out + g_host_fs_root_len, rel, rel_len + 1);   /* +1 for null */

        *out_backend = PATH_BACKEND_HOST;
        return 0;
    }

    /* (b) FatFs: translate "/foo" -> "0:/foo", pass through
     * "N:/foo" forms unchanged. */
    bool has_volume_prefix = false;
    if (raw[0] >= '0' && raw[0] <= '9' && raw[1] == ':') {
        has_volume_prefix = true;
    }

    size_t out_pos = 0;
    if (!has_volume_prefix && raw[0] == '/') {
        if (cap < raw_pos + 3) return -VM_ENAMETOOLONG;
        out[out_pos++] = '0';
        out[out_pos++] = ':';
    }
    if (out_pos + raw_pos + 1 > cap) return -VM_ENAMETOOLONG;
    memcpy(out + out_pos, raw, raw_pos + 1);   /* includes null */

    *out_backend = PATH_BACKEND_FATFS;
    return 0;
}

/* Copy a guest path (zero-terminated string at guest_addr) into
 * `out` (size `cap`), rewriting "/..." -> "0:/...". Returns the
 * length of the copied path on success (not including the null
 * terminator), or -errno on failure.
 *
 * This is the FatFs-only variant — paths starting with "/host/"
 * are translated to FatFs form anyway, which will fail since no
 * "host" volume is mounted. Used by handlers that DON'T need to
 * support the host mount (e.g., mkdir/unlink — we can't create
 * directories on the real host fs, so why translate the path).
 *
 * NEW handlers should use resolve_guest_path() instead.
 */
static int copy_path(VmCpu *cpu, uint32_t guest_addr,
                     char *out, size_t cap) {
    PathBackend backend;
    int r = resolve_guest_path(cpu, guest_addr, out, cap, &backend);
    if (r < 0) return r;
    if (backend == PATH_BACKEND_HOST) {
        /* Caller doesn't support host paths. Tell them no. */
        return -VM_EROFS;
    }
    return (int)strlen(out);
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

/* Returns bytes read, -errno on error, or VM_FS_NOT_OURS if fd
 * is not a file fd (caller should fall back to other handling). */
#define VM_FS_NOT_OURS  (-12345678)

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
        if (!g_host_fs_writable) return -VM_EROFS;
        size_t bw = fwrite(buf, 1, n, s->u.host);
        if (bw < n) return -VM_EIO;
        return (int32_t)bw;
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

    char buf[MAX_PATH];
    PathBackend backend;
    int rp = resolve_guest_path(cpu, path, buf, sizeof(buf), &backend);
    if (rp < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)rp;
        return;
    }

    /* ---- Host-filesystem path ---- */
    if (backend == PATH_BACKEND_HOST) {
        if (flags & VM_O_DIRECTORY) {
            /* Directory enumeration on the host fs isn't implemented
             * in this round — would need opendir/readdir/closedir
             * wrappers and a new SLOT_HOST_DIR. Easy to add later
             * if needed; for now /host is files-only. */
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_ENOSYS;
            return;
        }
        /* Reject write-class opens against a read-only mount. */
        uint32_t access = flags & VM_O_ACCMODE;
        bool wants_write = (access != VM_O_RDONLY) ||
                           (flags & (VM_O_CREAT | VM_O_TRUNC | VM_O_APPEND));
        if (wants_write && !g_host_fs_writable) {
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

    char buf[MAX_PATH];
    int p = copy_path(cpu, path, buf, sizeof(buf));
    if (p < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)p;
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
    int32_t dirfd = (int32_t)cpu->regs[VM_REG_A0];
    uint32_t path = cpu->regs[VM_REG_A1];

    if (dirfd != VM_AT_FDCWD) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
        return;
    }

    char buf[MAX_PATH];
    int p = copy_path(cpu, path, buf, sizeof(buf));
    if (p < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)p;
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

    FdSlot *s = slot_for(fd, SLOT_DIR);
    if (!s) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF;
        return;
    }

    VmDirent *out = vm_translate_write(cpu, dirent_p, sizeof(VmDirent));
    if (!out) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
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
 *  Host-filesystem mount configuration
 * ============================================================ */

bool vm_host_set_host_fs_root(const char *root, bool writable) {
    /* Tear down any previous configuration first. */
    if (g_host_fs_root) {
        free(g_host_fs_root);
        g_host_fs_root = NULL;
        g_host_fs_root_len = 0;
        g_host_fs_writable = false;
    }

    if (!root) return true;   /* NULL means "disable mount" */

    /* Validate that the directory actually exists. We use stat()
     * to check — this is portable across POSIX and works on Cygwin. */
    struct stat st;
    if (stat(root, &st) != 0) return false;
    if (!S_ISDIR(st.st_mode)) return false;

    /* Copy the path, stripping any trailing slash. We always
     * compose with rel paths that start with "/", so a trailing
     * slash on root would cause "//"  in the result. */
    size_t len = strlen(root);
    while (len > 1 && (root[len - 1] == '/' || root[len - 1] == '\\')) len--;

    char *copy = malloc(len + 1);
    if (!copy) return false;
    memcpy(copy, root, len);
    copy[len] = '\0';

    g_host_fs_root = copy;
    g_host_fs_root_len = len;
    g_host_fs_writable = writable;
    return true;
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
 * Loads the file at `path` as a new VM in the same VmSystem and
 * pumps the dispatcher until that VM halts. The parent VM (the
 * one that called this) is paused for the duration — the spawn
 * handler doesn't return until the child halts.
 *
 * Other VMs in the system are NOT scheduled during the spawn.
 * This keeps the model simple: spawn is fully synchronous.
 * Background spawns / multi-VM concurrency are future work.
 */
static void handle_spawn_and_wait(VmCpu *cpu, void *system) {
    VmSystem *sys = (VmSystem *)system;
    uint32_t path_addr = cpu->regs[VM_REG_A0];

    char buf[MAX_PATH];
    PathBackend backend;
    int rp = resolve_guest_path(cpu, path_addr, buf, sizeof(buf), &backend);
    if (rp < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)rp;
        return;
    }

    /* Slurp the whole ELF into RAM. */
    size_t elf_size = 0;
    int32_t err = 0;
    uint8_t *elf = slurp_file(buf, backend, &elf_size, &err);
    if (!elf) {
        cpu->regs[VM_REG_A0] = (uint32_t)(-err);
        return;
    }

    /* Load as a new VM. VM_BACKING_COPY_RAM means the loader
     * copies the bytes it needs out of our buffer, so we can
     * free the buffer after vm_system_load_vm returns. */
    VmLoadVmResult lr = vm_system_load_vm(sys, elf, elf_size,
                                          g_spawn_data_size,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    free(elf);

    if (lr.code != VM_SYS_OK) {
        /* Map system-load errors to errno. */
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

    /* Pump the new VM until it halts. We step ONLY the spawned
     * VM — other VMs in the scheduler don't tick during this
     * time, which keeps the model simple.
     *
     * We have to do a few things the main scheduler normally
     * does on our behalf, because we're not going through
     * vm_sched_step here:
     *
     *   - Refresh sys->sched->global_tick from the host tick
     *     source each iteration, so the child's SYS_TICKS_NOW
     *     sees forward progress and SYS_SLEEP_* deadlines can
     *     actually be reached.
     *   - Honor BLOCK_SLEEP / BLOCK_YIELDED: a child that calls
     *     sys_sleep_until needs us to wait until its deadline
     *     before stepping it again, otherwise the loop becomes
     *     a busy-wait that never makes timing progress.
     */
    VmCpu *child = sys->vms[lr.assigned_vm_id];
    if (!child) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;
        return;
    }

    VmSched *sched = sys->sched;

    while (!child->halted) {
        /* Refresh global_tick from the host's tick source. */
        if (sched->config.tick_source) {
            sched->global_tick =
                sched->config.tick_source(sched->config.tick_source_userdata);
        }

        /* If the child is blocked on a sleep, decide whether
         * its deadline has been reached. We use the same
         * wraparound-safe comparison the scheduler does. */
        if (child->block_reason == BLOCK_SLEEP) {
            uint32_t now = sched->global_tick;
            uint32_t deadline = child->block_deadline;
            int32_t delta = (int32_t)(deadline - now);

            if (delta <= 0) {
                /* Deadline reached. Wake. */
                child->block_reason = BLOCK_NONE;
                child->block_deadline = 0;
                child->regs[VM_REG_A0] = 0;
            } else {
                /* Still asleep. Hand the host CPU back so we
                 * don't pin a core.
                 *
                 * For waits over 20 ticks, nanosleep most of
                 * the way and leave a 20-tick tail to absorb
                 * the host OS timer's coarse granularity
                 * (15 ms on Windows). Inside the tail, nanosleep
                 * 1 ms at a time and re-check. */
                struct timespec ts;
                int32_t sleep_ticks = (delta > 20) ? (delta - 20) : 1;
                ts.tv_sec  = sleep_ticks / 1000;
                ts.tv_nsec = (long)(sleep_ticks % 1000) * 1000000L;
                nanosleep(&ts, NULL);
                continue;
            }
        } else if (child->block_reason == BLOCK_YIELDED) {
            /* YIELD is "wake on next pass" — just clear it
             * and step. */
            child->block_reason = BLOCK_NONE;
            child->regs[VM_REG_A0] = 0;
        }

        VmStepResult r = vm_step(child, 4096, NULL);
        if (r == VM_STEP_ECALL) {
            vm_ecall_dispatch(sched->config.ecall_router, child, sys);
        } else if (r == VM_STEP_TRAPPED) {
            /* The child crashed (illegal instruction, bad memory
             * access, etc.). Before returning to the parent, do
             * cleanup the child can't do for itself:
             *
             *   1. Restore the terminal to cooked mode. The child
             *      may have called SYS_TTY_SET_RAW(1) and trapped
             *      before getting to its SYS_TTY_SET_RAW(0) on
             *      exit; without this restore, the parent shell
             *      inherits a broken terminal.
             *   2. Print a brief diagnostic to stderr so the user
             *      knows something went wrong (otherwise the only
             *      sign is a nonzero exit code).
             *
             * We don't try to clear the screen or restore cursor
             * — the host doesn't know what the child was doing on
             * screen and over-cleaning could hide useful info. */
            vm_host_stdio_set_raw_mode(false);
            fprintf(stderr, "\r\nspawn: child trapped (cause=%u, trap_pc=0x%08x)\r\n",
                    (unsigned)child->trap_cause, child->trap_pc);
            child->halted = true;
            cpu->regs[VM_REG_A0] = (uint32_t)-VM_EIO;
            return;
        }
        /* VM_STEP_HALTED and VM_STEP_QUANTUM_EXPIRED loop back. */
    }

    /* Child halted normally. Its exit code is in regs[a0]. We
     * mask to 8 bits to match Unix exit-status conventions and
     * to keep our return positive (negative values would look
     * like errors). */
    uint32_t exit_code = child->regs[VM_REG_A0] & 0xff;
    cpu->regs[VM_REG_A0] = exit_code;
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
 */
static void handle_tty_set_raw(VmCpu *cpu, void *system) {
    (void)system;
    bool enable = (cpu->regs[VM_REG_A0] != 0);
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
    if (g_host_fs_root) {
        free(g_host_fs_root);
        g_host_fs_root = NULL;
        g_host_fs_root_len = 0;
        g_host_fs_writable = false;
    }
    vm_host_stdio_set_fs_hooks(NULL, NULL, NULL);
}

bool vm_host_install_fs(VmSystem *sys) {
    if (!sys || !sys->ecall_router) return false;

    /* Register all the fs syscalls. If any fails, roll back. */
    if (!vm_ecall_register(sys->ecall_router, SYS_OPENAT,   handle_openat))   goto fail;
    if (!vm_ecall_register(sys->ecall_router, SYS_CLOSE,    handle_close))    goto fail_openat;
    if (!vm_ecall_register(sys->ecall_router, SYS_LSEEK,    handle_lseek))    goto fail_close;
    if (!vm_ecall_register(sys->ecall_router, SYS_MKDIRAT,  handle_mkdirat))  goto fail_lseek;
    if (!vm_ecall_register(sys->ecall_router, SYS_UNLINKAT, handle_unlinkat)) goto fail_mkdirat;
    if (!vm_ecall_register(sys->ecall_router, SYS_READDIR,  handle_readdir))  goto fail_unlinkat;
    if (!vm_ecall_register(sys->ecall_router, SYS_SPAWN_AND_WAIT,
                                                            handle_spawn_and_wait)) goto fail_readdir;
    if (!vm_ecall_register(sys->ecall_router, SYS_TTY_SET_RAW,
                                                            handle_tty_set_raw))   goto fail_spawn;

    /* Wire up stdio's hooks so fd >= 3 routes here. */
    vm_host_stdio_set_fs_hooks(fs_read_fd, fs_write_fd, fs_close_fd);

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
