/* ============================================================
 *  vm_host_stdio.c — implementation of host stdio bridge
 *  See vm/vm_host_stdio.h for the public contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

/* For fileno, fcntl, termios, atexit, isatty — must precede any
 * system header. */
#define _POSIX_C_SOURCE 200809L

#include "vm/vm_host_stdio.h"
#include "vm/vm_host_transport.h"
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"
#include "vm/vm_sched.h"   /* VM_SCHED_MAX_VMS for per-VM transport table */

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

/* POSIX termios is the raw-mode API on Linux/macOS/Cygwin. On
 * mingw (native Windows) the header doesn't exist and raw mode
 * goes through vm_host_stdio_win32_enable_raw_mode instead. */
#ifndef _WIN32
#include <termios.h>
#else
/* Stub the termios bits enough that the rest of the file can
 * stay structurally the same. The fields we use are
 *   tcflag_t, struct termios, tcsetattr, tcgetattr, etc.
 * — none of these are ever called on _WIN32 builds because
 * enable_raw_mode short-circuits to the Win32 path before
 * reaching them. We provide minimal stand-ins for declarations
 * only so the file compiles. */
typedef unsigned int tcflag_t;
struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    char     c_cc[32];
};
#define ICRNL 0
#define IXON 0
#define BRKINT 0
#define INPCK 0
#define ISTRIP 0
#define ECHO 0
#define ICANON 0
#define ISIG 0
#define IEXTEN 0
#define VMIN 0
#define VTIME 1
#define TCSAFLUSH 0
static inline int tcgetattr(int fd, struct termios *t) { (void)fd; (void)t; return -1; }
static inline int tcsetattr(int fd, int act, const struct termios *t) {
    (void)fd; (void)act; (void)t; return -1;
}
#endif

/* ============================================================
 *  Module state
 *
 *  There can only be one stdio bridge installed per process —
 *  termios is process-global state and the atexit hook can only
 *  meaningfully restore one previous tty state. So the configured
 *  FILE pointers and the saved termios live here as module-level
 *  statics rather than per-VmSystem.
 *
 *  An attempt to install twice (without an explicit uninstall in
 *  between, which we don't currently provide) is allowed for the
 *  handler registration to fail naturally on the second call; we
 *  don't re-apply termios changes.
 * ============================================================ */

static FILE *g_in_file  = NULL;
static FILE *g_out_file = NULL;
static FILE *g_err_file = NULL;

/* fd overrides (used by read()/write() in preference to fileno()
 * on the FILE*s above). When -1, the bridge falls back to
 * fileno(FILE*) — the historical behavior. */
static int   g_in_fd  = -1;
static int   g_out_fd = -1;
static int   g_err_fd = -1;

/* ============================================================
 *  Active transport (round U.2; per-VM in round U.6)
 *
 *  U.2 introduced a single process-global transport pointer. U.6
 *  extends this to a per-VM table so multiple shells, each bound
 *  to a different transport, can coexist in one host process.
 *
 *  Lookup precedence (highest first):
 *    1. g_transport_by_vm[vm_id]  — set via set_transport_for_vm
 *    2. g_default_transport       — set via vm_host_set_transport
 *    3. NULL                      — falls through to legacy stdio
 *                                   path (g_in_file/g_out_file)
 *
 *  The single-arg `vm_host_set_transport(t)` API still works and
 *  sets the default. Single-session demos (no per-VM bindings) see
 *  identical behavior to U.5. Multi-session hosts call
 *  `vm_host_set_transport_for_vm(vm_id, t)` for each session.
 * ============================================================ */

static VmHostTransport *g_default_transport = NULL;
static VmHostTransport *g_transport_by_vm[VM_SCHED_MAX_VMS];

VmHostTransport *vm_host_set_transport(VmHostTransport *t) {
    VmHostTransport *prev = g_default_transport;
    g_default_transport = t;
    return prev;
}

VmHostTransport *vm_host_get_transport(void) {
    return g_default_transport;
}

