/* 05_shell/host.c — run the file-system shell guest.
 *
 * Sets up the full stack:
 *   - 64 KB trashdrive (RAM block device)
 *   - FatFs filesystem mounted at "0:/" (format-on-start)
 *   - VmSystem with stdio bridge and file syscalls installed
 *   - shell.elf loaded as the sole VM
 *
 * Then runs the scheduler until the shell calls SYS_EXIT (or
 * Ctrl-C from the user).
 *
 * Stdio routing:
 *   default        process stdin/stdout/stderr (current behavior)
 *   --pipe=<name>  bidirectional Windows named pipe; PuTTY (or
 *                  another client) connects to it as a "serial"
 *                  line. Decouples VM execution from the local
 *                  terminal's scheduling/rendering and is also a
 *                  realistic stand-in for a UART on real hardware.
 *
 * Build dependencies (beyond the standard -Iinclude):
 *   -Ithird_party/fatfs/source -Ithird_party/fatfs -DHAVE_FATFS
 *
 * Without those flags this example will fail to build because
 * FatFs symbols (f_mount, f_mkfs, etc.) won't resolve. See the
 * build.sh in this directory for the full link line.
 */

#define _POSIX_C_SOURCE 200809L
/* For posix_openpt, grantpt, unlockpt, ptsname (XSI ext). */
#define _XOPEN_SOURCE   600
/* For cfmakeraw on glibc. */
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE
#endif

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
#include "vm/vm_host_transport.h"
#include "vm/vm_host_fs.h"
#include "vm/vm_host_platform.h"
#include "vm/vm_host_tui.h"
#include "vm/vm_ecall.h"
#include "vm/vm_core.h"
#include "storage/trashdrive.h"
#include "storage/trashdrive_fatfs.h"
#include "util/inicfg.h"
#include "ff.h"

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

#include "vm/host_compat.h"

#if defined(__CYGWIN__) || defined(_WIN32)
#  define PIPE_MODE_SUPPORTED 1
#  include <windows.h>
#endif

/* PTY transport requires posix_openpt + grantpt + unlockpt + ptsname.
 * Available on Linux, BSD, macOS, and Cygwin (POSIX-compliant). Not
 * available on mingw / native Windows builds — those have ConPTY
 * instead, which is a completely different API and is not in scope
 * for this round. */
#if !defined(_WIN32) || defined(__CYGWIN__)
#  define PTY_MODE_SUPPORTED 1
#  include <fcntl.h>
#  include <termios.h>
#  include <sys/ioctl.h>
#endif

/* mkdir is single-arg on mingw (Windows doesn't have a permissions
 * concept that maps to a Unix mode). Wrapper to keep the call sites
 * portable. */
#ifdef _WIN32
static int host_mkdir(const char *path, int mode) {
    (void)mode;
    return mkdir(path);
}
#else
static int host_mkdir(const char *path, int mode) {
    return mkdir(path, (mode_t)mode);
}
#endif

/* ---------------------------------------------------------------
 * Backing storage
 *
 * The trashdrive pool needs to be large enough for FatFs R0.16 to
 * lay out a valid FAT volume. Empirically the minimum on R0.16
 * with our config (FM_FAT, n_fat=1) is around 96 KB; below that
 * f_mkfs returns FR_MKFS_ABORTED. We use 128 KB to leave headroom
 * for files plus FatFs's own bookkeeping.
 *
 * If you change this, keep it a multiple of TRASH_SECTOR_SIZE (512).
 * --------------------------------------------------------------- */
#define POOL_BYTES   (128 * 1024)
#define SHARED_BYTES (64 * 1024)
/* LOCAL_BYTES is the local-slab region. The slab carves it into
 * fixed-size bins; each per-VM allocation rounds up to the next
 * bin size. With our config (4 VMs × 64 KB data), each VM consumes
 * roughly:
 *   - VmCpu (280 B)    → 512 B bin
 *   - mailbox (272 B)  → 512 B bin
 *   - text (≤ 30 KB)   → 32 KB bin
 *   - rodata (≤ 4 KB)  → 8 KB bin
 *   - data (64 KB)     → 128 KB bin  (+ slab's 8 B header per block
 *                                       means a 64 KB request needs
 *                                       65544 B of block storage)
 * Plus the slab populates smaller bins (32 B..2 KB) for variable
 * segment sizes — vm_system_local_required reports ~1.3 MB total.
 * We round to 1.5 MB to leave a margin. */
#define LOCAL_BYTES  (1536 * 1024)

static uint8_t g_pool[POOL_BYTES];
static uint8_t g_shared[SHARED_BYTES];
static uint8_t g_local[LOCAL_BYTES];

static TrashDrive g_drive;
static FATFS g_fs;

/* ---------------------------------------------------------------
 * Host configuration (vm.cfg + CLI overrides).
 *
 * Defaults match the historical hardcoded values, so a host
 * launched without any config or flags behaves identically to
 * the M.1b version. Layering is:
 *
 *   1. Start with built-in defaults.
 *   2. If a vm.cfg is found (or --config <path> given), apply it.
 *   3. Apply CLI overrides on top.
 *
 * That way: CLI wins ties, config is just persistent defaults.
 * --------------------------------------------------------------- */

/* Mount entry parsed from [mount.<name>] sections.
 *
 * `tmpfs` and `sd` are separate config type names that both map
 * to FatFs internally on the dev host today. They diverge when
 * the platform moves to hardware:
 *   tmpfs → FatFs over a RAM-backed block device (volatile)
 *   sd    → FatFs over an SD card driver (persistent)
 * On the dev host both look the same; the distinction is intent. */
typedef enum {
    HOST_MOUNT_TMPFS = 0,
    HOST_MOUNT_SD    = 1,
    HOST_MOUNT_HOST  = 2,
} HostMountKind;

#define HOST_MOUNT_MAX 8

typedef struct {
    HostMountKind kind;
    char          name[16];
    /* TD: size in KB.
     * HOST: path string. */
    uint32_t      size_kb;
    char          path[128];
    bool          writable;
} HostMount;

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

static void host_config_set_defaults(HostConfig *hc) {
    hc->local_bytes   = LOCAL_BYTES;
    hc->shared_bytes  = SHARED_BYTES;
    hc->max_vms       = 4;
    hc->spawn_data_kb = 64;
    hc->raw_mode      = true;
    hc->mount_count   = 0;
}

/* Find or create a mount entry by name in hc. Returns NULL on
 * cap-exceeded. */
