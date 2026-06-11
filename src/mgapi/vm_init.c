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
#include "vm/vm_core.h"
#include "vm/vm_sched.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_fs.h"
#include "storage/trashfs.h"
#include "audio_init.h"
#include "copro_ecalls.h"
#include "l2_ecalls.h"

#include "cart_window.h"
#include "copro_mg_handlers.h"
#include "copro_mg_state.h"

#include "io/stream_arbiter.h"
#include "io/stream_ecalls.h"

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
extern const unsigned char demo_dynamic_letterbox_elf[];
extern const size_t        demo_dynamic_letterbox_elf_len;
extern const unsigned char demo_sprite_elf   [];
extern const size_t        demo_sprite_elf_len;
extern const unsigned char demo_mode7_elf    [];
extern const size_t        demo_mode7_elf_len;
extern const unsigned char demo_mode7_3d_elf [];
extern const size_t        demo_mode7_3d_elf_len;
extern const unsigned char demo_audio_mixer_elf [];
extern const size_t        demo_audio_mixer_elf_len;
extern const unsigned char demo_pcm_stream_elf [];
extern const size_t        demo_pcm_stream_elf_len;
extern const unsigned char demo_fmv_elf [];
extern const size_t        demo_fmv_elf_len;
extern const unsigned char demo_fmv_still_elf [];
extern const size_t        demo_fmv_still_elf_len;
extern const unsigned char demo_nmi_smoke_elf [];
extern const size_t        demo_nmi_smoke_elf_len;

/* v2.30.7 Phase 3b: cart_window frame_consumed hook → VM scheduler.
 * Called whenever cart_window's port-7-read callback bumps
 * g_frame_consumed (i.e., the SNES kernel processed a frame).
 * Wakes any VM blocked on BLOCK_FRAME_CONSUMED whose stored target
 * has been reached. */
static void frame_consumed_wake_hook(uint32_t now_consumed, void *userdata) {
    VmSystem *sys = (VmSystem *)userdata;
    if (sys && sys->sched) {
        (void)vm_sched_wake_frame_consumed(sys->sched, now_consumed);
    }
}

/* Adapter for vm_system unload hook (which passes vm_id + userdata) to
 * mg_state_reset's parameterless signature. Registered once at init.
 * See the call site below for the rationale (per-spawn shadow reset). */