VmHostTransport *vm_host_set_transport_for_vm(uint16_t vm_id,
                                              VmHostTransport *t) {
    if (vm_id >= VM_SCHED_MAX_VMS) return NULL;
    VmHostTransport *prev = g_transport_by_vm[vm_id];
    g_transport_by_vm[vm_id] = t;
    return prev;
}

VmHostTransport *vm_host_get_transport_for_vm(uint16_t vm_id) {
    if (vm_id < VM_SCHED_MAX_VMS && g_transport_by_vm[vm_id]) {
        return g_transport_by_vm[vm_id];
    }
    return g_default_transport;
}

/* Unload hook: clear a VM's per-VM transport binding when it exits,
 * so a later VM that reuses the same vm_id slot doesn't inherit a
 * stale transport pointer. Registered by vm_host_install_stdio_ex. */
static void stdio_transport_unload_hook(uint16_t vm_id, void *userdata) {
    (void)userdata;
    if (vm_id < VM_SCHED_MAX_VMS) {
        g_transport_by_vm[vm_id] = NULL;
    }
}

/* ============================================================
 *  Delegate hooks for file fds
 *
 *  When vm_host_fs is also installed, it calls the setter below
 *  to install function pointers that handle fd >= 3. Our
 *  read/write/close handlers consult the hooks for any fd
 *  outside the stdio range (0/1/2) and delegate the actual work.
 *  When fs is NOT installed (hooks remain NULL), reads/writes
 *  to fd >= 3 return -EBADF.
 *
 *  The hooks are stored as static here (not extern from fs)
 *  so that vm_host_stdio.c can be linked without vm_host_fs.c.
 *  The fs module only needs to know about the setter function. */
typedef int32_t (*vm_host_fs_read_hook_t)(int fd, void *buf, uint32_t n);
typedef int32_t (*vm_host_fs_write_hook_t)(int fd, const void *buf, uint32_t n);
typedef int32_t (*vm_host_fs_close_hook_t)(int fd);

static vm_host_fs_read_hook_t  g_fs_read_hook  = NULL;
static vm_host_fs_write_hook_t g_fs_write_hook = NULL;
static vm_host_fs_close_hook_t g_fs_close_hook = NULL;

/* Called by vm_host_install_fs to wire up the delegate. Pass NULL
 * for all three to disconnect. Public so vm_host_fs.c can reach it
 * without including vm_host_stdio.h's private types. */
void vm_host_stdio_set_fs_hooks(vm_host_fs_read_hook_t r,
                                vm_host_fs_write_hook_t w,
                                vm_host_fs_close_hook_t c) {
    g_fs_read_hook  = r;
    g_fs_write_hook = w;
    g_fs_close_hook = c;
}

/* Whether we've already saved and modified stdin's termios. If
 * true, the atexit hook will restore it. */
static bool          g_termios_saved = false;
static struct termios g_termios_orig;
static int           g_termios_fd = -1;

/* On Windows builds, true if we successfully enabled raw mode
 * via the Win32 console API path. Mutually exclusive with
 * g_termios_saved — only one of the two paths is in effect at
 * a time. */
#ifdef _WIN32
static bool g_win32_raw_active = false;
#endif

/* atexit hook: restore the saved termios so the user's shell
 * isn't left in raw mode after we exit. Also restores the
 * Windows console mode if that path was used. */
static void restore_termios_atexit(void) {
    if (g_termios_saved && g_termios_fd >= 0) {
        tcsetattr(g_termios_fd, TCSAFLUSH, &g_termios_orig);
        g_termios_saved = false;
    }
#ifdef _WIN32
    if (g_win32_raw_active) {
        vm_host_stdio_win32_disable_raw_mode();
        g_win32_raw_active = false;
    }
#endif
}

/* Put the given fd into raw mode, saving its current termios for
 * later restore. Returns true if anything was changed (so the
 * atexit hook should run), false if the fd is not a tty.
 *
 * On Windows builds, this first tries the Win32 console API
 * (via vm_host_stdio_win32_enable_raw_mode). If the fd is a real
 * Windows console handle, that takes care of things. Otherwise
 * (Cygwin pty, Linux tty), we fall through to the termios path
 * below. */