static HostMount *host_config_mount_get_or_create(HostConfig *hc,
                                                   const char *name) {
    for (unsigned i = 0; i < hc->mount_count; i++) {
        if (strcmp(hc->mounts[i].name, name) == 0) return &hc->mounts[i];
    }
    if (hc->mount_count >= HOST_MOUNT_MAX) return NULL;
    HostMount *m = &hc->mounts[hc->mount_count++];
    memset(m, 0, sizeof(*m));
    /* Copy name with truncation. */
    size_t n = strlen(name);
    if (n >= sizeof(m->name)) n = sizeof(m->name) - 1;
    memcpy(m->name, name, n);
    m->name[n] = '\0';
    return m;
}

/* Apply an IniCfg's settings to *hc. Unknown keys produce a
 * warning on stderr but don't fail the load — forward-compat
 * matters more than strictness for a config file the user
 * edits by hand. */
static bool apply_inicfg(const IniCfg *cfg, HostConfig *hc) {
    /* Per-key binders. Each one returns false on a value parse
     * error so we can blame the line in stderr. */
    long lv;
    bool bv;

    if (inicfg_get_int(cfg, "memory", "local_kb", &lv)) {
        if (lv < 32 || lv > (long)(LOCAL_BYTES / 1024)) {
            fprintf(stderr, "vm.cfg: [memory] local_kb=%ld out of range "
                    "(32..%zu)\n", lv, (size_t)(LOCAL_BYTES / 1024));
            return false;
        }
        hc->local_bytes = (size_t)lv * 1024;
    }
    if (inicfg_get_int(cfg, "memory", "shared_kb", &lv)) {
        if (lv < 8 || lv > (long)(SHARED_BYTES / 1024)) {
            fprintf(stderr, "vm.cfg: [memory] shared_kb=%ld out of range "
                    "(8..%zu)\n", lv, (size_t)(SHARED_BYTES / 1024));
            return false;
        }
        hc->shared_bytes = (size_t)lv * 1024;
    }
    if (inicfg_get_int(cfg, "memory", "max_vms", &lv)) {
        if (lv < 1 || lv > 16) {
            fprintf(stderr, "vm.cfg: [memory] max_vms=%ld out of range "
                    "(1..16)\n", lv);
            return false;
        }
        hc->max_vms = (uint16_t)lv;
    }
    if (inicfg_get_int(cfg, "memory", "spawn_data_kb", &lv)) {
        if (lv < 1 || lv > 256) {
            fprintf(stderr, "vm.cfg: [memory] spawn_data_kb=%ld out of "
                    "range (1..256)\n", lv);
            return false;
        }
        hc->spawn_data_kb = (uint16_t)lv;
    }
    if (inicfg_get_bool(cfg, "stdio", "raw_mode", &bv)) {
        hc->raw_mode = bv;
    }

    /* Walk all entries looking for [mount.<name>] sections. Each
     * such section defines one mount. Key bindings within the
     * section:
     *
     *   type     = host | tmpfs | sd
     *   path     = <dir>    (host only; absolute or relative)
     *   writable = bool     (host only; default false)
     *   size_kb  = <int>    (tmpfs / sd only; default 128)
     */
    for (size_t i = 0; i < cfg->count; i++) {
        const char *s = cfg->entries[i].section;
        if (strncmp(s, "mount.", 6) != 0) continue;
        const char *name = s + 6;
        if (*name == '\0') {
            fprintf(stderr, "vm.cfg: line %u: empty mount name in [mount.]\n",
                    cfg->entries[i].line);
            return false;
        }
        HostMount *m = host_config_mount_get_or_create(hc, name);
        if (!m) {
            fprintf(stderr, "vm.cfg: line %u: too many [mount.*] sections "
                    "(max %d)\n", cfg->entries[i].line, HOST_MOUNT_MAX);
            return false;
        }
        const char *k = cfg->entries[i].key;
        const char *v = cfg->entries[i].value;
        if (strcmp(k, "type") == 0) {
            if (strcmp(v, "host") == 0)       m->kind = HOST_MOUNT_HOST;
            else if (strcmp(v, "tmpfs") == 0) m->kind = HOST_MOUNT_TMPFS;
            else if (strcmp(v, "sd") == 0)    m->kind = HOST_MOUNT_SD;
            else {
                fprintf(stderr, "vm.cfg: line %u: [mount.%s] unknown type "
                        "'%s' (expected 'host', 'tmpfs', or 'sd')\n",
                        cfg->entries[i].line, name, v);
                return false;
            }
        } else if (strcmp(k, "path") == 0) {
            size_t pn = strlen(v);
            if (pn >= sizeof(m->path)) {
                fprintf(stderr, "vm.cfg: line %u: [mount.%s] path too long\n",
                        cfg->entries[i].line, name);
                return false;
            }
            memcpy(m->path, v, pn + 1);
        } else if (strcmp(k, "writable") == 0) {
            if (inicfg_get_bool(cfg, s, k, &bv)) {
                m->writable = bv;
            } else {
                fprintf(stderr, "vm.cfg: line %u: [mount.%s] writable: "
                        "unparseable bool '%s'\n",
                        cfg->entries[i].line, name, v);
                return false;
            }
        } else if (strcmp(k, "size_kb") == 0) {
            if (inicfg_get_int(cfg, s, k, &lv)) {
                if (lv < 8 || lv > 4096) {
                    fprintf(stderr, "vm.cfg: line %u: [mount.%s] "
                            "size_kb=%ld out of range (8..4096)\n",
                            cfg->entries[i].line, name, lv);
                    return false;
                }
                m->size_kb = (uint32_t)lv;
            } else {
                fprintf(stderr, "vm.cfg: line %u: [mount.%s] size_kb: "
                        "unparseable integer '%s'\n",
                        cfg->entries[i].line, name, v);
                return false;
            }
        } else {
            fprintf(stderr, "vm.cfg: line %u: warning: unknown key "
                    "'%s.%s'\n", cfg->entries[i].line, s, k);
        }
    }

    /* Warn on unknown keys so typos surface. We allow unknown
     * sections (e.g., a future [scheduler]) so older hosts
     * don't reject newer configs. [mount.<name>] sections are
     * already handled above, so skip those here. */
    static const struct {
        const char *section;
        const char *keys[8];     /* NULL-terminated */
    } known[] = {
        { "memory", {"local_kb", "shared_kb", "max_vms",
                     "spawn_data_kb", NULL} },
        { "stdio",  {"raw_mode", NULL} },
    };
    for (size_t i = 0; i < cfg->count; i++) {
        const char *s = cfg->entries[i].section;
        const char *k = cfg->entries[i].key;
        /* [mount.<name>] handled in the loop above. */
        if (strncmp(s, "mount.", 6) == 0) continue;
        bool found_section = false;
        bool found_key = false;
        for (size_t j = 0; j < sizeof(known) / sizeof(known[0]); j++) {
            if (strcmp(s, known[j].section) != 0) continue;
            found_section = true;
            for (size_t m = 0; known[j].keys[m]; m++) {
                if (strcmp(k, known[j].keys[m]) == 0) {
                    found_key = true;
                    break;
                }
            }
            break;
        }
        if (found_section && !found_key) {
            fprintf(stderr, "vm.cfg: line %u: warning: unknown key "
                    "'%s.%s'\n", cfg->entries[i].line, s, k);
        }
        /* Unknown section: silent, future-compat. */
    }
    return true;
}

