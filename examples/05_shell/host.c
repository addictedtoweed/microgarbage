/* 05_shell/host.c — bring up the file-system shell guest.
 *
 * main() orchestrates the per-concern modules (host_*.{h,c}); see
 * each module's header for its contract. The pieces, in install
 * order:
 *
 *   host_cli       parse argv into a HostCli
 *   host_config    layer defaults + vm.cfg + CLI overrides
 *   host_audio     start the AudioService worker + sink
 *   host_pty       optional POSIX pty transport (--pty)
 *   host_tcp       TCP socket transport (--tcp=<port>)
 *   host_runloop   multi-session TCP accept/reap/step loop
 *   host_mounts    install /td0 + /host (or vm.cfg's mounts)
 *   host_util      mkdir / exe-dir / load_file / no-console warning
 *
 * Static storage in this file: the shared + local slab regions
 * (sized at compile time from host_config.h), the trashfs RAM disk,
 * and the trashfs volume struct. Everything else flows through
 * the modules.
 *
 * Public domain (CC0). No warranty.
 */

#define _POSIX_C_SOURCE 200809L
/* For posix_openpt, grantpt, unlockpt, ptsname (XSI ext). */
#define _XOPEN_SOURCE   600
/* For cfmakeraw on glibc. */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif

/* host_tcp.h MUST come before any header that might transitively
 * pull <windows.h>, because it pulls <winsock2.h> first to win
 * the v1/v2 race (windows.h drags winsock.h v1 which conflicts
 * with v2). All other module headers go below. */
#include "host_tcp.h"

/* Project headers (vm + storage + util). */
#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_transport.h"
#include "vm/vm_host_fs.h"
#include "vm/vm_host_platform.h"
#include "vm/vm_host_tui.h"
#include "vm/vm_ecall.h"
#include "vm/vm_core.h"
#include "vm/host_platform.h"
#include "vm/host_compat.h"
#include "storage/trashfs.h"
#include "shell_embedded.h"

/* Local modules. host_tcp.h is up top for the winsock ordering; the
 * rest go here alphabetical to keep "what does this depend on?"
 * scannable. */
#include "host_audio.h"
#include "host_cli.h"
#include "host_config.h"
#include "host_mounts.h"
#include "host_pty.h"
#include "host_runloop.h"
#include "host_util.h"

/* Standard C / POSIX. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ---- Static storage backing main()'s install sequence ---- */

static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];

/* trashfs RAM disk — the default writable volume, mounted as /td0.
 * 128 KB here; on a real target, size to available internal RAM
 * / PSRAM. The filesystem owns this region directly (no block-
 * device layer). */
#define TRASHFS_REGION_BYTES (128 * 1024)
static uint8_t       g_trashfs_region[TRASHFS_REGION_BYTES];
static TrashfsVolume g_trashfs_vol;

