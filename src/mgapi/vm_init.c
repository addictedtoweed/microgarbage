/* ============================================================
 *  vm_init.c — VM system + shell ELF boot inside mgapi.
 *
 *  Mirrors examples/05_shell/host.c lines 1606-2061 (system init,
 *  fs install, mount table, load+step) trimmed to what the cart
 *  runtime needs.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_init.h"

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_fs.h"
#include "storage/trashfs.h"
#include "copro_ecalls.h"
#include "l2_ecalls.h"

#include "copro_mg_handlers.h"
#include "copro_mg_state.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* The embedded shell ELF, baked in by bin2c at build time
 * (build/mgapi/gen/shell_elf_data.c). When the RISC-V cross compiler
 * isn't installed the array is zero-length and we degrade to
 * "VmSystem alive, no VM loaded." */
extern const unsigned char shell_elf[];
extern const size_t        shell_elf_len;

/* Bundled demo guest ELFs. Installed into /td0/demos/ at boot so a
 * PuTTY shell can `run /td0/demos/demo_<name>.elf`. Each is a tiny
 * one-feature sample (palette cycle, force-blank toggle, walkable
 * sprite); see tools/guests/demos/. Same zero-length-when-absent
 * fallback as shell_elf. */
extern const unsigned char demo_palette_elf  [];
extern const size_t        demo_palette_elf_len;
extern const unsigned char demo_letterbox_elf[];
extern const size_t        demo_letterbox_elf_len;
extern const unsigned char demo_sprite_elf   [];
extern const size_t        demo_sprite_elf_len;
extern const unsigned char demo_mode7_elf    [];
extern const size_t        demo_mode7_elf_len;

/* install_bundled_demos lives below the g_td0_vol definition so the
 * helper can reach it. The forward declaration here just lets
 * mgapi_vm_init call it. */
static void install_bundled_demos(void);

/* Host clock for the scheduler's tick source — provided by
 * src/host/platform_win.c, declared via vm/host_compat.h. */
extern uint32_t host_platform_monotonic_ms(void *userdata);

/* ----------------------------------------------------------------
 *  Storage. Backed by static arrays in the DLL's BSS / data
 *  sections. The numbers track host.c's defaults: 64 KB shared
 *  message storage + 1.5 MB local slab + 128 KB trashfs RAM disk
 *  for /td0/. /cart/ is the 1 MB PSRAM volume (owned by
 *  cart_volume.c; we just register it).
 * ---------------------------------------------------------------- */

#define MGAPI_VM_SHARED_BYTES   (64u  * 1024u)
#define MGAPI_VM_LOCAL_BYTES    (1536u * 1024u)
#define MGAPI_VM_TD0_BYTES      (128u * 1024u)
#define MGAPI_VM_SPAWN_DATA_KB  64u
#define MGAPI_VM_MAX_VMS        4u

static uint8_t       g_shared[MGAPI_VM_SHARED_BYTES];
static uint8_t       g_local [MGAPI_VM_LOCAL_BYTES];
static uint8_t       g_td0   [MGAPI_VM_TD0_BYTES];
static TrashfsVolume g_td0_vol;

static VmSystem      g_sys;
static int           g_sys_alive;
static uint16_t      g_shell_vm_id;
static int           g_shell_loaded;

/* ----------------------------------------------------------------
 *  Init / shutdown
 * ---------------------------------------------------------------- */

/* Optional system-wide L2 backing (the upper half of SHARED). The
 * embedder passes the PSRAM L2 slice here so every loaded VM gets
 * to dereference L2 VAs (0xE000_0000+) into that region. Set with
 * mgapi_vm_set_l2_backing BEFORE mgapi_vm_init runs.  */
static void   *g_vm_l2_base;
static size_t  g_vm_l2_size;
static int     g_disable_default_stdio;

void mgapi_vm_set_l2_backing(void *base, size_t size) {
    g_vm_l2_base = base;
    g_vm_l2_size = size;
}

void mgapi_vm_set_disable_default_stdio(int flag) {
    g_disable_default_stdio = flag ? 1 : 0;
}

