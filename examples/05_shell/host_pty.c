/* ============================================================
 *  host_pty.c — POSIX pseudoterminal transport.
 *
 *  See host_pty.h for the contract.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "host_pty.h"

#ifdef PTY_MODE_SUPPORTED

#include "vm/vm_host_transport.h"
#include "vm/vm_host_stdio.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>

/* Single-instance: only one pty per host process. */
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
         * from real-disconnect requires more lifecycle work. */
        return 0;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
    if (errno == EIO)    return 0;   /* common 'no slave attached' code */
    return -5;
}

static int pty_t_write(VmHostTransport *t, const void *buf, unsigned n) {
    (void)t;
    if (g_pty_master_fd < 0) return -5;
    /* Same LF->CRLF policy as the TCP transport. */
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
        ssize_t w = write(g_pty_master_fd, p + run_start, n - run_start);
        if (w < 0 && errno != EAGAIN) return -5;
        total_in += (unsigned)(n - run_start);
    }
    return (int)total_in;
}

static int pty_t_flush(VmHostTransport *t) {
    (void)t;
    /* POSIX write to a pty master is unbuffered at the
     * application layer. The kernel may buffer in the pty driver
     * but tcdrain() would block — not what we want. No-op. */
    return 0;
}

static int pty_t_set_raw(VmHostTransport *t, bool enable) {
    (void)t;
    if (g_pty_master_fd < 0) return -5;
    /* tcgetattr/tcsetattr on a pty master affects the slave-side
     * termios (per POSIX). */
    struct termios ts;
    if (tcgetattr(g_pty_master_fd, &ts) != 0) return -5;
    if (enable) {
        cfmakeraw(&ts);
    } else {
        /* Restore cooked-ish defaults. We don't preserve the EXACT
         * termios across toggles (that would need a saved copy);
         * approximate cooked is good enough for a transport that's
         * typically used in raw mode. */
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

bool pty_install(struct VmSystem *sys) {
    (void)sys;   /* stdio install happens in main() before this */
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

    /* Non-blocking on the master so reads return 0 instead of
     * blocking when no slave is connected. */
    int flags = fcntl(master, F_GETFL, 0);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0) {
        fprintf(stderr, "host: fcntl O_NONBLOCK on pty master failed: %s\n",
                strerror(errno));
        close(master);
        return false;
    }

    /* Put the slave in raw mode by default — that's what TUI demos
     * want. The shell handles its own line editing; cooked mode
     * would echo and buffer in ways the shell doesn't expect. */
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

    vm_host_set_transport(&g_pty_transport);
    return true;
}

#else  /* !PTY_MODE_SUPPORTED */

/* ISO C forbids an empty translation unit. On native Windows the
 * whole file body above is gated out — give the linker something
 * to chew so -Wpedantic stays happy. The typedef is unreferenced
 * but legal. */
typedef int host_pty_unused_on_this_platform;

#endif /* PTY_MODE_SUPPORTED */