static void mg_state_reset_on_unload(uint16_t vm_id, void *userdata) {
    (void)vm_id;
    (void)userdata;
    mg_state_reset();
    /* v2.21: drop any custom NMI handler the unloading guest installed,
     * so the next guest gets the kernel's default NMI proc instead of
     * inheriting a stale handler (which manifested as "after running
     * nmi_smoke, everything stays red regardless of which demo runs
     * next" — the no-op handler from nmi_smoke stage C kept being
     * dispatched). */
    cart_window_clear_nmi_version();
    /* v2.26: same uninstall protocol for HIRQ — set version 0 so the
     * kernel @loop's HIRQ poll restores the IRQ vector to default
     * (disabled — boot init leaves $0202 = 0). Also zero the HIRQ
     * schedule so the kernel writes NMITIMEN=$80 (just NMI, no IRQ
     * enables) on its next loop iter, even if the previous demo had
     * armed HIRQ. Otherwise the next demo would keep firing the
     * stale IRQ vector. */
    cart_window_clear_hirq_version();
    {
        static const uint8_t zeros[5] = {0};
        cart_window_load_blob(CW_OFF_HIRQ_SCHED, zeros, sizeof zeros);
    }
    /* v2.29 Phase 3a: clear unified-kernel layout + siphon config so
     * the previous demo's letterbox / siphon doesn't leak into the
     * next guest. NMI handler picks up the zero values at next vblank
     * and the default HIRQ ISR returns to "no letterbox, no siphon"
     * baseline. */
    {
        static const uint8_t zeros[8] = {0};
        cart_window_load_blob(CW_OFF_KERNEL_LAYOUT, zeros, sizeof zeros);
    }
    /* v2.24 (bisect): the v2.22 auto-arm of a VRAM clear here regressed
     * demo_audio_mixer's second-run-onwards: CGRAM[1] (the yellow font
     * color) came up pure black, with similar effects on CGRAM[129]
     * (cyan FFT bar). The full-region-CGRAM DMA staged by the demo's
     * own clean_slate apparently doesn't fully land when slot 0's VRAM
     * clear was first armed by the unload hook (instead of by the demo
     * itself), even though clean_slate calls mg_state_reset which is
     * supposed to discard the unload-armed slot. Root cause is somewhere
     * in the interaction between the two arms; for now, dropping the
     * unload-side arm restores the v2.20 behavior for audio_mixer / sprite
     * while keeping the v2.21 NMI version clear that fixes nmi_smoke.
     * Demos that don't call clean_slate (palette / letterbox / mode7)
     * will again inherit the previous demo's VRAM — to compensate, those
     * demos should be patched to call mg_ppu_clean_slate at startup. */
}

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

    /* v2.30.7 Phase 3b: wire cart_window's frame_consumed bump to
     * the VM scheduler so BLOCK_FRAME_CONSUMED waiters wake
     * deterministically when the SNES kernel acks a frame. */
    cart_window_set_frame_consumed_hook(
        &frame_consumed_wake_hook, &g_sys);

    /* 3. Install bridges.
     *
     *    Stdio handler registration is unconditional -- the SYS_READ /
     *    WRITE / FFLUSH handlers MUST be present so guests can do
     *    I/O through whichever transport is bound (TCP from
     *    tcp_listen.c, future UART on the M7, etc.). What the
     *    `disable_default_stdio` config flag actually wants is the
     *    second-order effect: don't fall through to the host's
     *    stdin/stdout when no transport is bound. We thread that
     *    through as VmHostStdioConfig.no_default_files so the
     *    handlers stay live but g_in_file/g_out_file stay NULL.
     *
     *    Background: a Windows GUI-subsystem .exe (bsnes-plus)
     *    LoadLibrary'ing mgapi.dll inherits stdin/stdout handles
     *    that are typically broken -- fwrite either blocks or fails
     *    silently in a way that wedges subsequent reads. Falling
     *    through to those handles would make the shell unrecoverable
     *    in that embedding, even though every guest I/O call would
     *    route correctly through TCP once a client connected. The
     *    no_default_files path is the explicit opt-out.
     *
     *    The fs install is what makes /td0/, /cart/, and /host/
     *    visible to guests AND enables SYS_SPAWN_AND_WAIT (the
     *    shell's `run <path>` command). */
    {
        VmHostStdioConfig sio = {0};
        sio.no_default_files = g_disable_default_stdio ? true : false;
        if (!vm_host_install_stdio_ex(&g_sys, &sio)) goto fail_sys;
    }
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

    /* Reset mg_state on every VM unload so the next demo starts with
     * clean shadow state. Without this, BG enables, HDMA channels,
     * sprite config, M7 wrap, and force-blank settings leak between
     * demos (only CGRAM/OAM/CHR get re-staged because they're shadow-
     * backed; the boolean/enum config bits persist). Observed symptoms
     * before this hook: palette.elf after letterbox.elf doesn't show
     * its red backdrop because BG1 stays enabled from letterbox; demos
     * after mode7_3d.elf get mangled video because HDMA channels 1..4
     * stay enabled with stale table_off pointers. */
    (void)vm_system_register_unload_hook(&g_sys,
        &mg_state_reset_on_unload, NULL);

    /* Stage 3c: SYS_L2_* ecalls. Handlers convert host pointers <->
     * guest VAs against the same L2 backing the VM core's translation
     * uses for the upper half of SHARED. */
    if (g_vm_l2_base && g_vm_l2_size > 0) {
        if (!mgapi_install_l2_ecalls(&g_sys, g_vm_l2_base, g_vm_l2_size))
            goto fail_sys;
    }

    /* v2.10: SYS_STREAM_* ecalls + the global round-robin file-stream
     * arbiter that drives them. The arbiter ticks once per worker
     * tick (before vm_step, see mgapi_step_body) so guests find their
     * ring filled when they call mg_stream_consume. Cheap when no
     * streams are registered. */
    if (!stream_arbiter_init()) goto fail_sys;
    if (!mgapi_install_stream_ecalls(&g_sys)) goto fail_sys;

    /* 4. Mount table:
     *      /td0/  small RAM-disk scratch (writable, this DLL's BSS)
     *      /cart/ large PSRAM read-mostly bulk (the volume stage 2c
     *              brought up; we just register its handle).
     *      /host/ read-only passthrough to the bsnes-plus working dir's
     *             "host" subfolder. Lets demos load assets the developer
     *             dropped next to bsnes.exe (music.wav, sfx*.wav,
     *             video.fmv) without rebuilding the trashfs image. */
    if (!vm_host_fs_mount_trashfs("td0", &g_td0_vol)) goto fail_sys;
    if (cart_volume_handle) {
        if (!vm_host_fs_mount_trashfs("cart", cart_volume_handle)) {
            /* Non-fatal: the rest of the system still works without
             * /cart/. Log via stderr so the embedder notices. */
        }
    }
    /* Non-fatal if the host directory doesn't exist; the user just
     * doesn't get /host/ access until they create it. */
    if (!vm_host_fs_mount_host("host", "./host", /*writable=*/false)) {
        fprintf(stderr,
                "mgapi: /host/ mount skipped (./host not a directory) "
                "— demos that read /host/*.wav or /host/video.fmv will "
                "see ENOENT until you create ./host next to bsnes.exe\n");
    }

    /* Now that the VmSystem is up, hand the SYS_AUDIO_* ecall handlers
     * to it so guests calling mg_sfx_load / mg_stream_play /
     * audio_get_levels actually reach the audio service mgapi_audio_init
     * brought up earlier. host_fs_root="./host" matches the /host/ mount
     * above so the resolved host paths agree. Non-fatal if it fails -
     * audio just stays unavailable (mg_sfx_load returns 0). */
    if (!mgapi_audio_install_ecalls(&g_sys, "./host")) {
        fprintf(stderr,
                "mgapi: audio ecalls not installed -- guests' mg_sfx_load "
                "and friends will return 0\n");
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
    install_demo("/demos/dynamic_letterbox.elf", demo_dynamic_letterbox_elf, demo_dynamic_letterbox_elf_len);
    install_demo("/demos/sprite.elf",    demo_sprite_elf,    demo_sprite_elf_len);
    install_demo("/demos/mode7.elf",     demo_mode7_elf,     demo_mode7_elf_len);
    install_demo("/demos/mode7_3d.elf",    demo_mode7_3d_elf,    demo_mode7_3d_elf_len);
    install_demo("/demos/audio_mixer.elf", demo_audio_mixer_elf, demo_audio_mixer_elf_len);
    install_demo("/demos/pcm_stream.elf",  demo_pcm_stream_elf,  demo_pcm_stream_elf_len);
    install_demo("/demos/fmv.elf",         demo_fmv_elf,         demo_fmv_elf_len);
    install_demo("/demos/fmv_still.elf",   demo_fmv_still_elf,   demo_fmv_still_elf_len);
    install_demo("/demos/nmi_smoke.elf",   demo_nmi_smoke_elf,   demo_nmi_smoke_elf_len);
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

/* Exposed so tcp_listen can route a Ctrl-C byte from PuTTY into a
 * halt of the currently spawned demo. Walks the VM table looking for
 * a parent in BLOCK_ON_CHILD, halts its child cleanly so the next
 * reap delivers an exit code and the shell resumes its prompt.
 * Returns the killed child's vm_id, or UINT16_MAX if no spawn was
 * running. */
uint16_t mgapi_vm_kill_running_spawn(void) {
    if (!g_sys_alive) return (uint16_t)UINT16_MAX;
    for (uint16_t pid = 0; pid < VM_SCHED_MAX_VMS; pid++) {
        VmCpu *p = g_sys.vms[pid];
        if (!p) continue;
        if (p->block_reason != BLOCK_ON_CHILD) continue;
        uint16_t cid = p->block_child_vm;
        if (cid >= VM_SCHED_MAX_VMS) continue;
        VmCpu *c = g_sys.vms[cid];
        if (!c || c->halted) continue;
        /* Halt the child cleanly. trap_cause = TRAP_HALT signals the
         * reap path that this isn't a crash; exit code 130 follows
         * the Unix convention for SIGINT-killed processes (128 + 2). */
        c->halted     = true;
        c->trap_cause = TRAP_HALT;
        c->regs[VM_REG_A0] = 130u;
        return cid;
    }
    return (uint16_t)UINT16_MAX;
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