static bool enable_raw_mode(int fd) {
#ifdef _WIN32
    if (vm_host_stdio_win32_enable_raw_mode(fd)) {
        /* Mark as if we set raw mode so disable_raw_mode runs
         * the Win32 restore path. g_termios_saved stays false
         * so we don't try to tcsetattr a non-tty. We use a
         * dedicated flag to disambiguate. */
        g_win32_raw_active = true;
        atexit(restore_termios_atexit);
        return true;
    }
#endif
    if (!isatty(fd)) return false;

    struct termios raw;
    if (tcgetattr(fd, &g_termios_orig) != 0) return false;
    raw = g_termios_orig;

    /* Disable: ICRNL (CR→NL translation), IXON (XON/XOFF flow ctl),
     * BRKINT (BREAK generates SIGINT), INPCK (parity check),
     * ISTRIP (strip high bit). */
    raw.c_iflag &= ~(tcflag_t)(ICRNL | IXON | BRKINT | INPCK | ISTRIP);
    /* We deliberately keep c_oflag (OPOST) enabled — we WANT
     * \n → \r\n translation on a tty so guest writes look right
     * at the terminal. */
    /* Disable: ECHO (local echo), ICANON (line buffering),
     * ISIG (signal generation from Ctrl-C / Ctrl-Z / Ctrl-\),
     * IEXTEN (extended input processing like literal-next Ctrl-V). */
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | ISIG | IEXTEN);

    /* Read returns immediately with whatever's available (0 if
     * nothing). Combined with O_NONBLOCK this gives us the
     * non-blocking semantics SYS_READ promises. */
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSAFLUSH, &raw) != 0) return false;

    g_termios_fd = fd;
    g_termios_saved = true;
    atexit(restore_termios_atexit);
    return true;
}

/* Restore the saved termios (or Win32 console state). Returns
 * true if anything changed (i.e., we were previously in raw
 * mode via either path), false if there was no saved state. */
static bool disable_raw_mode(void) {
#ifdef _WIN32
    if (g_win32_raw_active) {
        vm_host_stdio_win32_disable_raw_mode();
        g_win32_raw_active = false;
        return true;
    }
#endif
    if (!g_termios_saved || g_termios_fd < 0) return false;
    tcsetattr(g_termios_fd, TCSAFLUSH, &g_termios_orig);
    g_termios_saved = false;
    /* Keep g_termios_fd set — if the guest re-enables raw mode
     * later, we want enable_raw_mode to know which fd to operate
     * on. (It'll re-tcgetattr its current state and save it again.) */
    return true;
}

/* Public: toggle raw mode on the previously-installed stdin
 * fd. Used by SYS_TTY_SET_RAW. Returns true on success, false
 * if the fd is not a tty / Windows console or no stdio was
 * installed. */
bool vm_host_stdio_set_raw_mode(bool enable) {
    /* Round U.2/U.6: this helper still operates on the DEFAULT
     * transport. Per-VM raw-mode toggles are done by the TUI
     * module reading its session's transport directly; this
     * fallback path handles non-TUI callers (legacy code).
     * Transports that don't supply set_raw fall through to the
     * termios path on g_in_file. */
    VmHostTransport *t = g_default_transport;
    if (t && t->set_raw) {
        return t->set_raw(t, enable) >= 0;
    }

    if (!g_in_file) return false;
    int fd = fileno(g_in_file);
    if (fd < 0) return false;

#ifdef _WIN32
    /* On Windows we accept the fd if either isatty() says yes
     * (Cygwin pty) OR it's a real Windows console. */
    bool ok = isatty(fd) || vm_host_stdio_win32_is_console(fd);
    if (!ok) return false;
#else
    if (!isatty(fd)) return false;
#endif

    if (enable) {
        if (g_termios_saved
#ifdef _WIN32
            || g_win32_raw_active
#endif
            ) return true;   /* already raw */
        return enable_raw_mode(fd);
    } else {
        return disable_raw_mode();
    }
}