int mgapi_vm_init(void *cart_volume_handle) {
    if (g_sys_alive) return -EALREADY;

    /* 1. Format + mount the /td0/ trashfs volume in regular RAM.
     *    Same call shape as cart_volume.c uses for /cart/. */
    if (trashfs_format(g_td0, sizeof g_td0, 0, 0) != TRASHFS_OK) return -EIO;
    if (trashfs_mount(&g_td0_vol, g_td0, sizeof g_td0) != TRASHFS_OK) return -EIO;

    /* 2. Stand up VmSystem. The L2 backing — if the embedder
     *    supplied one — is the upper half of SHARED for every VM
     *    we load. Guests see L2 VAs at 0xE000_0000+ and the VM
     *    translates them into this region transparently. */
    VmSystemConfig cfg = {
        .shared_storage         = g_shared,
        .shared_storage_size    = sizeof g_shared,
        .l2_shared_storage      = g_vm_l2_base,
        .l2_shared_storage_size = g_vm_l2_size,
        .local_storage          = g_local,
        .local_storage_size     = sizeof g_local,
        .max_vms                = MGAPI_VM_MAX_VMS,
        .spawn_data_kb          = MGAPI_VM_SPAWN_DATA_KB,
        .tick_source            = host_platform_monotonic_ms,
        .ticks_per_second       = 1000,
    };
    if (!vm_system_init(&g_sys, &cfg)) return -ENOMEM;
    g_sys_alive = 1;

    /* 3. Install bridges. Stdio installation accepts a NULL config
     *    to mean "default behavior" (raw-mode off, no per-VM
     *    override). For stage 3a there's no TCP transport bound;
     *    stage 4 will hook PuTTY in via vm_host_set_transport_for_vm.
     *
     *    The fs install is what makes /td0/, /cart/, and /host/
     *    visible to guests AND enables SYS_SPAWN_AND_WAIT (the
     *    shell's `run <path>` command). */
    if (!g_disable_default_stdio) {
        VmHostStdioConfig sio = {0};
        if (!vm_host_install_stdio_ex(&g_sys, &sio)) goto fail_sys;
    }
    /* When disable_default_stdio is set the shell's sys_read returns
     * EOF immediately (no transport bound), so the shell exits
     * cleanly on its first read. TCP transport binding (stage 4)
     * still works because it's per-VM, not the default. */
    if (!vm_host_install_fs(&g_sys)) goto fail_sys;

    /* Stage 3b: SYS_COPRO_* ecalls so guests can drive the cart window. */
    if (!mgapi_install_copro_ecalls(&g_sys)) goto fail_sys;

    /* Stage 3b': SYS_MG_* game-API ecalls (1190..1219). The shadow
     * PPU state lives in copro_mg_state; handlers in
     * copro_mg_handlers update it on each ecall, and the
     * SYS_COPRO_FRAME_COMMIT handler walks the shadow at commit to
     * build the DMA descriptor list the SNES kernel consumes. */
    mg_state_init();
    if (!mg_handlers_install(&g_sys)) goto fail_sys;

    /* Stage 3c: SYS_L2_* ecalls. Handlers convert host pointers <->
     * guest VAs against the same L2 backing the VM core's translation
     * uses for the upper half of SHARED. */
    if (g_vm_l2_base && g_vm_l2_size > 0) {
        if (!mgapi_install_l2_ecalls(&g_sys, g_vm_l2_base, g_vm_l2_size))
            goto fail_sys;
    }

    /* 4. Mount table:
     *      /td0/  small RAM-disk scratch (writable, this DLL's BSS)
     *      /cart/ large PSRAM read-mostly bulk (the volume stage 2c
     *              brought up; we just register its handle). */
    if (!vm_host_fs_mount_trashfs("td0", &g_td0_vol)) goto fail_sys;
    if (cart_volume_handle) {
        if (!vm_host_fs_mount_trashfs("cart", cart_volume_handle)) {
            /* Non-fatal: the rest of the system still works without
             * /cart/. Log via stderr so the embedder notices. */
        }
    }

    /* Drop bundled demo ELFs into /td0/demos/ so a PuTTY shell can
     * `run /td0/demos/<name>.elf` without having to upload anything.
     * No-ops when the baked images are empty. */
    install_bundled_demos();

    /* 5. Load the embedded shell ELF as the first VM. XIP backings
     *    so we execute from the read-only baked image. data_region_size
     *    matches host.c's default (16 KB). */
    if (shell_elf_len > 0) {
        VmLoadVmResult lr = vm_system_load_vm(&g_sys,
                                              shell_elf, shell_elf_len,
                                              16 * 1024,
                                              VM_BACKING_XIP,
                                              VM_BACKING_XIP);
        if (lr.code == VM_SYS_OK) {
            g_shell_loaded = 1;
            g_shell_vm_id  = lr.assigned_vm_id;
        }
        /* If the load failed, the VM system is still up — the shell
         * just isn't running. The host test surfaces this. */
    }

    return 0;

fail_sys:
    vm_system_destroy(&g_sys);
    g_sys_alive = 0;
    return -ENOMEM;
}

/* Helper: drop one ELF into /td0/demos/<name>. Silently no-ops when
 * the baked image is empty (no cross compiler at build time). */
static void install_demo(const char *name,
                         const unsigned char *bytes, size_t len) {
    if (len == 0) return;
    TrashfsFile f;
    if (trashfs_open(&g_td0_vol, name,
                     TRASHFS_O_CREAT | TRASHFS_O_TRUNC, &f) != TRASHFS_OK)
        return;
    uint32_t written = 0;
    (void)trashfs_write(&f, bytes, (uint32_t)len, &written, /*now=*/0);
    (void)trashfs_close(&f);
}

static void install_bundled_demos(void) {
    /* Since we just formatted /td0 above, the mkdir always succeeds
     * on first install — check only matters if this got called
     * twice (which mgapi_vm_init guards against via EALREADY). */
    (void)trashfs_mkdir(&g_td0_vol, "/demos", /*now=*/0);
    install_demo("/demos/palette.elf",   demo_palette_elf,   demo_palette_elf_len);
    install_demo("/demos/letterbox.elf", demo_letterbox_elf, demo_letterbox_elf_len);
    install_demo("/demos/sprite.elf",    demo_sprite_elf,    demo_sprite_elf_len);
    install_demo("/demos/mode7.elf",     demo_mode7_elf,     demo_mode7_elf_len);
}

