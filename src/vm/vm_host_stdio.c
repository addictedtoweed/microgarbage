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
#include "vm/vm_core.h"
#include "vm/vm_ecall.h"

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <stdlib.h>
#include <string.h>

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

/* atexit hook: restore the saved termios so the user's shell
 * isn't left in raw mode after we exit. */
static void restore_termios_atexit(void) {
    if (g_termios_saved && g_termios_fd >= 0) {
        tcsetattr(g_termios_fd, TCSAFLUSH, &g_termios_orig);
        g_termios_saved = false;
    }
}

/* Put the given fd into raw mode, saving its current termios for
 * later restore. Returns true if anything was changed (so the
 * atexit hook should run), false if the fd is not a tty. */
static bool enable_raw_mode(int fd) {
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

/* Restore the saved termios. Returns true if anything changed
 * (i.e., we were previously in raw mode), false if there was
 * no saved state to restore. */
static bool disable_raw_mode(void) {
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
 * if the fd is not a tty or no stdio was installed. */
bool vm_host_stdio_set_raw_mode(bool enable) {
    if (!g_in_file) return false;
    int fd = fileno(g_in_file);
    if (fd < 0 || !isatty(fd)) return false;

    if (enable) {
        if (g_termios_saved) return true;   /* already raw */
        return enable_raw_mode(fd);
    } else {
        return disable_raw_mode();
    }
}

/* Set O_NONBLOCK on a fd. Returns 0 on success, -1 on failure. */
static int set_nonblock(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
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
    if (fd == 1) dest = g_out_file;
    else if (fd == 2) dest = g_err_file;
    else {
        cpu->regs[VM_REG_A0] = (uint32_t)-((int32_t)VM_EBADF);
        return;
    }
    if (!dest) {
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

    size_t written = fwrite(host_buf, 1, n, dest);
    /* Flushing here would hurt throughput. We rely on the FILE*'s
     * own buffering policy (line-buffered on TTYs, block-buffered
     * otherwise). Guests that need an explicit flush should
     * append a newline, or — eventually — call SYS_FFLUSH. */
    cpu->regs[VM_REG_A0] = (uint32_t)written;
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
    if (fd == 0) {
        if (g_out_file) fflush(g_out_file);
        if (g_err_file && g_err_file != g_out_file) fflush(g_err_file);
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

    int src_fd = fileno(g_in_file);
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
         * disable_raw_mode). Use it to disambiguate. */
        if (g_termios_saved) {
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

    /* Set the underlying stdin fd to non-blocking. We do this
     * unconditionally because SYS_READ is documented non-blocking. */
    int in_fd = fileno(in);
    if (in_fd >= 0) {
        set_nonblock(in_fd);
    }

    /* Raw mode (tty only). enable_raw_mode is a no-op for non-tty
     * fds, returning false silently. */
    if (raw && in_fd >= 0) {
        enable_raw_mode(in_fd);
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

    /* Commit streams to module state after registrations succeed
     * so that a failed install leaves the module state unchanged. */
    g_in_file  = in;
    g_out_file = out;
    g_err_file = err;
    return true;
}