int vm_host_stdio_read_bytes_nonblock(void *buf, unsigned cap) {
    /* Round U.2: prefer the transport if one is installed. This
     * is the path the TUI host service uses for input — when a
     * pipe or TCP transport is active, this is how mouse/keyboard
     * bytes reach the input parser. */
    {
        VmHostTransport *t = g_default_transport;
        if (t && t->read_nonblock) {
            return t->read_nonblock(t, buf, cap);
        }
    }

    if (!g_in_file || cap == 0) return 0;
    int src_fd = (g_in_fd >= 0) ? g_in_fd : fileno(g_in_file);
    if (src_fd < 0) return -1;

#ifdef _WIN32
    /* On Windows console handles, the POSIX read() / O_NONBLOCK
     * combo doesn't always behave; use the Win32 console reader
     * for those, fall through to POSIX read for Cygwin ptys and
     * pipes. */
    if (g_win32_raw_active) {
        return vm_host_stdio_win32_read_bytes_nonblock(src_fd, buf, cap);
    }
#endif

    ssize_t r = read(src_fd, buf, cap);
    if (r > 0) return (int)r;
    if (r == 0) return 0;        /* no data / EOF — caller doesn't care which */
    /* errno EAGAIN/EWOULDBLOCK = no data ready */
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    return -1;
}

/* Set O_NONBLOCK on a fd. Returns 0 on success, -1 on failure.
 *
 * On _WIN32 builds, fcntl/F_GETFL/O_NONBLOCK don't exist. The
 * Win32 path doesn't need this — Windows console reads are
 * gated by WaitForSingleObject in vm_host_stdio_win32_read_*
 * rather than by non-blocking file flags. So we make this a
 * no-op on Windows. */
static int set_nonblock(int fd) {
#ifdef _WIN32
    (void)fd;
    return 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
#endif
}

/* ============================================================
 *  SYS_WRITE handler
 *
 *    a0 = fd (1 = stdout, 2 = stderr; else -EBADF)
 *    a1 = guest address of buffer
 *    a2 = byte count
 *    → a0 = bytes written, or -EFAULT / -EBADF
 * ============================================================ */

static void handle_write(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;

    uint32_t fd      = cpu->regs[VM_REG_A0];
    uint32_t guest_p = cpu->regs[VM_REG_A1];
    uint32_t n       = cpu->regs[VM_REG_A2];

    /* Delegate file fds to vm_host_fs if installed. */
    if (fd >= 3) {
        if (!g_fs_write_hook) {
            cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBADF);
            return;
        }
        if (n == 0) {
            cpu->regs[VM_REG_A0] = 0;
            return;
        }
        const void *host_buf = vm_translate_read(cpu, guest_p, n);
        if (!host_buf) {
            cpu->trap_cause = TRAP_NONE;
            cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EFAULT);
            return;
        }
        int32_t r = g_fs_write_hook((int)fd, host_buf, n);
        cpu->regs[VM_REG_A0] = (uint32_t)r;
        return;
    }

    FILE *dest = NULL;
    int   dest_fd = -1;
    if (fd == 1) {
        dest = g_out_file;
        dest_fd = g_out_fd;
    } else if (fd == 2) {
        dest = g_err_file;
        dest_fd = g_err_fd;
    } else {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBADF);
        return;
    }

    /* Round U.2/U.6: if a transport is bound to this VM (or a
     * process-default transport is set), it owns stdout AND
     * stderr (they're the same byte sink as far as the user is
     * concerned). Route through it before the FILE/fd fallback.
     * The transport's `write` is best-effort and may write less
     * than `n`; we propagate the count to the guest. */
    VmHostTransport *vt = vm_host_get_transport_for_vm(cpu->vm_id);
    if (vt && vt->write) {
        if (n == 0) {
            cpu->regs[VM_REG_A0] = 0;
            return;
        }
        const void *host_buf = vm_translate_read(cpu, guest_p, n);
        if (!host_buf) {
            cpu->trap_cause = TRAP_NONE;
            cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EFAULT);
            return;
        }
        int w = vt->write(vt, host_buf, n);
        cpu->regs[VM_REG_A0] = (uint32_t)w;   /* >=0 bytes or -errno */
        return;
    }

    if (!dest && dest_fd < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBADF);
        return;
    }

    if (n == 0) {
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    const void *host_buf = vm_translate_read(cpu, guest_p, n);
    if (!host_buf) {
        cpu->trap_cause = TRAP_NONE;
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EFAULT);
        return;
    }

    size_t written;
    if (dest_fd >= 0) {
        /* fd-override path: write directly via the POSIX syscall.
         * Used by hosts that supply an fd whose FILE* wrapping
         * doesn't roundtrip cleanly (e.g., a Cygwin attached
         * named-pipe HANDLE). The fd may be the same for stdout
         * and stderr — that's fine, both streams just flow to
         * the same destination. */
        ssize_t w = write(dest_fd, host_buf, n);
        if (w < 0) {
            cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EIO);
            return;
        }
        written = (size_t)w;
    } else {
        written = fwrite(host_buf, 1, n, dest);
    }
    /* Flushing here would hurt throughput. We rely on the FILE*'s
     * own buffering policy (line-buffered on TTYs, block-buffered
     * otherwise), with two exits hatches:
     *
     *   - SYS_READ on fd 0 fflushes stdout/stderr first, so any
     *     interactive prompt is visible before we block on input.
     *   - SYS_FFLUSH lets guests flush explicitly when they need
     *     to push output that has no newline (ANSI sequences,
     *     game-over screens, etc.). */
    cpu->regs[VM_REG_A0] = (uint32_t)written;
}

