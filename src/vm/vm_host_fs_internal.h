/* ============================================================
 *  vm_host_fs_internal.h — types and helpers shared between
 *  vm_host_fs.c and its split-off siblings (vm_host_fs_spawn.c).
 *
 *  NOT a public header: nothing under include/ should include this.
 *  Lives next to the .c files that include it. The types and
 *  functions exposed here are deliberately the minimum needed for
 *  the split — the rest stays static in vm_host_fs.c.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VM_HOST_FS_INTERNAL_H
#define VM_HOST_FS_INTERNAL_H

#include "vm/vm_host_fs.h"
#include "vm/vm_core.h"
#include "storage/trashfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Largest guest path the host accepts (including the leading mount
 * segment). Defined here so all split-off pieces use one cap. */
#define VM_HOST_FS_MAX_PATH 256

/* Mount kind. FREE means the slot is unused (memset state). */
typedef enum {
    MOUNT_KIND_FREE    = 0,
    MOUNT_KIND_HOST    = 1,
    MOUNT_KIND_TRASHFS = 3,
} MountKind;

#define MOUNT_NAME_MAX 15    /* names fit in [A-Za-z0-9_-]{1,15} */

typedef struct {
    MountKind kind;
    char      name[MOUNT_NAME_MAX + 1];

    /* HOST fields */
    char    *host_root;       /* malloc'd, no trailing slash */
    size_t   host_root_len;
    bool     writable;        /* only meaningful for HOST */

    /* TRASHFS fields */
    TrashfsVolume *trashfs_vol;
} Mount;

/* Which backend a path was routed to. */
typedef enum {
    PATH_BACKEND_HOST    = 1,
    PATH_BACKEND_TRASHFS = 2,
} PathBackend;

/* FS-wide lock helpers. Coarse mutex under the preemptive scheduler,
 * no-ops under the cooperative one. Inlined here so callers in either
 * file pay no call cost in the cooperative case. */
#if GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE
#  include <pthread.h>
extern pthread_mutex_t vm_host_fs_g_fs_mtx;
static inline void fs_lock(void)   { pthread_mutex_lock(&vm_host_fs_g_fs_mtx); }
static inline void fs_unlock(void) { pthread_mutex_unlock(&vm_host_fs_g_fs_mtx); }
#else
static inline void fs_lock(void)   { }
static inline void fs_unlock(void) { }
#endif

/* Resolve a guest-supplied path string into the host-side path the
 * backend wants. Lives in vm_host_fs.c. Inputs:
 *   cpu, guest_addr  → guest pointer to a NUL-terminated string
 *   out, cap         → caller's buffer to receive the host path
 *   out_backend      → which backend the path routed to (HOST/TRASHFS)
 *   out_writable     → backend's writable flag (HOST only meaningful)
 *   out_mount        → the resolved mount entry (caller-owned pointer
 *                      receives an interior pointer; valid while the
 *                      caller holds the fs lock)
 * Returns 0 on success, -VM_E* on failure. */
int resolve_guest_path(VmCpu *cpu, uint32_t guest_addr,
                       char *out, size_t cap,
                       PathBackend *out_backend,
                       bool *out_writable,
                       const Mount **out_mount);

/* Slurp an entire file into a freshly malloc'd buffer. Caller frees.
 * Lives in vm_host_fs_spawn.c (its only user). Inputs:
 *   path             → already-resolved host-side path string
 *   backend, mnt     → from resolve_guest_path
 *   out_size         → byte count of the returned buffer
 *   out_err          → set to a POSITIVE VM_E* on failure (so callers
 *                      negate as needed)
 * Returns the buffer on success, NULL on failure. */
uint8_t *slurp_file(const char *path, PathBackend backend,
                    const Mount *mnt,
                    size_t *out_size, int32_t *out_err);

#endif /* VM_HOST_FS_INTERNAL_H */
