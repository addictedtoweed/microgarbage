/* ============================================================
 *  host_mounts.h — install the shell host's mount table.
 *
 *  Reads HostConfig.mounts[] (set by vm.cfg) or falls back to the
 *  built-in default pair (/td0 + /host) when no mounts were
 *  configured. Calls into vm_host_fs_mount_{trashfs,host} for each
 *  entry; emits a one-line status to stderr for each mount.
 *
 *  Failure modes:
 *    - tmpfs/sd mount install failure: hard fail (return false)
 *    - host (passthrough) mount install failure: soft fail (logged,
 *      mount skipped, /host stays unmounted)
 *    - host_fs_root doesn't exist: try to create it; if that fails,
 *      log and skip the /host mount (matches the old behaviour).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HOST_MOUNTS_H
#define HOST_MOUNTS_H

#include <stdbool.h>
#include <stdint.h>

#include "host_config.h"
#include "storage/trashfs.h"

/* Install the mounts described by hc. If hc->mount_count == 0,
 * installs the built-in defaults: /td0 (trashfs RAM disk) and,
 * unless host_fs_disabled is true, /host (passthrough to
 * host_fs_root). The trashfs region size is needed only for the
 * status message that names the /td0 size. Returns true on success
 * (hard mount failures); soft failures are logged to stderr. */
bool host_mounts_install(const HostConfig *hc,
                         TrashfsVolume   *trash_vol,
                         uint32_t         trash_region_kb,
                         const char      *host_fs_root,
                         bool             host_fs_writable,
                         bool             host_fs_disabled);

#endif /* HOST_MOUNTS_H */