/* ============================================================
 *  SYS_FFLUSH handler
 *
 *    a0 = fd
 *      1 → fflush stdout
 *      2 → fflush stderr
 *      0 → fflush both (convenience; "flush everything")
 *      anything else → -EBADF
 *    → a0 = 0 on success, -EBADF on bad fd
 *
 *  Use case: guests producing terminal UI (ANSI escape sequences,
 *  partial lines) need their output to reach the screen at known
 *  syncpoints — game over messages, status updates, prompts.
 *  Without an explicit flush, the FILE* buffer can hold output
 *  indefinitely on a pipe/file destination (block-buffered) or
 *  until a newline on a TTY (line-buffered).
 * ============================================================ */

static void handle_fflush(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;
    uint32_t fd = cpu->regs[VM_REG_A0];

    /* Round U.2/U.6: defer to per-VM transport if installed.
     * fd 0/1/2 are all the same byte sink as far as the transport's
     * concerned. */
    VmHostTransport *vt = vm_host_get_transport_for_vm(cpu->vm_id);
    if (vt && vt->flush) {
        if (fd <= 2) {
            int r = vt->flush(vt);
            cpu->regs[VM_REG_A0] = (r >= 0) ? 0 : (uint32_t)r;
            return;
        }
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBADF);
        return;
    }

    if (fd == 0) {
        if (g_out_file) fflush(g_out_file);
        if (g_err_file && g_err_file != g_out_file) fflush(g_err_file);
        cpu->regs[VM_REG_A0] = 0;
        return;
    }
    if (fd == 1 && g_out_file) {
        fflush(g_out_file);
        cpu->regs[VM_REG_A0] = 0;
        return;
    }
    if (fd == 2 && g_err_file) {
        fflush(g_err_file);
        cpu->regs[VM_REG_A0] = 0;
        return;
    }
    cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBADF);
}

/* ============================================================
 *  SYS_READ handler
 *
 *    a0 = fd (0 = stdin; else -EBADF)
 *    a1 = guest address of buffer to fill
 *    a2 = maximum byte count
 *    → a0 = bytes read (0 = nothing ready),
 *      or -EBADF / -EFAULT / -EIO
 *
 *  Reads via the underlying file descriptor with read(2) directly
 *  rather than fread, to keep "would-block" cleanly separated
 *  from "EOF". fread loses this distinction because it returns 0
 *  for both, and feof/ferror are sticky.
 * ============================================================ */

