/* ============================================================
 *  vm_host_fs_spawn.c — SYS_SPAWN_AND_WAIT handler + config.
 *
 *  Pulled out of vm_host_fs.c so that file stays under the
 *  conventions.md soft cap. The split is along the natural seam:
 *  spawn is the one file syscall that loads an ELF into a brand-
 *  new VM and parks the parent on the child, rather than touching
 *  the fd table like every other handler.
 *
 *  vm_host_fs.c keeps the path resolver and lock helpers; this
 *  file imports them via vm_host_fs_internal.h.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_host_fs_internal.h"

#include "vm/vm_system.h"
#include "vm/vm_host_transport.h"

#if GARBAGE_SCHED_MODE == GARBAGE_SCHED_PREEMPTIVE
#  include "vm/presched.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* ============================================================
 *  Per-spawned-VM data region size
 *
 *  The shell example might want a larger value than 16 KB if it
 *  spawns guests that allocate a lot; smaller embedded systems
 *  might dial it down. This is the LEGACY knob — the modern path
 *  is VmSystemConfig.spawn_data_kb. handle_spawn_and_wait prefers
 *  the config field and falls back to this for hosts that haven't
 *  migrated.
 * ============================================================ */
static uint32_t g_spawn_data_size = 16 * 1024;

bool vm_host_fs_set_spawn_data_size(uint32_t bytes) {
    if (bytes < 4096 || (bytes & 4095) != 0) return false;
    g_spawn_data_size = bytes;
    return true;
}

uint32_t vm_host_fs_get_spawn_data_size(void) {
    return g_spawn_data_size;
}

/* ============================================================
 *  slurp_file
 *
 *  Read an entire file into a freshly malloc'd buffer. Works for
 *  both trashfs and host-filesystem paths. The path string is the
 *  already-resolved host-side path (a trashfs mount-relative
 *  path, or e.g. "C:/host_files/foo.elf" for the host mount).
 *
 *  Exposed via vm_host_fs_internal.h in case another sibling
 *  module needs it later; today's only caller is the spawn
 *  handler immediately below.
 * ============================================================ */
uint8_t *slurp_file(const char *path, PathBackend backend,
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
    }
    /* Only HOST and TRASHFS backends exist. */
    *out_err = VM_EIO;
    return NULL;
}

/* ============================================================
 *  SYS_SPAWN_AND_WAIT
 *
 *    a0 = path (guest ptr to null-terminated string)
 *    -> a0 = exit code of spawned VM (0..255 from its SYS_EXIT),
 *           or -errno on failure
 *
 *  Loads the file at `path` as a new VM in the same VmSystem,
 *  then BLOCKS the parent on the child and returns to the
 *  scheduler — the spawn is ASYNCHRONOUS. The host's main
 *  vm_system_step loop runs the child alongside every other VM,
 *  so spawning a long-lived program in one session does NOT
 *  freeze the others (this is what makes concurrent multi-
 *  session apps work). The parent resumes with the child's exit
 *  code when the child is reaped
 *  (vm_system_reap_halted_children). From the parent guest's
 *  view the call still looks synchronous: it returns only once
 *  the child exits.
 * ============================================================ */
void vm_host_fs_handle_spawn_and_wait(VmCpu *cpu, void *system) {
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

    /* Determine the spawn data region size for the child. Prefer
     * sys->config.spawn_data_kb (the modern path); fall back to the
     * legacy global if the host didn't migrate. */
    uint32_t spawn_data_bytes = (uint32_t)sys->config.spawn_data_kb * 1024u;
    if (spawn_data_bytes == 0) {
        spawn_data_bytes = g_spawn_data_size;
    }

    /* Load as a new VM. VM_BACKING_COPY_RAM means the loader copies
     * the bytes it needs out of our buffer, so we can free the buffer
     * after vm_system_load_vm returns.
     *
     * Per-allocation freeing via the slab means we can unload the
     * child cleanly on halt — see vm_system_unload_vm at the bottom
     * of this handler. */
    VmLoadVmResult lr = vm_system_load_vm(sys, elf, elf_size,
                                          spawn_data_bytes,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    free(elf);

    if (lr.code != VM_SYS_OK) {
        /* Load failure: nothing committed — vm_system_load_vm does its
         * own cleanup on partial failure. Just report and return. */
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

    /* Spawn is ASYNCHRONOUS: register the child as a normal scheduler
     * VM and BLOCK THE PARENT on it, then return to the scheduler. The
     * host's main vm_system_step loop runs the child alongside every
     * other session — so spawning a long-lived TUI program in one
     * session no longer freezes the others.
     *
     * Lifecycle:
     *   - here: load child, inherit transport, park parent
     *           (BLOCK_ON_CHILD, block_child_vm = child id)
     *   - main loop: scheduler runs child + all siblings
     *   - child halts: vm_system_reap_halted_children() delivers the
     *     child's exit code to the parent's a0, wakes the parent,
     *     clears the child's transport binding, and unloads the child
     *
     * (The old synchronous design pumped the child in a nested loop
     * right here, which commandeered the host run loop and starved
     * all other VMs for the child's entire lifetime — the multi-
     * session freeze.) */
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