/* Attempt to load vm.cfg from `path`. If `path` is NULL, tries
 * "./vm.cfg" and silently does nothing if it's not there. If
 * `path` is non-NULL, missing-or-unreadable IS an error (the
 * user explicitly asked for that file). */
static bool load_host_config(const char *path, HostConfig *hc) {
    bool explicit = (path != NULL);
    if (!path) path = "vm.cfg";

    /* If the path isn't explicit, peek to see if the default
     * config exists. Missing default is fine; we just keep
     * built-in defaults. */
    if (!explicit) {
        FILE *probe = fopen(path, "rb");
        if (!probe) return true;
        fclose(probe);
    }

    IniCfg cfg;
    IniCfgError err;
    if (!inicfg_parse_file(path, &cfg, &err)) {
        if (err.line == 0) {
            fprintf(stderr, "host: %s\n", err.msg);
        } else {
            fprintf(stderr, "host: %s: line %u: %s\n",
                    path, err.line, err.msg);
        }
        return false;
    }
    bool ok = apply_inicfg(&cfg, hc);
    inicfg_destroy(&cfg);
    return ok;
}

/* ---------------------------------------------------------------
 * Tick source: milliseconds since first call.
 *
 * The scheduler calls this on each step to refresh global_tick.
 * Guests see tick units of one millisecond, and SYS_TICK_HZ
 * returns 1000. The tick wraps at 2^32 ms ≈ 49.7 days of
 * continuous runtime.
 *
 * We anchor at the first call so global_tick starts at 0 (or
 * close to it) — easier to reason about than raw monotonic time
 * which can be a giant number. CLOCK_MONOTONIC is immune to
 * wall-clock adjustments (NTP, manual set).
 * --------------------------------------------------------------- */
static struct timespec g_t0;
static int             g_t0_set = 0;

static uint32_t monotonic_ms_ticks(void *userdata) {
    (void)userdata;
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (!g_t0_set) {
        g_t0 = t;
        g_t0_set = 1;
    }
    /* Difference in milliseconds. Borrow from seconds if nsec
     * went backwards relative to the anchor — without that, a
     * signed long subtraction (-500_000_000) cast to uint64
     * blows up to a near-2^64 value and the millisecond math
     * skews badly once per second of real time. */
    long sec_delta  = (long)(t.tv_sec  - g_t0.tv_sec);
    long nsec_delta = (long)(t.tv_nsec - g_t0.tv_nsec);
    if (nsec_delta < 0) {
        sec_delta  -= 1;
        nsec_delta += 1000000000L;
    }
    uint64_t ms = (uint64_t)sec_delta * 1000ULL
                + (uint64_t)nsec_delta / 1000000ULL;
    /* The cast to uint32 truncates to 49.7-day wraparound, which
     * is the documented intended behavior. */
    return (uint32_t)ms;
}

/* ---------------------------------------------------------------
 * Realtime (wall-clock) source for SYS_REALTIME_NOW.
 * Uses CLOCK_REALTIME — subject to NTP/manual adjustments, unlike
 * the monotonic source above. Embedded hosts without an RTC would
 * leave this NULL and SYS_REALTIME_NOW returns -ENOSYS.
 * --------------------------------------------------------------- */
static bool realtime_clock_source(void *userdata,
                                   uint32_t *seconds_out,
                                   uint32_t *nanos_out) {
    (void)userdata;
    struct timespec t;
    if (clock_gettime(CLOCK_REALTIME, &t) != 0) return false;
    /* Truncate to 32-bit Unix epoch seconds. This wraps in 2106;
     * for a microcontroller that's not really an issue in any
     * scenario I can imagine. */
    *seconds_out = (uint32_t)t.tv_sec;
    *nanos_out   = (uint32_t)t.tv_nsec;
    return true;
}

static volatile sig_atomic_t g_stop = 0;
static void on_sigint(int signo) { (void)signo; g_stop = 1; }

static int load_file(const char *path, uint8_t **out_buf, size_t *out_size) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "host: cannot open '%s'\n", path); return -1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return -1; }
    uint8_t *buf = malloc((size_t)sz);
    if (!buf) { fclose(f); return -1; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return -1; }
    *out_buf = buf; *out_size = (size_t)sz;
    return 0;
}

#ifdef PIPE_MODE_SUPPORTED
/* ---------------------------------------------------------------
 *  Named-pipe transport (Windows-only)
 *
 *  Creates a bidirectional Windows named pipe and waits for a
 *  single client to connect. The pipe is byte-oriented, in
 *  blocking mode (PIPE_WAIT — the default). PuTTY (or any other
 *  named-pipe-capable serial client) connects to the pipe and
 *  sees a normal blocking byte stream.
 *
 *  Approach: register our own SYS_READ and SYS_WRITE ECALL
 *  handlers (replacing the bridge's default ones) that talk
 *  directly to the HANDLE via Win32 APIs. This avoids two
 *  Cygwin-specific problems:
 *
 *    1. Attaching a HANDLE to a Cygwin fd via
 *       cygwin_attach_handle_to_fd works, but read() on that
 *       fd can block even after fcntl(O_NONBLOCK), because the
 *       Cygwin POSIX layer may not fully honor non-blocking
 *       semantics for every kind of attached HANDLE.
 *    2. fopencookie FILE*s have no backing fd (fileno returns
 *       -1), which the bridge doesn't tolerate.
 *
 *  By using PeekNamedPipe before each ReadFile, we get
 *  guaranteed-non-blocking reads on a pipe that remains in
 *  blocking mode for PuTTY's side (PIPE_NOWAIT would cause
 *  PuTTY to see EOF immediately and disconnect).
 *
 *  We also translate '\n' to '\r\n' on writes — there's no
 *  terminal driver in the path (OPOST/ONLCR don't apply to
 *  raw pipes), so the guest's '\n' output would otherwise
 *  appear in PuTTY as "down one line, same column" instead
 *  of "down one line, column 1." The translation produces
 *  cooked-terminal-style line endings.
 *
 *  Why this lives in host.c instead of vm_host_stdio.c:
 *    - Host-application policy (which transport) vs VM-bridge
 *      functionality (how the guest sees stdio)
 *    - Windows-specific; the bridge is portable
 *    - Future hosts (TCP socket, etc.) plug in here using the
 *      same custom-handler pattern
 * --------------------------------------------------------------- */