static void handle_read(VmCpu *cpu, void *system) {
    (void)system;
    if (!cpu) return;

    uint32_t fd      = cpu->regs[VM_REG_A0];
    uint32_t guest_p = cpu->regs[VM_REG_A1];
    uint32_t n       = cpu->regs[VM_REG_A2];

    /* Before the guest blocks on stdin, make sure any buffered
     * stdout/stderr is visible to the user. Otherwise a prompt
     * like "$ " (no trailing newline) can sit in stdio's buffer
     * forever, leaving the user staring at a blank line wondering
     * if the program has hung. This mirrors what real terminals
     * and most libcs do — flush-before-blocking-read is the rule
     * that makes interactive prompts work. */
    VmHostTransport *vt = vm_host_get_transport_for_vm(cpu->vm_id);
    if (fd == 0) {
        if (vt && vt->flush) {
            (void)vt->flush(vt);
        } else {
            if (g_out_file) fflush(g_out_file);
            if (g_err_file && g_err_file != g_out_file) fflush(g_err_file);
        }
    }

    /* Delegate file fds to vm_host_fs if installed. */
    if (fd >= 3) {
        if (!g_fs_read_hook) {
            cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBADF);
            return;
        }
        if (n == 0) {
            cpu->regs[VM_REG_A0] = 0;
            return;
        }
        void *host_buf = vm_translate_write(cpu, guest_p, n);
        if (!host_buf) {
            cpu->trap_cause = TRAP_NONE;
            cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EFAULT);
            return;
        }
        int32_t r = g_fs_read_hook((int)fd, host_buf, n);
        cpu->regs[VM_REG_A0] = (uint32_t)r;
        return;
    }

    /* Round U.2/U.6: if a transport is bound to this VM, it owns
     * stdin. Same routing pattern as handle_write. */
    if (fd == 0 && vt && vt->read_nonblock) {
        if (n == 0) {
            cpu->regs[VM_REG_A0] = 0;
            return;
        }
        void *host_buf = vm_translate_write(cpu, guest_p, n);
        if (!host_buf) {
            cpu->trap_cause = TRAP_NONE;
            cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EFAULT);
            return;
        }
        int r = vt->read_nonblock(vt, host_buf, n);
        cpu->regs[VM_REG_A0] = (uint32_t)r;   /* >=0 or -errno */
        return;
    }

    if (fd != 0 || !g_in_file) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBADF);
        return;
    }
    if (n == 0) {
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    void *host_buf = vm_translate_write(cpu, guest_p, n);
    if (!host_buf) {
        cpu->trap_cause = TRAP_NONE;
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EFAULT);
        return;
    }

    int src_fd = (g_in_fd >= 0) ? g_in_fd : fileno(g_in_file);
    if (src_fd < 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EIO);
        return;
    }

    ssize_t r = read(src_fd, host_buf, n);
    if (r > 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)r;
    } else if (r == 0) {
        /* In cooked mode, read() == 0 means EOF (stdin closed).
         * In raw mode with VMIN=0, read() == 0 means "no data
         * ready right now" — NOT EOF. Same physical syscall,
         * different semantics, distinguished only by the termios
         * state we set up at install/toggle time.
         *
         * g_termios_saved tracks whether we're currently in raw
         * mode (set true by enable_raw_mode, cleared by
         * disable_raw_mode). Use it to disambiguate.
         *
         * Additional case: when stdin is a non-tty fd we've put
         * into non-blocking mode (e.g., a named pipe in a host
         * that uses --pipe), read() returning 0 USUALLY means
         * EOF — but for some attachment types it can mean "no
         * data" instead. We can't easily distinguish without
         * more context, so when the override path is in use we
         * report "no data" (returning 0 to the guest) and rely
         * on the host to detect a real EOF via a separate
         * mechanism (e.g., a SIGPIPE/SIGINT signal handler). */
        if (g_termios_saved || g_in_fd >= 0) {
            cpu->regs[VM_REG_A0] = 0;            /* no data ready */
        } else {
            cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EIO); /* EOF */
        }
    } else {
        /* r < 0: errno tells us what happened. */
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
            /* No bytes ready right now, or interrupted by signal.
             * Either way, "try again later" from guest's POV. */
            cpu->regs[VM_REG_A0] = 0;
        } else {
            cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EIO);
        }
    }
}

/* ============================================================
 *  Install
 * ============================================================ */

bool vm_host_install_stdio(VmSystem *sys) {
    return vm_host_install_stdio_ex(sys, NULL);
}

