/* ============================================================
 *  host_mounts.c — mount-table installer.
 *
 *  See host_mounts.h for the contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "host_mounts.h"

#include "host_util.h"
#include "vm/vm_host_fs.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

/* Install the default mount pair: /td0 (trashfs) + /host (passthrough).
 * Returns false only on /td0 failure (the /host mount degrades to
 * "skipped" with a logged warning). */
static bool install_defaults(TrashfsVolume *trash_vol,
                             const char    *host_fs_root,
                             bool           host_fs_writable,
                             bool           host_fs_disabled) {
    if (!vm_host_fs_mount_trashfs("td0", trash_vol)) {
        fprintf(stderr, "host: vm_host_fs_mount_trashfs('td0') failed\n");
        return false;
    }
    if (host_fs_disabled) return true;

    struct stat st;
    if (stat(host_fs_root, &st) != 0) {
        if (host_mkdir(host_fs_root, 0755) != 0) {
            fprintf(stderr, "host: warning - could not create '%s' for "
                    "/host mount: %s\n",
                    host_fs_root, strerror(errno));
            fprintf(stderr, "host: /host will be disabled\n");
            return true;
        }
    }
    if (!vm_host_fs_mount_host("host", host_fs_root, host_fs_writable)) {
        fprintf(stderr, "host: warning - vm_host_fs_mount_host('%s') failed\n",
                host_fs_root);
        fprintf(stderr, "host: /host will be disabled\n");
        return true;
    }
    fprintf(stderr, "host: /host mounted from '%s' (%s)\n",
            host_fs_root, host_fs_writable ? "read/write" : "read-only");
    return true;
}

/* Install one config-driven mount. Returns false on a hard failure
 * (trashfs install fails, or a HOST mount has no path). The "multiple
 * writable mounts" case warns and skips (returns true). */
static bool install_one(const HostMount *m,
                        TrashfsVolume   *trash_vol,
                        uint32_t         trash_region_kb,
                        bool            *any_td_mounted_inout) {
    if (m->kind == HOST_MOUNT_TMPFS || m->kind == HOST_MOUNT_SD) {
        /* tmpfs and sd both map to the single trashfs RAM disk today.
         * On hardware they'll diverge (tmpfs stays in RAM; sd uses
         * the SD card driver). For now: enforce one writable volume
         * until multi-volume support. */
        if (*any_td_mounted_inout) {
            fprintf(stderr, "host: vm.cfg: multiple writable mounts "
                    "(tmpfs/sd) not supported yet (ignoring mount.%s)\n",
                    m->name);
            return true;
        }
        if (!vm_host_fs_mount_trashfs(m->name, trash_vol)) {
            fprintf(stderr, "host: vm_host_fs_mount_trashfs('%s') failed\n",
                    m->name);
            return false;
        }
        const char *kind_str =
            (m->kind == HOST_MOUNT_TMPFS) ? "tmpfs" : "sd";
        fprintf(stderr, "host: /%s mounted (%s via trashfs, %u KB%s)\n",
                m->name, kind_str, (unsigned)trash_region_kb,
                m->size_kb ? "; size_kb override ignored" : "");
        *any_td_mounted_inout = true;
        return true;
    }

    /* HOST passthrough. Path is required. */
    if (m->path[0] == '\0') {
        fprintf(stderr, "host: vm.cfg: [mount.%s] type=host needs a "
                "'path' setting\n", m->name);
        return false;
    }
    struct stat st;
    if (stat(m->path, &st) != 0) {
        if (host_mkdir(m->path, 0755) != 0) {
            fprintf(stderr, "host: vm.cfg: [mount.%s] cannot create '%s': "
                    "%s\n", m->name, m->path, strerror(errno));
            return false;
        }
    }
    if (!vm_host_fs_mount_host(m->name, m->path, m->writable)) {
        fprintf(stderr, "host: vm.cfg: [mount.%s] "
                "vm_host_fs_mount_host('%s') failed\n",
                m->name, m->path);
        return false;
    }
    fprintf(stderr, "host: /%s mounted from '%s' (%s)\n",
            m->name, m->path, m->writable ? "read/write" : "read-only");
    return true;
}

bool host_mounts_install(const HostConfig *hc,
                         TrashfsVolume   *trash_vol,
                         uint32_t         trash_region_kb,
                         const char      *host_fs_root,
                         bool             host_fs_writable,
                         bool             host_fs_disabled) {
    if (hc->mount_count == 0) {
        return install_defaults(trash_vol, host_fs_root,
                                host_fs_writable, host_fs_disabled);
    }

    bool any_td_mounted = false;
    for (unsigned i = 0; i < hc->mount_count; i++) {
        if (!install_one(&hc->mounts[i], trash_vol, trash_region_kb,
                         &any_td_mounted)) {
            return false;
        }
    }
    return true;
}