static HANDLE g_pipe_handle = INVALID_HANDLE_VALUE;
/* ============================================================
 *  Pipe transport (round U.2)
 *
 *  Implements the VmHostTransport vtable. Shares the named-pipe
 *  HANDLE with the legacy pipe_sys_* ecall handlers (which are
 *  no longer registered when the transport is active — see
 *  setup_pipe_transport).
 *
 *  The write function performs LF→CRLF translation, but ONLY
 *  for lone '\n' bytes (those not preceded by '\r'). This means:
 *
 *    - Shell cooked output (lone '\n' as line terminator) gets
 *      the '\r' inserted, so PuTTY shows it correctly.
 *    - TUI canvas escapes that emit '\r\n' deliberately pass
 *      through unchanged. Adding a second '\r' would corrupt
 *      cursor positioning.
 *
 *  The translation is single-pass and stateful across the call
 *  via the `prev_was_cr` static — that's correct because the
 *  transport instance is process-global today and we want
 *  cross-call coherence. When U.4/U.5 make this per-session,
 *  the state moves into transport ctx. */

static int pipe_t_read(VmHostTransport *t, void *buf, unsigned cap) {
    (void)t;
    if (g_pipe_handle == INVALID_HANDLE_VALUE) return -5;  /* -EIO */
    if (cap == 0) return 0;
    DWORD avail = 0;
    if (!PeekNamedPipe(g_pipe_handle, NULL, 0, NULL, &avail, NULL)) {
        return -5;
    }
    if (avail == 0) return 0;
    DWORD want = (DWORD)cap;
    if (want > avail) want = avail;
    DWORD got = 0;
    if (!ReadFile(g_pipe_handle, buf, want, &got, NULL)) return -5;
    return (int)got;
}

static int pipe_t_write(VmHostTransport *t, const void *buf, unsigned n) {
    (void)t;
    if (g_pipe_handle == INVALID_HANDLE_VALUE) return -5;
    static int prev_was_cr = 0;
    const char *p = (const char *)buf;
    DWORD total_in = 0;
    /* Scan through the buffer, batching runs that don't need
     * translation. When we hit a lone '\n', emit "\r\n" instead. */
    size_t run_start = 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n' && !prev_was_cr) {
            if (i > run_start) {
                DWORD wr = 0;
                if (!WriteFile(g_pipe_handle, p + run_start,
                               (DWORD)(i - run_start), &wr, NULL)) return -5;
            }
            DWORD wr = 0;
            if (!WriteFile(g_pipe_handle, "\r\n", 2, &wr, NULL)) return -5;
            total_in += 1;          /* one source byte consumed */
            run_start = i + 1;
            prev_was_cr = 0;
            continue;
        }
        prev_was_cr = (c == '\r');
    }
    if (run_start < n) {
        DWORD wr = 0;
        if (!WriteFile(g_pipe_handle, p + run_start,
                       (DWORD)(n - run_start), &wr, NULL)) return -5;
        total_in += (DWORD)(n - run_start);
    }
    return (int)total_in;
}

static int pipe_t_flush(VmHostTransport *t) {
    (void)t;
    /* WriteFile on a Windows named pipe is unbuffered at the
     * application layer — bytes go straight to the kernel pipe
     * object. Nothing for the transport to flush. */
    return 0;
}

static int pipe_t_set_raw(VmHostTransport *t, bool enable) {
    (void)t; (void)enable;
    /* Pipes don't have a line discipline; raw mode is implicit
     * (no terminal driver intervenes between us and PuTTY).
     * Always report success. */
    return 0;
}

static VmHostTransport g_pipe_transport = {
    .read_nonblock = pipe_t_read,
    .write         = pipe_t_write,
    .flush         = pipe_t_flush,
    .set_raw       = pipe_t_set_raw,
    .close         = NULL,
    .is_terminal   = true,
    .ctx           = NULL,
};

/* Create the named pipe, wait for the client to connect, and
 * register our SYS_READ/SYS_WRITE/SYS_FFLUSH handlers on the
 * given VmSystem.
 *
 * Returns true on success. On failure, prints a diagnostic and
 * returns false; the caller should exit. */
static bool setup_pipe_transport(VmSystem *sys, const char *name) {
    /* Pipe naming: callers can pass either a fully-qualified
     * "\\\\.\\pipe\\foo" or a short "foo". Translate short forms
     * to the full prefix to make the CLI friendlier. */
    char full[256];
    if (strncmp(name, "\\\\.\\pipe\\", 9) == 0) {
        snprintf(full, sizeof(full), "%s", name);
    } else {
        snprintf(full, sizeof(full), "\\\\.\\pipe\\%s", name);
    }

    /* PIPE_ACCESS_DUPLEX     — bidirectional
     * PIPE_TYPE_BYTE         — stream-oriented, not message-oriented
     * PIPE_WAIT              — default blocking semantics; PuTTY
     *                          and other normal clients expect this
     * 1 instance, 4 KB buffers, default timeout. */
    g_pipe_handle = CreateNamedPipeA(
        full,
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,                     /* max instances */
        4096, 4096,            /* out, in buffer sizes */
        0,                     /* default timeout */
        NULL);                 /* default security */
    if (g_pipe_handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "host: CreateNamedPipe('%s') failed (error %lu)\n",
                full, (unsigned long)GetLastError());
        return false;
    }

    fprintf(stderr, "host: waiting for client on %s ...\n", full);
    fprintf(stderr, "host: in PuTTY: Session type=Serial, "
                    "Serial line=%s, Speed=any\n", full);
    fflush(stderr);

    /* Blocks until a client connects. ERROR_PIPE_CONNECTED means
     * the client connected between CreateNamedPipe and here,
     * which is fine. */
    if (!ConnectNamedPipe(g_pipe_handle, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_PIPE_CONNECTED) {
            fprintf(stderr, "host: ConnectNamedPipe failed (error %lu)\n",
                    (unsigned long)err);
            CloseHandle(g_pipe_handle);
            g_pipe_handle = INVALID_HANDLE_VALUE;
            return false;
        }
    }
    fprintf(stderr, "host: client connected.\n");
    fflush(stderr);

    /* Round U.2: register the stdio handlers (SYS_READ/WRITE/FFLUSH)
     * and then point the active transport at our pipe vtable. The
     * stdio handlers consult the transport for every byte they
     * move, so this single set_transport call diverts everything —
     * shell prompt, guest puts/printf, AND TUI canvas escapes —
     * to the pipe. No more ecall-registration overrides; no more
     * separate TUI output hook. */
    VmHostStdioConfig sio = {0};
    sio.raw_mode = false;   /* the pipe transport's set_raw is a no-op */
    if (!vm_host_install_stdio_ex(sys, &sio)) {
        fprintf(stderr, "host: vm_host_install_stdio_ex failed\n");
        return false;
    }
    vm_host_set_transport(&g_pipe_transport);

    return true;
}
#endif  /* PIPE_MODE_SUPPORTED */