int main(int argc, char **argv) {
#ifdef _WIN32
    /* If we were launched without a console attached (e.g. as
     * a GUI-subsystem binary from Explorer, or as a mingw
     * console binary with stdin/out somehow detached), attach
     * to the parent's console or allocate a fresh one. This
     * keeps double-click usable and means a native Windows
     * binary launched from cmd doesn't pop an extra window. */
    vm_host_stdio_win32_attach_console_if_native();
#endif
    /* ----- Parse args ----- 
     *
     * Usage: host [options] [shell.elf]
     *
     * Options live in host_cli.{h,c}: that owns the parser, the
     * --help text, range-checking of integer overrides, and the
     * HostCli struct that bundles every flag. We alias the fields
     * back into locals so the bring-up code below reads the same
     * as it did before the cli cut. */
    HostCli cli;
    host_cli_set_defaults(&cli);
    if (!host_cli_parse(argc, argv, &cli)) return 1;

    const char *elf_path              = cli.elf_path;
    const char *host_fs_root          = cli.host_fs_root;
    bool        host_fs_root_explicit = cli.host_fs_root_explicit;
    bool        host_fs_writable      = cli.host_fs_writable;
    bool        host_fs_disabled      = cli.host_fs_disabled;
    bool        want_pty              = cli.want_pty;
    int         n_tcp_ports           = cli.n_tcp_ports;
    const int  *tcp_ports             = cli.tcp_ports;
    const char *cfg_path              = cli.cfg_path;
    bool        no_config             = cli.no_config;

    /* No explicit ELF path -> use the baked-in shell image (XIP).
     * An explicit path still loads from disk (COPY_RAM) for dev. */
    bool use_embedded = (elf_path == NULL);
    if (use_embedded && shell_elf_len == 0) {
        fprintf(stderr,
            "host: no embedded shell in this build and no ELF path given.\n"
            "      Pass a path, e.g.  host build/shell.elf\n"
            "      (embedded shell is omitted when the build had no guest\n"
            "      cross-compiler.)\n");
        return 1;
    }

    /* ----- Load HostConfig: defaults -> vm.cfg -> CLI overrides ----- */
    HostConfig hc;
    host_config_set_defaults(&hc);
    if (!no_config) {
        if (!host_config_load(cfg_path, &hc)) return 1;
    } else if (cfg_path) {
        fprintf(stderr, "host: --no-config and --config are mutually exclusive\n");
        return 1;
    }
    if (!host_cli_apply_to_config(&cli, &hc)) return 1;

#ifndef PTY_MODE_SUPPORTED
    if (want_pty) {
        fprintf(stderr, "host: --pty is not supported on this platform "
                        "(needs POSIX posix_openpt/grantpt).\n");
        return 1;
    }
#endif
    /* --pty is a single-instance transport; TCP can be multi-instance
     * (multiple --tcp= ports = multiple sessions). Mixing them isn't
     * supported — pick either --pty OR one-or-more --tcp ports. */
    if (want_pty && n_tcp_ports > 0) {
        fprintf(stderr, "host: --tcp can't be combined with --pty (for now)\n");
        return 1;
    }

    /* Install the platform's stop/interrupt handler(s): SIGINT on
     * POSIX, SIGINT + console control handler on Windows. The run
     * loop below polls host_platform_stop_requested(). */
    host_platform_install_stop_handler();

    /* Host-policy advice (native Windows only): warn if there's no
     * real console, where Ctrl-C can't be delivered. No-op elsewhere. */
    warn_if_no_real_console();

    /* 1. Format + mount the trashfs RAM disk — the default writable
     * volume, mounted as /td0 below. RAM-backed, so we format fresh
     * each run (for persistence you'd skip the format and just mount a
     * pre-formatted region). now=0: RTC not yet wired, so timestamps
     * are 0 (see docs/trashfs-format.md). */
    if (trashfs_format(g_trashfs_region, TRASHFS_REGION_BYTES, 0, 0)
            != TRASHFS_OK) {
        fprintf(stderr, "host: trashfs_format failed\n");
        return 1;
    }
    if (trashfs_mount(&g_trashfs_vol, g_trashfs_region, TRASHFS_REGION_BYTES)
            != TRASHFS_OK) {
        fprintf(stderr, "host: trashfs_mount failed\n");
        return 1;
    }

    /* Pre-create a few items so `ls` has something to show on first
     * launch. Pure convenience — remove for a truly empty start. */
    trashfs_mkdir(&g_trashfs_vol, "/home", 0);
    trashfs_mkdir(&g_trashfs_vol, "/tmp", 0);
    {
        TrashfsFile f;
        if (trashfs_open(&g_trashfs_vol, "/readme.txt",
                         TRASHFS_O_CREAT | TRASHFS_O_TRUNC, &f) == TRASHFS_OK) {
            const char *msg =
                "Welcome to the VM shell.\n"
                "Try: ls, cd home, mkdir foo, touch bar.txt, cat readme.txt\n"
                "\n"
                "Filesystem layout:\n"
                "  /td0/    the RAM-backed trashfs volume (default cwd)\n"
                "  /host/   host directory passthrough (read-only)\n"
                "\n"
                "Absolute paths must start with /<name>/. Relative\n"
                "paths are resolved against the current directory.\n";
            uint32_t off = 0, len = (uint32_t)strlen(msg), w;
            while (off < len &&
                   trashfs_write(&f, msg + off, len - off, &w, 0) == TRASHFS_OK
                   && w > 0) {
                off += w;
            }
            trashfs_close(&f);
        }
    }

    /* 5. Resolve the guest ELF image: the baked-in array (XIP) or a
     * file from disk (COPY_RAM). For the embedded image we keep a
     * const pointer; for the disk path we own a malloc'd buffer. The
     * backing mode is chosen to match: XIP for the embedded image
     * (executes straight out of the host's .rodata — flash, on an
     * MCU), COPY_RAM for a disk image (no stable backing to point
     * at). */
    const uint8_t *elf = NULL;
    size_t   elf_size  = 0;
    uint8_t *elf_owned = NULL;   /* non-NULL only for the disk path */
    VmBacking elf_backing = VM_BACKING_COPY_RAM;
    if (use_embedded) {
        elf         = shell_elf;
        elf_size    = shell_elf_len;
        elf_backing = VM_BACKING_XIP;
    } else {
        if (load_file(elf_path, &elf_owned, &elf_size) != 0) {
            return 1;
        }
        elf         = elf_owned;
        elf_backing = VM_BACKING_COPY_RAM;
    }

    /* 6. Build the VmSystem and install both bridges. Memory
     * sizing and per-VM limits come from HostConfig (defaults +
     * vm.cfg + CLI overrides). */
    VmSystem sys;
    VmSystemConfig cfg = {
        .shared_storage      = g_shared,
        .shared_storage_size = hc.shared_bytes,
        .local_storage       = g_local,
        .local_storage_size  = hc.local_bytes,

        .max_vms             = hc.max_vms,
        .spawn_data_kb       = hc.spawn_data_kb,

        /* Real-time tick source: 1 ms granularity from
         * CLOCK_MONOTONIC. Guests can use SYS_SLEEP_TICKS and
         * SYS_SLEEP_UNTIL to pace themselves at human timescales
         * (animations, polling, periodic loops). */
        .tick_source         = host_platform_monotonic_ms,
        .ticks_per_second    = 1000,
    };
    if (!vm_system_init(&sys, &cfg)) {
        fprintf(stderr, "host: vm_system_init failed "
                "(local=%llu shared=%llu max_vms=%u spawn_data_kb=%u — "
                "try smaller values in vm.cfg or via --max-vms / "
                "--spawn-data-kb)\n",
                (unsigned long long)hc.local_bytes,
                (unsigned long long)hc.shared_bytes,
                (unsigned)hc.max_vms, (unsigned)hc.spawn_data_kb);
        return 1;
    }

    /* Stdio handlers are always installed: they consult the per-VM
     * transport table for every byte. For the single-instance --pty
     * transport we also set a process-default transport. For multi-
     * instance TCP, we bind per-VM after each connection is accepted
     * and a shell is spawned. */
    {
        VmHostStdioConfig sio = {0};
        sio.raw_mode = (want_pty || n_tcp_ports > 0)
                           ? false : hc.raw_mode;
        if (!vm_host_install_stdio_ex(&sys, &sio)) {
            fprintf(stderr, "host: vm_host_install_stdio failed\n");
            return 1;
        }
    }

    if (want_pty) {
#ifdef PTY_MODE_SUPPORTED
        if (!pty_install(&sys)) {
            return 1;
        }
#else
        return 1;   /* unreachable — checked above */
#endif
    }
    /* TCP multi-session is set up after fs/platform install, just
     * before the run loop (it needs the session pool and spawns
     * VMs lazily as clients connect). */
    if (!vm_host_install_fs(&sys)) {
        fprintf(stderr, "host: vm_host_install_fs failed\n");
        return 1;
    }

    /* 6c. Platform services (printf machinery, realtime clock,
     * PRNG, alloc-introspection, frame-budget helper). Seed the
     * PRNG from the realtime clock so guests get a different
     * sequence on each host run. */
    {
        VmHostPlatformConfig pcfg = {0};
        pcfg.realtime_source   = host_platform_realtime;
        pcfg.realtime_userdata = NULL;
        uint32_t sec = 0, nsec = 0;
        if (host_platform_realtime(NULL, &sec, &nsec)) {
            /* Mix sec and nsec into the seed so two runs in the
             * same second still differ. SplitMix64 will diffuse. */
            pcfg.rand_seed = ((uint64_t)sec << 32) ^
                             (uint64_t)nsec ^
                             0x9E3779B97F4A7C15ULL;
        }
        if (!vm_host_install_platform(&sys, &pcfg)) {
            fprintf(stderr, "host: vm_host_install_platform failed\n");
            return 1;
        }
    }

    /* 6d. TUI service (terminal-canvas drawing). Always installed;
     * guests that don't call SYS_TUI_INIT consume zero state. */
    if (!vm_host_install_tui(&sys)) {
        fprintf(stderr, "host: vm_host_install_tui failed\n");
        return 1;
    }

    /* If host_fs_root is still the built-in default (the user didn't
     * pass --host-fs=), resolve "host_files" next to the EXECUTABLE
     * rather than the current working directory. This makes
     * `./build/host.exe` find `build/host_files` whether you launch
     * from the repo root or from inside build/. An explicit --host-fs=
     * path is always honoured as given. If we can't determine the exe
     * directory, we leave the plain relative default (old behaviour).
     *
     * Static storage: host_fs_root is a const char* held for the life
     * of the program, so the buffer must outlive this scope. */
    static char host_fs_root_buf[1056];
    if (!host_fs_root_explicit) {
        char exedir[1024];
        if (host_exe_dir(exedir, sizeof(exedir))) {
            int w = snprintf(host_fs_root_buf, sizeof(host_fs_root_buf),
                             "%s/%s", exedir, host_fs_root);
            if (w > 0 && (size_t)w < sizeof(host_fs_root_buf)) {
                host_fs_root = host_fs_root_buf;
            }
            /* else: path too long — keep the plain default, no harm */
        }
    }

    /* 6e. Audio service (the desktop "M4"): a worker thread runs the
     * mixer/pool/arbiter behind the service channel; guest SYS_AUDIO_*
     * calls post to it. Non-fatal if it fails to start — the shell
     * still runs, guests' audio calls just return failure. Available on
     * native Windows (channel_win32.c transport + waveOut sink) as well
     * as Cygwin/POSIX. NOTE: this gets audio flowing to the service's
     * output ring; an actual sound-device backend (ring -> speakers) is
     * separate and platform-specific. */
#ifdef HOST_AUDIO_SUPPORTED
    /* Audio resolves "/host/x" to a native path itself (the service reads
     * the file directly off disk/SD), so it MUST use the same root as the
     * real /host mount. When vm.cfg remaps /host via a [mount.host]
     * section, honor that path; otherwise use the resolved default.
     * Without this, a config that points /host elsewhere leaves audio
     * looking in the wrong directory (e.g. an empty build/host_files) and
     * every stream/load of a /host wav silently fails. */
    const char *audio_host_root = host_fs_root;
    for (unsigned i = 0; i < hc.mount_count; i++) {
        if (hc.mounts[i].kind == HOST_MOUNT_HOST &&
            strcmp(hc.mounts[i].name, "host") == 0 &&
            hc.mounts[i].path[0]) {
            audio_host_root = hc.mounts[i].path;
            break;
        }
    }
    if (!host_audio_start(&sys, audio_host_root, &g_trashfs_vol)) {
        fprintf(stderr, "host: audio service not started "
                        "(continuing without audio)\n");
    }
#endif

    /* 6b. Install the mount table (host_mounts.c). The mount installer
     * is leaf-shaped: it takes the loaded HostConfig + the static
     * trashfs region + the /host CLI knobs, prints one line per
     * mount, and returns. */
    if (!host_mounts_install(&hc, &g_trashfs_vol,
                             TRASHFS_REGION_BYTES / 1024,
                             host_fs_root, host_fs_writable,
                             host_fs_disabled)) {
        return 1;
    }

    /* ============================================================
     *  7+8. Load shells and run.
     *
     *  Two modes:
     *   (a) Multi-session TCP: one shell VM per --tcp= port,
     *       reaping + reaccepting per slot. Lives in host_runloop.c.
     *   (b) Single session: pty / default stdio. Load one shell,
     *       step until it halts.
     * ============================================================ */

#ifdef TCP_MODE_SUPPORTED
    if (n_tcp_ports > 0) {
        static HostRunloopTcp rl;
        if (!host_runloop_tcp_setup(&rl, tcp_ports, n_tcp_ports)) {
            return 1;
        }
        host_runloop_tcp_run(&rl, &sys, elf, elf_size, 16, elf_backing);
        host_runloop_tcp_teardown(&rl);
    } else
#endif
    {

    /* ----- Single-session path (pty / default stdio) ----- */

    /* 7. Load the shell. */
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 16 * 1024,
                                          elf_backing,
                                          elf_backing);
    if (lr.code != VM_SYS_OK) {
        fprintf(stderr, "host: load failed (code=%d)\n", lr.code);
        return 1;
    }

    /* 8. Run. The shell never exits on its own unless the user
     * types 'exit' (or the host terminates). We use the stepping
     * loop rather than vm_system_run so SIGINT can break us out
     * cleanly. */
    for (;;) {
        if (host_platform_stop_requested()) {
            fprintf(stderr, "\nhost: stop requested, shutting down.\n");
            break;
        }
        VmSchedStepResult r = vm_system_step(&sys);
        if (r == VM_SCHED_ALL_HALTED) {
            break;
        }
        if (r != VM_SCHED_RAN) {
            /* IDLE: the shell is parked waiting for input (it sleeps
             * briefly between polls rather than busy-yielding). Yield
             * the CPU so we don't spin a core at 100% — which also
             * keeps SIGINT responsive on Cygwin. */
            host_platform_sleep_ms(5);
        }
    }
    }  /* end single-session else branch */

    /* Shared shutdown for both TCP and single-session paths. */
#ifdef HOST_AUDIO_SUPPORTED
    host_audio_stop();
#endif
    vm_system_destroy(&sys);
    free(elf_owned);   /* NULL for the embedded/XIP image — safe */
    return 0;
}
