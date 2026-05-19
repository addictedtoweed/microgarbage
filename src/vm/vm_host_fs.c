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

#include "ff.h"

#include <stdlib.h>
#include <string.h>

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

typedef enum { SLOT_FREE = 0, SLOT_FILE, SLOT_DIR } SlotKind;

typedef struct {
    SlotKind kind;
    union {
        FIL file;
        DIR dir;
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

/* Copy a guest path (zero-terminated string at guest_addr) into
 * `out` (size `cap`), rewriting "/..." -> "0:/...". Returns the
 * length of the copied path on success (not including the null
 * terminator), or -errno on failure.
 *
 * Recognized guest path forms:
 *   "/foo/bar"     ->  "0:/foo/bar"   (default volume; common case)
 *   "0:/foo/bar"   ->  "0:/foo/bar"   (already has volume prefix)
 *   "1:/foo/bar"   ->  "1:/foo/bar"   (other volume; pass through)
 *   "foo"          ->  "foo"          (no slash; pass through — FatFs
 *                                       will treat as relative path
 *                                       and will fail since RPATH=0)
 */
static int copy_path(VmCpu *cpu, uint32_t guest_addr,
                     char *out, size_t cap) {
    size_t out_pos = 0;
    uint32_t guest_pos = 0;
    if (cap < 8) return -VM_EINVAL;  /* sanity floor */

    /* Peek first 2 bytes to decide if a volume prefix is present. */
    const char *first = vm_translate_read(cpu, guest_addr, 1);
    if (!first) return -VM_EFAULT;

    bool has_volume_prefix = false;
    if (*first >= '0' && *first <= '9') {
        const char *second = vm_translate_read(cpu, guest_addr + 1, 1);
        if (second && *second == ':') has_volume_prefix = true;
    }

    /* Inject "0:" if guest path starts with "/" and has no
     * volume prefix of its own. */
    if (!has_volume_prefix && *first == '/') {
        out[out_pos++] = '0';
        out[out_pos++] = ':';
    }

    /* Copy guest bytes one at a time until null. */
    for (;;) {
        if (out_pos >= cap - 1) return -VM_ENAMETOOLONG;
        const char *b = vm_translate_read(cpu, guest_addr + guest_pos, 1);
        if (!b) return -VM_EFAULT;
        out[out_pos] = *b;
        if (*b == '\0') return (int)out_pos;
        out_pos++;
        guest_pos++;
    }
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
        case FR_INVALID_NAME:        return -VM_EINVAL;
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
 * ============================================================ */

/* Returns bytes read, -errno on error, or VM_FS_NOT_OURS if fd
 * is not a file fd (caller should fall back to other handling). */
#define VM_FS_NOT_OURS  (-12345678)

static int32_t fs_read_fd(int fd, void *buf, uint32_t n) {
    if (fd < FD_BASE) return VM_FS_NOT_OURS;
    FdSlot *s = slot_for(fd, SLOT_FILE);
    if (!s) return -VM_EBADF;
    UINT br = 0;
    FRESULT r = f_read(&s->u.file, buf, n, &br);
    if (r != FR_OK) return fres_to_errno(r);
    return (int32_t)br;
}

static int32_t fs_write_fd(int fd, const void *buf, uint32_t n) {
    if (fd < FD_BASE) return VM_FS_NOT_OURS;
    FdSlot *s = slot_for(fd, SLOT_FILE);
    if (!s) return -VM_EBADF;
    UINT bw = 0;
    FRESULT r = f_write(&s->u.file, buf, n, &bw);
    if (r != FR_OK) return fres_to_errno(r);
    return (int32_t)bw;
}

static int32_t fs_close_fd(int fd) {
    if (fd < FD_BASE) return VM_FS_NOT_OURS;
    if (fd >= FD_LIMIT) return -VM_EBADF;
    FdSlot *s = &g_fds[fd - FD_BASE];
    if (s->kind == SLOT_FREE) return -VM_EBADF;

    FRESULT r = FR_OK;
    if (s->kind == SLOT_FILE) r = f_close(&s->u.file);
    else if (s->kind == SLOT_DIR) r = f_closedir(&s->u.dir);
    free_slot(s);
    return fres_to_errno(r);
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
    int p = copy_path(cpu, path, buf, sizeof(buf));
    if (p < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)p;
        return;
    }

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

    FdSlot *s = slot_for(fd, SLOT_FILE);
    if (!s) {
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
        g_fds[i].kind = SLOT_FREE;
    }
}

void vm_host_fs_reset(void) {
    vm_host_fs_close_all();
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

    /* Wire up stdio's hooks so fd >= 3 routes here. */
    vm_host_stdio_set_fs_hooks(fs_read_fd, fs_write_fd, fs_close_fd);

    return true;

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
