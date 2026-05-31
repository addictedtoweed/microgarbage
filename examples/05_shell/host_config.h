/* ============================================================
 *  host_config.h — HostConfig + vm.cfg loader for the shell host.
 *
 *  Three concerns the host has to make legible to the user:
 *
 *    1. How big the backing slabs are (SHARED_BYTES / LOCAL_BYTES).
 *       These are compile-time caps the .c file uses to size its
 *       static storage and the config layer uses as upper bounds
 *       when validating user values.
 *    2. What HostConfig holds — memory sizes, stdio mode, mount
 *       table. The struct is the single value main() pipes through
 *       VM bring-up, stdio install, mount install, and cleanup.
 *    3. How vm.cfg is loaded. Layering is:
 *         defaults -> vm.cfg -> CLI overrides
 *       so the CLI wins ties and the config is just persistent
 *       defaults. The CLI half is still in main(); this header
 *       covers stages 1 and 2.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HOST_CONFIG_H
#define HOST_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ----------------------------------------------------------------
 *  Backing-storage caps
 *
 *  Shared slab: cross-VM RPC/mailbox storage. Tiny per VM, so 64 KB
 *  is roomy. Local slab: per-VM segments — text, data, stack, mailboxes.
 *  Slabs carve into fixed-size bins; each VM allocation rounds up to
 *  the next bin size. With our config (4 VMs x 64 KB data), each VM
 *  consumes roughly:
 *
 *     VmCpu (280 B)    -> 512 B bin
 *     mailbox (272 B)  -> 512 B bin
 *     text (<= 30 KB)  -> 32 KB bin
 *     rodata (<= 4 KB) -> 8 KB bin
 *     data (64 KB)     -> 128 KB bin  (+ slab's 8 B header per block
 *                                       means a 64 KB request needs
 *                                       65544 B of block storage)
 *
 *  Plus the slab populates smaller bins (32 B..2 KB) for variable
 *  segment sizes — vm_system_local_required reports ~1.3 MB total.
 *  We round to 1.5 MB to leave margin.
 * ---------------------------------------------------------------- */
#define SHARED_BYTES (64 * 1024)
#define LOCAL_BYTES  (1536 * 1024)

/* ----------------------------------------------------------------
 *  Mount table
 *
 *  Entry kind dispatches in the mount installer. `tmpfs` and `sd`
 *  both map to a trashfs volume on the dev host today — they diverge
 *  only when the platform moves to hardware (tmpfs over RAM vs. a
 *  persistent volume on an SD-card driver). On the dev host the
 *  distinction is intent.
 * ---------------------------------------------------------------- */
typedef enum {
    HOST_MOUNT_TMPFS = 0,
    HOST_MOUNT_SD    = 1,
    HOST_MOUNT_HOST  = 2,
} HostMountKind;

#define HOST_MOUNT_MAX 8

typedef struct {
    HostMountKind kind;
    char          name[16];
    /* TMPFS/SD: size in KB.
     * HOST:     path string. */
    uint32_t      size_kb;
    char          path[128];
    bool          writable;
} HostMount;

/* ----------------------------------------------------------------
 *  HostConfig
 * ---------------------------------------------------------------- */
typedef struct {
    /* Memory */
    size_t   local_bytes;        /* local slab size */
    size_t   shared_bytes;       /* shared slab size */
    uint16_t max_vms;
    uint16_t spawn_data_kb;

    /* Stdio */
    bool     raw_mode;           /* enable raw mode on stdin if tty */

    /* Mounts. If mount_count==0, the host uses built-in defaults
     * (td0 + host). Non-zero means the config explicitly listed
     * mounts; defaults are skipped entirely. */
    HostMount mounts[HOST_MOUNT_MAX];
    unsigned  mount_count;
} HostConfig;

/* Lifecycle */

/* Reset hc to the historical hardcoded defaults so a host launched
 * with no config or flags behaves identically to the M.1b version. */
void host_config_set_defaults(HostConfig *hc);

/* Attempt to load vm.cfg from `path`. If `path` is NULL, tries
 * "./vm.cfg" and silently does nothing if it's not there. If `path`
 * is non-NULL, missing-or-unreadable IS an error (the user explicitly
 * asked for that file). Returns true on success. */
bool host_config_load(const char *path, HostConfig *hc);

#endif /* HOST_CONFIG_H */
