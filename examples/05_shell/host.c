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

#include "vm/vm_system.h"
#include "vm/vm_host_stdio.h"
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

#include "vm/host_compat.h"

#if defined(__CYGWIN__) || defined(_WIN32)
#  define PIPE_MODE_SUPPORTED 1
#  include <windows.h>
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

/* Mount entry parsed from [mount.<name>] sections. */
typedef enum {
    HOST_MOUNT_TD   = 0,
    HOST_MOUNT_HOST = 1,
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
     *   type     = host | td
     *   path     = <dir>    (host only; absolute or relative)
     *   writable = bool     (host only; default false)
     *   size_kb  = <int>    (td only; default 128)
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
            if (strcmp(v, "host") == 0)      m->kind = HOST_MOUNT_HOST;
            else if (strcmp(v, "td") == 0)   m->kind = HOST_MOUNT_TD;
            else {
                fprintf(stderr, "vm.cfg: line %u: [mount.%s] unknown type "
                        "'%s' (expected 'host' or 'td')\n",
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

/* Syscall numbers — duplicated from vm_ecall.h for clarity in
 * this contained section. */
#define HOST_SYS_READ    63
#define HOST_SYS_WRITE   64
#define HOST_SYS_FFLUSH  82

static HANDLE g_pipe_handle = INVALID_HANDLE_VALUE;

/* === Pipe SYS_READ handler ===
 *
 * Non-blocking read on the HANDLE via PeekNamedPipe + ReadFile.
 * Returns:
 *   > 0  number of bytes read
 *   = 0  no data available right now (guest polls again later)
 *   < 0  -EIO on hard error
 *
 * The 'no data' case is what makes interactive guests work —
 * they spin a read+yield loop, and as long as we never block
 * inside the handler, the spawn pump can step the snake game's
 * frame loop normally. */
static void pipe_sys_read(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;

    uint32_t fd = cpu->regs[VM_REG_A0];
    uint32_t guest_p = cpu->regs[VM_REG_A1];
    uint32_t n = cpu->regs[VM_REG_A2];

    /* File-fd path: any fd >= 3 belongs to vm_host_fs (FatFs or
     * the /host passthrough). Translate the guest pointer here
     * since vm_host_fs's routing function takes a host pointer. */
    if (fd >= 3) {
        if (n == 0) {
            cpu->regs[VM_REG_A0] = 0;
            return;
        }
        void *host_buf = vm_translate_write(cpu, guest_p, n);
        if (!host_buf) {
            cpu->regs[VM_REG_A0] = (uint32_t)-14;  /* -EFAULT */
            return;
        }
        int32_t r = vm_host_fs_route_read((int)fd, host_buf, n);
        cpu->regs[VM_REG_A0] = (uint32_t)r;
        return;
    }

    if (fd != 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-9;  /* -EBADF */
        return;
    }
    if (n == 0) {
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    /* PeekNamedPipe tells us how many bytes are queued without
     * removing them. Zero queued = report 'no data' (read returns
     * 0) — guest will yield and retry. Nonzero = ReadFile up to
     * min(want, available), guaranteed not to block. */
    DWORD avail = 0;
    if (!PeekNamedPipe(g_pipe_handle, NULL, 0, NULL, &avail, NULL)) {
        cpu->regs[VM_REG_A0] = (uint32_t)-5;  /* -EIO */
        return;
    }
    if (avail == 0) {
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    /* Bytes ARE available — translate the guest pointer and
     * read directly into the guest's address space. */
    void *host_buf = vm_translate_write(cpu, guest_p, n);
    if (!host_buf) {
        cpu->regs[VM_REG_A0] = (uint32_t)-14;  /* -EFAULT */
        return;
    }

    DWORD want = (DWORD)n;
    if (want > avail) want = avail;
    DWORD got = 0;
    if (!ReadFile(g_pipe_handle, host_buf, want, &got, NULL)) {
        cpu->regs[VM_REG_A0] = (uint32_t)-5;
        return;
    }
    cpu->regs[VM_REG_A0] = (uint32_t)got;
}

/* === Pipe SYS_WRITE handler ===
 *
 * Writes to the HANDLE via WriteFile, translating '\n' to '\r\n'
 * so output appears correctly in PuTTY (which has no terminal
 * driver to do that translation for us).
 *
 * The translation uses a small stack-buffered batching strategy:
 * scan the input for '\n', flush the run before it, emit
 * '\r\n', then continue. Worst case is one ReadFile per byte
 * of '\n'-heavy output, which is fine. */
static void pipe_sys_write(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;

    uint32_t fd = cpu->regs[VM_REG_A0];
    uint32_t guest_p = cpu->regs[VM_REG_A1];
    uint32_t n = cpu->regs[VM_REG_A2];

    /* File-fd path: any fd >= 3 belongs to vm_host_fs. We DON'T
     * do the \n -> \r\n translation here — file content is
     * supposed to be byte-exact. The translation only applies
     * to the pipe transport's TTY emulation for stdout/stderr. */
    if (fd >= 3) {
        if (n == 0) {
            cpu->regs[VM_REG_A0] = 0;
            return;
        }
        const void *host_buf = vm_translate_read(cpu, guest_p, n);
        if (!host_buf) {
            cpu->regs[VM_REG_A0] = (uint32_t)-14;
            return;
        }
        int32_t r = vm_host_fs_route_write((int)fd, host_buf, n);
        cpu->regs[VM_REG_A0] = (uint32_t)r;
        return;
    }

    if (fd != 1 && fd != 2) {
        cpu->regs[VM_REG_A0] = (uint32_t)-9;  /* -EBADF */
        return;
    }
    if (n == 0) {
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    const void *vbuf = vm_translate_read(cpu, guest_p, n);
    if (!vbuf) {
        cpu->regs[VM_REG_A0] = (uint32_t)-14;
        return;
    }
    const char *buf = (const char *)vbuf;

    /* Scan for '\n's. Write the run-up-to-each, then emit '\r\n'. */
    DWORD written_total = 0;
    size_t run_start = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] != '\n') continue;

        /* Flush any pending run [run_start .. i) — bytes before
         * this newline. */
        if (i > run_start) {
            DWORD wr = 0;
            if (!WriteFile(g_pipe_handle, buf + run_start,
                           (DWORD)(i - run_start), &wr, NULL)) {
                cpu->regs[VM_REG_A0] = (uint32_t)-5;
                return;
            }
            written_total += wr;
        }
        /* Emit "\r\n" for the newline. */
        DWORD wr = 0;
        if (!WriteFile(g_pipe_handle, "\r\n", 2, &wr, NULL)) {
            cpu->regs[VM_REG_A0] = (uint32_t)-5;
            return;
        }
        /* Count one byte (the guest only wrote one '\n', which
         * we expanded to two — the guest's accounting tracks
         * its own bytes). */
        written_total += 1;
        run_start = i + 1;
    }
    /* Flush the trailing run after the last newline. */
    if (run_start < n) {
        DWORD wr = 0;
        if (!WriteFile(g_pipe_handle, buf + run_start,
                       (DWORD)(n - run_start), &wr, NULL)) {
            cpu->regs[VM_REG_A0] = (uint32_t)-5;
            return;
        }
        written_total += wr;
    }
    cpu->regs[VM_REG_A0] = written_total;
}

/* === Pipe SYS_FFLUSH handler ===
 *
 * No-op: WriteFile on a Windows named pipe doesn't buffer
 * (the bytes go straight to the kernel pipe object), so
 * there's nothing for fflush to do. */
static void pipe_sys_fflush(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;
    cpu->regs[VM_REG_A0] = 0;
}

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

    /* Register our pipe-aware handlers. They REPLACE whatever
     * was installed by vm_host_install_stdio (which in pipe mode
     * shouldn't have been called). */
    if (!vm_ecall_register(sys->ecall_router, HOST_SYS_READ, pipe_sys_read)) {
        fprintf(stderr, "host: register SYS_READ failed\n");
        return false;
    }
    if (!vm_ecall_register(sys->ecall_router, HOST_SYS_WRITE, pipe_sys_write)) {
        fprintf(stderr, "host: register SYS_WRITE failed\n");
        return false;
    }
    if (!vm_ecall_register(sys->ecall_router, HOST_SYS_FFLUSH, pipe_sys_fflush)) {
        fprintf(stderr, "host: register SYS_FFLUSH failed\n");
        return false;
    }

    return true;
}
#endif  /* PIPE_MODE_SUPPORTED */

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
        bool any_td_mounted = false;
        for (unsigned i = 0; i < hc.mount_count; i++) {
            const HostMount *m = &hc.mounts[i];
            if (m->kind == HOST_MOUNT_TD) {
                if (any_td_mounted) {
                    fprintf(stderr, "host: vm.cfg: multiple [mount.*] of "
                            "type=td not supported in M.3a (ignoring "
                            "mount.%s)\n", m->name);
                    continue;
                }
                if (!vm_host_fs_mount_fatfs(m->name, 0, &g_fs)) {
                    fprintf(stderr, "host: vm_host_fs_mount_fatfs('%s') "
                            "failed\n", m->name);
                    return 1;
                }
                fprintf(stderr, "host: /%s mounted (FatFs, %u KB pool"
                        "%s)\n", m->name, (unsigned)(POOL_BYTES / 1024),
                        m->size_kb ? "; size_kb override ignored" : "");
                any_td_mounted = true;
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