#ifdef PTY_MODE_SUPPORTED
/* ============================================================
 *  PTY transport (round U.3)
 *
 *  Allocates a POSIX pseudoterminal pair. The master fd lives
 *  in this process; the slave path (typically /dev/pts/N on
 *  Linux) is printed to stderr so the user can connect a
 *  terminal emulator to it:
 *
 *      $ ./host --pty
 *      host: pty created at /dev/pts/7
 *      host: connect with: screen /dev/pts/7
 *      host: ready
 *
 *  Then in another window:
 *
 *      $ screen /dev/pts/7
 *
 *  The shell prompt appears in screen; TUI demos appear there
 *  too because the active transport routes every byte through
 *  the master fd.
 *
 *  Compared to the pipe transport:
 *    - Cross-platform: works on Linux, BSD, macOS, and Cygwin.
 *      Does NOT work on native Windows (no posix_openpt).
 *    - Has a real line discipline. set_raw() actually does
 *      something — it flips the slave's termios into cbreak.
 *    - Same LF→CRLF policy on writes: lone '\\n' gets the '\\r',
 *      explicit '\\r\\n' passes through.
 *    - read_nonblock uses O_NONBLOCK and reports EAGAIN as 0.
 *    - When no slave is connected yet, reads return 0
 *      ("no data"). Once the user attaches their terminal,
 *      bytes start flowing. When the slave disconnects, reads
 *      eventually return 0 or EIO; the host process keeps
 *      running (U.6 will add proper session lifecycle).
 * ============================================================ */

static int g_pty_master_fd = -1;

static int pty_t_read(VmHostTransport *t, void *buf, unsigned cap) {
    (void)t;
    if (g_pty_master_fd < 0) return -5;       /* -EIO */
    if (cap == 0) return 0;
    ssize_t r = read(g_pty_master_fd, buf, cap);
    if (r > 0) return (int)r;
    if (r == 0) {
        /* On Linux, read() == 0 on a pty master can mean
         * "slave not yet open" OR "slave has closed." In
         * non-blocking mode with no slave attached, some kernels
         * report this as EAGAIN instead. Either way we report
         * "no data" — distinguishing transient-not-connected
         * from real-disconnect requires the U.6 lifecycle work. */
        return 0;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    if (errno == EIO)    return 0;   /* common 'no slave attached' code */
    return -5;
}

static int pty_t_write(VmHostTransport *t, const void *buf, unsigned n) {
    (void)t;
    if (g_pty_master_fd < 0) return -5;
    /* Same LF→CRLF policy as the pipe transport. */
    static int prev_was_cr = 0;
    const char *p = (const char *)buf;
    unsigned total_in = 0;
    size_t run_start = 0;
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '\n' && !prev_was_cr) {
            if (i > run_start) {
                ssize_t w = write(g_pty_master_fd, p + run_start,
                                  i - run_start);
                if (w < 0 && errno != EAGAIN) return -5;
            }
            ssize_t w = write(g_pty_master_fd, "\r\n", 2);
            if (w < 0 && errno != EAGAIN) return -5;
            total_in += 1;
            run_start = i + 1;
            prev_was_cr = 0;
            continue;
        }
        prev_was_cr = (c == '\r');
    }
    if (run_start < n) {
        ssize_t w = write(g_pty_master_fd, p + run_start,
                          n - run_start);
        if (w < 0 && errno != EAGAIN) return -5;
        total_in += (unsigned)(n - run_start);
    }
    return (int)total_in;
}

static int pty_t_flush(VmHostTransport *t) {
    (void)t;
    /* POSIX write to a pty master is unbuffered at the
     * application layer. The kernel may buffer in the pty
     * driver but tcdrain() would block — not what we want.
     * No-op. */
    return 0;
}

static int pty_t_set_raw(VmHostTransport *t, bool enable) {
    (void)t;
    if (g_pty_master_fd < 0) return -5;
    /* Manipulate the slave's line discipline via the master fd.
     * POSIX defines tcgetattr/tcsetattr on a pty master as
     * affecting the slave-side termios. */
    struct termios ts;
    if (tcgetattr(g_pty_master_fd, &ts) != 0) return -5;
    if (enable) {
        cfmakeraw(&ts);
    } else {
        /* Restore cooked-ish defaults. We don't preserve the
         * EXACT termios across toggles (that would need a
         * saved copy); approximate cooked is good enough for
         * a transport that's typically used in raw mode. */
        ts.c_iflag |= (ICRNL | BRKINT);
        ts.c_oflag |= (OPOST | ONLCR);
        ts.c_lflag |= (ICANON | ECHO | ISIG);
    }
    if (tcsetattr(g_pty_master_fd, TCSANOW, &ts) != 0) return -5;
    return 0;
}

static void pty_t_close(VmHostTransport *t) {
    (void)t;
    if (g_pty_master_fd >= 0) {
        close(g_pty_master_fd);
        g_pty_master_fd = -1;
    }
}

static VmHostTransport g_pty_transport = {
    .read_nonblock = pty_t_read,
    .write         = pty_t_write,
    .flush         = pty_t_flush,
    .set_raw       = pty_t_set_raw,
    .close         = pty_t_close,
    .is_terminal   = true,
    .ctx           = NULL,
};

/* Create the pty pair, print the slave path, install stdio +
 * transport. The slave is NOT held open here — the user
 * connects whenever they want. */