void mgapi_vm_shutdown(void) {
    if (!g_sys_alive) return;
    vm_system_destroy(&g_sys);
    memset(&g_sys, 0, sizeof g_sys);
    g_sys_alive    = 0;
    g_shell_loaded = 0;
    g_shell_vm_id  = 0;
}

bool mgapi_vm_step(void) {
    if (!g_sys_alive) return false;
    VmSchedStepResult r = vm_system_step(&g_sys);
    return r == VM_SCHED_RAN;
}

/* Exposed for dev probes (mgapi_dev_td0_demo_size in mgapi_init.c) that
 * need to read files out of /td0/ to confirm install_bundled_demos
 * actually populated the volume. */
TrashfsVolume *mgapi_vm_td0_volume(void) {
    return g_sys_alive ? &g_td0_vol : NULL;
}

uint16_t mgapi_vm_shell_id(void) {
    return g_shell_loaded ? g_shell_vm_id : (uint16_t)0xFFFFu;
}

#include "vm/vm_core.h"

int mgapi_dev_spawn_elf_and_wait(const void *elf, uint32_t elf_size) {
    if (!g_sys_alive) return -EAGAIN;
    if (!elf || elf_size == 0) return -EINVAL;

    VmLoadVmResult lr = vm_system_load_vm(&g_sys, elf, elf_size,
                                          16 * 1024,
                                          VM_BACKING_XIP,
                                          VM_BACKING_XIP);
    if (lr.code != VM_SYS_OK) return -EIO;
    uint16_t vm_id = (uint16_t)lr.assigned_vm_id;

    /* Pump the scheduler until the spawned VM halts. We use the
     * lower-level scheduler op directly to avoid vm_system_step's
     * automatic reap, which would NULL out vms[vm_id] (and free the
     * VmCpu) before we can read the exit code. After we have the
     * exit code, we unload the VM explicitly. */
    const uint64_t max_iters = 1000000ull;
    int captured_exit_code = -ETIMEDOUT;
    int captured_trap_cause = (int)TRAP_NONE;
    bool finished = false;
    for (uint64_t i = 0; i < max_iters; i++) {
        (void)g_sys.ops.step(g_sys.ops.ctx);
        VmCpu *cpu = g_sys.vms[vm_id];
        if (cpu && cpu->halted) {
            captured_exit_code = (int)cpu->regs[VM_REG_A0];
            captured_trap_cause = (int)cpu->trap_cause;
            finished = true;
            break;
        }
    }
    if (!finished) return -ETIMEDOUT;

    /* Now safely unload the VM (frees the cpu struct + slot). */
    vm_system_unload_vm(&g_sys, vm_id);

    if (captured_trap_cause != (int)TRAP_NONE &&
        captured_trap_cause != (int)TRAP_ECALL) {
        fprintf(stderr, "mgapi: spawned ELF trapped: cause=%d\n",
                captured_trap_cause);
        fflush(stderr);
        return -100 - captured_trap_cause;
    }
    return captured_exit_code;
}

int mgapi_dev_spawn_elf_for_steps(const void *elf, uint32_t elf_size,
                                   uint32_t max_steps) {
    if (!g_sys_alive) return -EAGAIN;
    if (!elf || elf_size == 0) return -EINVAL;

    /* Match the shell's spawn_and_wait config: COPY_RAM + 64KB data
     * region. This way a demo that fails when the shell runs it also
     * fails in this probe — keeping the dev path honest against the
     * real shell path. */
    uint32_t spawn_data_bytes = (uint32_t)g_sys.config.spawn_data_kb * 1024u;
    if (spawn_data_bytes == 0) spawn_data_bytes = 64 * 1024;
    VmLoadVmResult lr = vm_system_load_vm(&g_sys, elf, elf_size,
                                          spawn_data_bytes,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) return -EIO;
    uint16_t vm_id = (uint16_t)lr.assigned_vm_id;

    /* Same low-level step as spawn_and_wait so reap doesn't fire and
     * destroy our peek-target. After max_steps, we manually halt and
     * unload. Stops early if the guest happens to halt on its own. */
    for (uint32_t i = 0; i < max_steps; i++) {
        (void)g_sys.ops.step(g_sys.ops.ctx);
        VmCpu *cpu = g_sys.vms[vm_id];
        if (cpu && cpu->halted) break;
    }
    VmCpu *cpu = g_sys.vms[vm_id];
    if (cpu) cpu->halted = true;
    vm_system_unload_vm(&g_sys, vm_id);
    return 0;
}

void mgapi_vm_stats(MgapiVmStats *out) {
    if (!out) return;
    out->has_shell_elf  = shell_elf_len > 0;
    out->shell_elf_bytes = shell_elf_len;
    out->vm_system_alive = g_sys_alive != 0;
    out->shell_loaded    = g_shell_loaded != 0;
    out->shell_vm_id     = g_shell_vm_id;
}