bool vm_host_install_stdio_ex(VmSystem *sys,
                              const VmHostStdioConfig *cfg) {
    if (!sys || !sys->ecall_router) return false;

    /* Apply config (NULL = defaults). */
    FILE *in  = (cfg && cfg->stdin_src)    ? cfg->stdin_src    : stdin;
    FILE *out = (cfg && cfg->stdout_dest)  ? cfg->stdout_dest  : stdout;
    FILE *err = (cfg && cfg->stderr_dest)  ? cfg->stderr_dest  : stderr;
    bool  raw = cfg && cfg->raw_mode;

    /* fd overrides — when ≥0, used by handle_read/handle_write
     * instead of fileno(FILE*). Stored separately so a host can
     * supply both a FILE* (for fflush) and an fd (for raw I/O)
     * — useful when the FILE* and fd don't reliably roundtrip
     * (e.g., Cygwin attached HANDLE). */
    int override_in  = (cfg && cfg->stdin_fd_override  > 0) ? cfg->stdin_fd_override  : -1;
    int override_out = (cfg && cfg->stdout_fd_override > 0) ? cfg->stdout_fd_override : -1;
    int override_err = (cfg && cfg->stderr_fd_override > 0) ? cfg->stderr_fd_override : -1;

    /* Note: > 0, not >= 0, because fd=0 is meaningful for the
     * default path (stdin); a caller that genuinely wants to
     * override to fd 0 should just use the default behavior. */

    /* Determine the fd to use for nonblock and raw-mode operations.
     * Prefer the override if set; otherwise fall back to fileno. */
    int in_fd = (override_in >= 0) ? override_in : fileno(in);
    if (in_fd >= 0) {
        set_nonblock(in_fd);
    }

    /* Raw mode. enable_raw_mode tries Win32 console mode first
     * (on _WIN32 builds), then falls back to termios for Cygwin
     * pty / Linux tty handles. If neither works the fd isn't a
     * usable terminal at all. */
    if (raw && in_fd >= 0) {
        if (!enable_raw_mode(in_fd)) {
            const char *why;
#ifdef _WIN32
            if (vm_host_stdio_win32_is_console(in_fd)) {
                why = "Windows SetConsoleMode failed";
            } else if (isatty(in_fd)) {
                why = "tcgetattr failed";
            } else {
                why = "not a tty or console";
            }
#else
            why = isatty(in_fd) ? "tcgetattr failed" : "not a tty";
#endif
            fprintf(stderr,
                "host: raw mode requested but unavailable on stdin "
                "(fd=%d, %s).\n"
                "      Interactive features may behave oddly. Try "
                "launching\n"
                "      from a real terminal (mintty / Windows Terminal /\n"
                "      cmd) or use 'set raw-mode false' in vm.cfg.\n",
                in_fd, why);
        }
    }

    /* Register handlers. If either registration fails (e.g.,
     * someone already installed a handler at that slot), back out
     * the other one too so we don't leave a partial install. */
    if (!vm_ecall_register(sys->ecall_router, SYS_WRITE, handle_write)) {
        return false;
    }
    if (!vm_ecall_register(sys->ecall_router, SYS_READ, handle_read)) {
        vm_ecall_unregister(sys->ecall_router, SYS_WRITE);
        return false;
    }
    if (!vm_ecall_register(sys->ecall_router, SYS_FFLUSH, handle_fflush)) {
        vm_ecall_unregister(sys->ecall_router, SYS_WRITE);
        vm_ecall_unregister(sys->ecall_router, SYS_READ);
        return false;
    }

    /* Commit streams to module state after registrations succeed
     * so that a failed install leaves the module state unchanged. */
    g_in_file  = in;
    g_out_file = out;
    g_err_file = err;
    g_in_fd    = override_in;
    g_out_fd   = override_out;
    g_err_fd   = override_err;

    /* Clear a VM's per-VM transport binding when it unloads, so a
     * reused vm_id slot doesn't inherit a stale transport pointer.
     * Registered once; the hook is idempotent (NULLing an already-
     * NULL slot is fine). */
    vm_system_register_unload_hook(sys, stdio_transport_unload_hook, NULL);
    return true;
}