static bool setup_pty_transport(VmSystem *sys) {
    int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        fprintf(stderr, "host: posix_openpt failed: %s\n", strerror(errno));
        return false;
    }
    if (grantpt(master) != 0) {
        fprintf(stderr, "host: grantpt failed: %s\n", strerror(errno));
        close(master);
        return false;
    }
    if (unlockpt(master) != 0) {
        fprintf(stderr, "host: unlockpt failed: %s\n", strerror(errno));
        close(master);
        return false;
    }
    const char *slave_path = ptsname(master);
    if (!slave_path) {
        fprintf(stderr, "host: ptsname failed: %s\n", strerror(errno));
        close(master);
        return false;
    }

    /* Non-blocking mode on the master so reads return 0
     * instead of blocking when no slave is connected. */
    int flags = fcntl(master, F_GETFL, 0);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "host: fcntl O_NONBLOCK on pty master failed: %s\n",
                strerror(errno));
        close(master);
        return false;
    }

    /* Put the slave's line discipline in raw mode by default —
     * that's what TUI demos want. The shell handles its own
     * line editing; cooked mode would echo and buffer in ways
     * the shell doesn't expect. */
    struct termios ts;
    if (tcgetattr(master, &ts) == 0) {
        cfmakeraw(&ts);
        tcsetattr(master, TCSANOW, &ts);
    }

    g_pty_master_fd = master;

    fprintf(stderr, "host: pty created at %s\n", slave_path);
    fprintf(stderr, "host: connect with: screen %s\n", slave_path);
    fprintf(stderr, "host:          or:  minicom -D %s\n", slave_path);
    fprintf(stderr, "host: ready\n");
    fflush(stderr);

    VmHostStdioConfig sio = {0};
    sio.raw_mode = false;   /* pty transport's set_raw owns this */
    if (!vm_host_install_stdio_ex(sys, &sio)) {
        fprintf(stderr, "host: vm_host_install_stdio_ex failed\n");
        return false;
    }
    vm_host_set_transport(&g_pty_transport);

    return true;
}
#endif  /* PTY_MODE_SUPPORTED */

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
     * Options:
     *   --config=<path>     Load config from <path>. Default is
     *                       ./vm.cfg if present (silently skipped
     *                       if missing). Use this to point at a
     *                       different config file.
     *   --no-config         Skip even a present ./vm.cfg. Useful
     *                       for testing CLI-only behavior.
     *   --local-kb=<N>      Local-slab size in KB (overrides config).
     *   --shared-kb=<N>     Shared-slab size in KB (overrides config).
     *   --max-vms=<N>       Max concurrent VMs (overrides config).
     *   --spawn-data-kb=<N> Per-spawn data region size in KB.
     *   --raw=on|off        Toggle raw-mode stdin.
     *   --host-fs=<path>    Mount <path> as /host inside the shell.
     *                       The guest can then read/run files via
     *                       /host/<name>. Default: ./host_files
     *   --host-fs-rw        Make the /host mount writable. Default
     *                       is read-only for safety.
     *   --no-host-fs        Disable the /host mount entirely.
     *   --pipe=<name>       (Windows/Cygwin) Route stdio through a
     *                       named pipe; PuTTY connects to it as a
     *                       Serial session. Name can be a bare
     *                       identifier ('microgarbage') or a full
     *                       \\.\\pipe\\<name> path.
     *
     * Layering: built-in defaults < config file < CLI.
     *
     * Positional: the path to shell.elf. Defaults to build/shell.elf.
     */
    const char *elf_path     = NULL;
    const char *host_fs_root = "host_files";   /* default — created if missing */
    bool host_fs_writable    = false;
    bool host_fs_disabled    = false;
    const char *pipe_name    = NULL;
    bool        want_pty     = false;
    const char *cfg_path     = NULL;
    bool        no_config    = false;

    /* CLI overrides for HostConfig fields. These are "unset" until
     * the user passes the flag, so they only fire after we've
     * loaded the config file (which gets the chance to set them
     * first). Sentinel values: -1 for ints, -1 for tri-state bool. */
    long cli_local_kb       = -1;
    long cli_shared_kb      = -1;
    long cli_max_vms        = -1;
    long cli_spawn_data_kb  = -1;
    int  cli_raw_mode       = -1;   /* 0=off, 1=on, -1=unset */

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--config=", 9) == 0) {
            cfg_path = argv[i] + 9;
        } else if (strcmp(argv[i], "--no-config") == 0) {
            no_config = true;
        } else if (strncmp(argv[i], "--local-kb=", 11) == 0) {
            cli_local_kb = strtol(argv[i] + 11, NULL, 10);
        } else if (strncmp(argv[i], "--shared-kb=", 12) == 0) {
            cli_shared_kb = strtol(argv[i] + 12, NULL, 10);
        } else if (strncmp(argv[i], "--max-vms=", 10) == 0) {
            cli_max_vms = strtol(argv[i] + 10, NULL, 10);
        } else if (strncmp(argv[i], "--spawn-data-kb=", 16) == 0) {
            cli_spawn_data_kb = strtol(argv[i] + 16, NULL, 10);
        } else if (strncmp(argv[i], "--raw=", 6) == 0) {
            const char *v = argv[i] + 6;
            if (strcmp(v, "on") == 0 || strcmp(v, "true") == 0 ||
                strcmp(v, "yes") == 0 || strcmp(v, "1") == 0) {
                cli_raw_mode = 1;
            } else if (strcmp(v, "off") == 0 || strcmp(v, "false") == 0 ||
                       strcmp(v, "no") == 0 || strcmp(v, "0") == 0) {
                cli_raw_mode = 0;
            } else {
                fprintf(stderr, "host: --raw expects on|off (got '%s')\n", v);
                return 1;
            }
        } else if (strncmp(argv[i], "--host-fs=", 10) == 0) {
            host_fs_root = argv[i] + 10;
        } else if (strcmp(argv[i], "--host-fs-rw") == 0) {
            host_fs_writable = true;
        } else if (strcmp(argv[i], "--no-host-fs") == 0) {
            host_fs_disabled = true;
        } else if (strncmp(argv[i], "--pipe=", 7) == 0) {
            pipe_name = argv[i] + 7;
        } else if (strcmp(argv[i], "--pty") == 0) {
            want_pty = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "host: unknown option '%s'\n", argv[i]);
            fprintf(stderr, "  --config=<path>     load config from <path>\n");
            fprintf(stderr, "  --no-config         skip ./vm.cfg even if present\n");
            fprintf(stderr, "  --local-kb=<N>      local-slab size in KB\n");
            fprintf(stderr, "  --shared-kb=<N>     shared-slab size in KB\n");
            fprintf(stderr, "  --max-vms=<N>       max concurrent VMs\n");
            fprintf(stderr, "  --spawn-data-kb=<N> per-spawn data region in KB\n");
            fprintf(stderr, "  --raw=on|off        toggle raw-mode stdin\n");
            fprintf(stderr, "  --host-fs=<path>    mount path as /host (default: ./host_files)\n");
            fprintf(stderr, "  --host-fs-rw        allow writes to /host (default: read-only)\n");
            fprintf(stderr, "  --no-host-fs        disable /host mount\n");
            fprintf(stderr, "  --pipe=<name>       route stdio through a named pipe (Windows)\n");
            fprintf(stderr, "  --pty               route stdio through a POSIX pty (Linux/Cygwin)\n");
            return 1;
        } else if (!elf_path) {
            elf_path = argv[i];
        } else {
            fprintf(stderr, "host: extra positional argument '%s'\n", argv[i]);
            return 1;
        }
    }
    if (!elf_path) elf_path = "build/shell.elf";

    /* ----- Load HostConfig: defaults -> vm.cfg -> CLI overrides -----
     *
     * If --no-config was passed, we skip even the implicit default
     * file. If --config=<path> was passed, missing file is an error
     * (user asked for that file explicitly). Without --config, a
     * missing ./vm.cfg is fine. */
    HostConfig hc;
    host_config_set_defaults(&hc);
    if (!no_config) {
        if (!load_host_config(cfg_path, &hc)) return 1;
    } else if (cfg_path) {
        fprintf(stderr, "host: --no-config and --config are mutually exclusive\n");
        return 1;
    }
    /* CLI overrides last. Each cli_* is range-checked here so
     * an out-of-range value still fails cleanly (instead of being
     * silently clamped or wrapping). */
    if (cli_local_kb >= 0) {
        if (cli_local_kb < 32 || cli_local_kb > (long)(LOCAL_BYTES / 1024)) {
            fprintf(stderr, "host: --local-kb=%ld out of range (32..%zu)\n",
                    cli_local_kb, (size_t)(LOCAL_BYTES / 1024));
            return 1;
        }
        hc.local_bytes = (size_t)cli_local_kb * 1024;
    }
    if (cli_shared_kb >= 0) {
        if (cli_shared_kb < 8 || cli_shared_kb > (long)(SHARED_BYTES / 1024)) {
            fprintf(stderr, "host: --shared-kb=%ld out of range (8..%zu)\n",
                    cli_shared_kb, (size_t)(SHARED_BYTES / 1024));
            return 1;
        }
        hc.shared_bytes = (size_t)cli_shared_kb * 1024;
    }
    if (cli_max_vms >= 0) {
        if (cli_max_vms < 1 || cli_max_vms > 16) {
            fprintf(stderr, "host: --max-vms=%ld out of range (1..16)\n",
                    cli_max_vms);
            return 1;
        }
        hc.max_vms = (uint16_t)cli_max_vms;
    }
    if (cli_spawn_data_kb >= 0) {
        if (cli_spawn_data_kb < 1 || cli_spawn_data_kb > 256) {
            fprintf(stderr, "host: --spawn-data-kb=%ld out of range (1..256)\n",
                    cli_spawn_data_kb);
            return 1;
        }
        hc.spawn_data_kb = (uint16_t)cli_spawn_data_kb;
    }
    if (cli_raw_mode != -1) hc.raw_mode = (cli_raw_mode != 0);

#ifndef PIPE_MODE_SUPPORTED
    if (pipe_name) {
        fprintf(stderr, "host: --pipe is Windows-only "
                        "(this build targets a non-Windows platform).\n");
        return 1;
    }
#endif
#ifndef PTY_MODE_SUPPORTED
    if (want_pty) {
        fprintf(stderr, "host: --pty is not supported on this platform "
                        "(needs POSIX posix_openpt/grantpt).\n");
        return 1;
    }
#endif
    if (pipe_name && want_pty) {
        fprintf(stderr, "host: --pipe and --pty are mutually exclusive\n");
        return 1;
    }

#ifdef _WIN32
    /* mingw doesn't have struct sigaction. Use the simpler
     * signal() ANSI API; SIGINT is what we care about (Ctrl-C). */
    signal(SIGINT, on_sigint);
#else
    struct sigaction sa = {0};
    sa.sa_handler = on_sigint;
    sigaction(SIGINT, &sa, NULL);
#endif

    /* 1. Initialize the block device. */
    if (trash_init(&g_drive, g_pool, sizeof(g_pool)) != TRASH_OK) {
        fprintf(stderr, "host: trash_init failed\n");
        return 1;
    }

    /* 2. Register with FatFs as drive 0. */
    if (!trash_fatfs_register(0, &g_drive)) {
        fprintf(stderr, "host: trash_fatfs_register failed\n");
        return 1;
    }

    /* 3. Format the volume (always — trashdrive is RAM so we
     * start fresh each run). For persistence between runs you'd
     * skip f_mkfs and just f_mount; FatFs auto-detects a
     * pre-formatted volume. */
    BYTE work[FF_MAX_SS];
    MKFS_PARM opt = {0};
    opt.fmt = FM_FAT;
    opt.n_fat = 1;
    FRESULT fr = f_mkfs("0:", &opt, work, sizeof(work));
    if (fr != FR_OK) {
        fprintf(stderr, "host: f_mkfs failed: %d\n", fr);
        return 1;
    }

    /* 4. Mount the volume. */
    fr = f_mount(&g_fs, "0:", 1);
    if (fr != FR_OK) {
        fprintf(stderr, "host: f_mount failed: %d\n", fr);
        return 1;
    }

    /* Pre-create a few items in the volume so `ls` has something
     * to show on first launch. Pure convenience — remove if you
     * want a truly empty start. */
    f_mkdir("0:/home");
    f_mkdir("0:/tmp");
    {
        FIL f;
        UINT bw;
        if (f_open(&f, "0:/readme.txt", FA_WRITE | FA_CREATE_ALWAYS) == FR_OK) {
            const char *msg =
                "Welcome to the VM shell.\n"
                "Try: ls, cd home, mkdir foo, touch bar.txt, cat readme.txt\n"
                "\n"
                "Filesystem layout:\n"
                "  /td0/    this RAM-backed FatFs volume (default cwd)\n"
                "  /host/   host directory passthrough (read-only)\n"
                "\n"
                "Absolute paths must start with /<name>/. Relative\n"
                "paths are resolved against the current directory.\n";
            f_write(&f, msg, (UINT)strlen(msg), &bw);
            f_close(&f);
        }
    }

    /* 5. Load the guest ELF. */
    uint8_t *elf = NULL;
    size_t elf_size = 0;
    if (load_file(elf_path, &elf, &elf_size) != 0) {
        return 1;
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
        .tick_source         = monotonic_ms_ticks,
        .ticks_per_second    = 1000,
    };
    if (!vm_system_init(&sys, &cfg)) {
        fprintf(stderr, "host: vm_system_init failed "
                "(local=%zu shared=%zu max_vms=%u spawn_data_kb=%u — "
                "try smaller values in vm.cfg or via --max-vms / "
                "--spawn-data-kb)\n",
                hc.local_bytes, hc.shared_bytes,
                (unsigned)hc.max_vms, (unsigned)hc.spawn_data_kb);
        return 1;
    }

    /* Stdio install: either default (process stdin/stdout/stderr)
     * via the portable bridge, or routed through a named pipe with
     * Windows-direct handlers. The pipe call BLOCKS until a client
     * connects, so the user sees the "waiting" message first and
     * the shell banner once PuTTY is attached. */
    if (pipe_name) {
#ifdef PIPE_MODE_SUPPORTED
        if (!setup_pipe_transport(&sys, pipe_name)) {
            return 1;
        }
#else
        /* unreachable — we checked above */
        return 1;
#endif
    } else if (want_pty) {
#ifdef PTY_MODE_SUPPORTED
        if (!setup_pty_transport(&sys)) {
            return 1;
        }
#else
        /* unreachable — we checked above */
        return 1;
#endif
    } else {
        VmHostStdioConfig sio = {0};
        sio.raw_mode = hc.raw_mode;
        if (!vm_host_install_stdio_ex(&sys, &sio)) {
            fprintf(stderr, "host: vm_host_install_stdio failed\n");
            return 1;
        }
    }
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
        pcfg.realtime_source   = realtime_clock_source;
        pcfg.realtime_userdata = NULL;
        struct timespec t;
        if (clock_gettime(CLOCK_REALTIME, &t) == 0) {
            /* Mix sec and nsec into the seed so two runs in the
             * same second still differ. SplitMix64 will diffuse. */
            pcfg.rand_seed = ((uint64_t)t.tv_sec << 32) ^
                             (uint64_t)t.tv_nsec ^
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

    /* 6b. Mounts.
     *
     * If vm.cfg's [mount.<name>] sections were used, hc.mount_count
     * is non-zero and those become the mounts. Otherwise we set up
     * the built-in defaults: /td0 (the RAM-backed FatFs)
     * and /host (a passthrough to host_fs_root, unless
     * --no-host-fs was passed).
     *
     * The shell defaults its cwd to /td0. If a custom config
     * doesn't include a td0, the shell's first `pwd` will show a
     * non-resolvable cwd — but that's the user's choice.
     *
     * Multiple TD mounts aren't supported in M.3a: there's only one
     * static FatFs volume backing. A configured td<N> reuses it,
     * but the size_kb override is ignored (the backing pool size
     * is compile-time). M.3b adds image-file backends and proper
     * per-mount backing pools. */

    if (hc.mount_count == 0) {
        /* No mount section in vm.cfg — use built-in defaults. */
        if (!vm_host_fs_mount_fatfs("td0", 0, &g_fs)) {
            fprintf(stderr, "host: vm_host_fs_mount_fatfs('td0') failed\n");
            return 1;
        }

        if (!host_fs_disabled) {
            struct stat st;
            if (stat(host_fs_root, &st) != 0) {
                if (host_mkdir(host_fs_root, 0755) != 0) {
                    fprintf(stderr, "host: warning — could not create '%s' "
                            "for /host mount: %s\n",
                            host_fs_root, strerror(errno));
                    fprintf(stderr, "host: /host will be disabled\n");
                    host_fs_disabled = true;
                }
            }
            if (!host_fs_disabled) {
                if (!vm_host_fs_mount_host("host", host_fs_root,
                                           host_fs_writable)) {
                    fprintf(stderr, "host: warning — "
                            "vm_host_fs_mount_host('%s') failed\n",
                            host_fs_root);
                    fprintf(stderr, "host: /host will be disabled\n");
                } else {
                    fprintf(stderr, "host: /host mounted from '%s' "
                            "(%s)\n",
                            host_fs_root,
                            host_fs_writable ? "read/write" : "read-only");
                }
            }
        }
    } else {
        /* Config-driven mount setup. */
        bool any_fatfs_mounted = false;
        for (unsigned i = 0; i < hc.mount_count; i++) {
            const HostMount *m = &hc.mounts[i];
            if (m->kind == HOST_MOUNT_TMPFS || m->kind == HOST_MOUNT_SD) {
                /* tmpfs and sd both map to FatFs over the single
                 * host-side trashdrive pool today. On hardware
                 * they'll diverge (tmpfs stays in RAM; sd uses
                 * the SD card driver). For now: just enforce one
                 * FatFs mount until we add multi-volume support. */
                if (any_fatfs_mounted) {
                    fprintf(stderr, "host: vm.cfg: multiple FatFs mounts "
                            "(tmpfs/sd) not supported in M.3a (ignoring "
                            "mount.%s)\n", m->name);
                    continue;
                }
                if (!vm_host_fs_mount_fatfs(m->name, 0, &g_fs)) {
                    fprintf(stderr, "host: vm_host_fs_mount_fatfs('%s') "
                            "failed\n", m->name);
                    return 1;
                }
                const char *kind_str =
                    (m->kind == HOST_MOUNT_TMPFS) ? "tmpfs" : "sd";
                fprintf(stderr, "host: /%s mounted (%s via FatFs, %u KB pool"
                        "%s)\n", m->name, kind_str,
                        (unsigned)(POOL_BYTES / 1024),
                        m->size_kb ? "; size_kb override ignored" : "");
                any_fatfs_mounted = true;
            } else {
                /* HOST. Path is required. */
                if (m->path[0] == '\0') {
                    fprintf(stderr, "host: vm.cfg: [mount.%s] type=host "
                            "needs a 'path' setting\n", m->name);
                    return 1;
                }
                struct stat st;
                if (stat(m->path, &st) != 0) {
                    if (host_mkdir(m->path, 0755) != 0) {
                        fprintf(stderr, "host: vm.cfg: [mount.%s] "
                                "cannot create '%s': %s\n",
                                m->name, m->path, strerror(errno));
                        return 1;
                    }
                }
                if (!vm_host_fs_mount_host(m->name, m->path, m->writable)) {
                    fprintf(stderr, "host: vm.cfg: [mount.%s] "
                            "vm_host_fs_mount_host('%s') failed\n",
                            m->name, m->path);
                    return 1;
                }
                fprintf(stderr, "host: /%s mounted from '%s' (%s)\n",
                        m->name, m->path,
                        m->writable ? "read/write" : "read-only");
            }
        }
    }

    /* 7. Load the shell. */
    VmLoadVmResult lr = vm_system_load_vm(&sys, elf, elf_size, 16 * 1024,
                                          VM_BACKING_COPY_RAM,
                                          VM_BACKING_COPY_RAM);
    if (lr.code != VM_SYS_OK) {
        fprintf(stderr, "host: load failed (code=%d)\n", lr.code);
        return 1;
    }

    /* 8. Run. The shell never exits on its own unless the user
     * types 'exit' (or the host terminates). We use the stepping
     * loop rather than vm_system_run so SIGINT can break us out
     * cleanly. */
    for (;;) {
        if (g_stop) {
            fprintf(stderr, "\nhost: SIGINT received, stopping.\n");
            break;
        }
        VmSchedStepResult r = vm_system_step(&sys);
        if (r == VM_SCHED_ALL_HALTED) {
            break;
        }
        /* VM_SCHED_IDLE means all VMs are blocked. The shell
         * blocks on SYS_READ when the user isn't typing — but
         * our SYS_READ is non-blocking (returns 0), so the shell
         * actually loops with SYS_YIELDs. We won't see IDLE in
         * practice. */
    }

    vm_system_destroy(&sys);
    f_mount(NULL, "0:", 0);
    free(elf);
    return 0;
}
