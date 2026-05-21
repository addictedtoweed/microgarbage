/* ============================================================
 *  vm_host_stdio.h — host-side stdio bridge for guest VMs
 *
 *  Provides optional handlers for SYS_READ and SYS_WRITE that
 *  forward bytes between guest VMs and a host-side input/output
 *  pair — by default the process's stdin/stdout/stderr, but
 *  configurable to any FILE* (sockets, pipes, log files, etc.).
 *
 *  This is a HOST-ONLY facility — it depends on <stdio.h> being
 *  available. Embedded firmware where the "host" is bare-metal
 *  ignores this header and provides its own SYS_READ / SYS_WRITE
 *  handlers (e.g., ones that route bytes to a UART).
 *
 *  ---------------------------------------------------------------
 *  Quick start
 *  ---------------------------------------------------------------
 *
 *    VmSystem sys;
 *    vm_system_init(&sys, &cfg);
 *    vm_host_install_stdio(&sys);     // default config
 *    vm_system_load_vm(...);
 *    vm_system_run(...);
 *
 *  Once installed, guests can call SYS_WRITE(1, buf, n) to write
 *  to host stdout and SYS_READ(0, buf, n) to read from host stdin.
 *  fd=2 routes writes to stderr. Other fds return -EBADF.
 *
 *  ---------------------------------------------------------------
 *  Customizing the streams (TCP, files, raw mode, ...)
 *  ---------------------------------------------------------------
 *
 *  For interactive use — TUIs, games, the eventual shell-VM — the
 *  default cooked, line-buffered stdin is not what you want. Use
 *  vm_host_install_stdio_ex() with a VmHostStdioConfig:
 *
 *    VmHostStdioConfig sio = {
 *        .stdin_src    = NULL,    // NULL = process stdin
 *        .stdout_dest  = NULL,    // NULL = process stdout
 *        .stderr_dest  = NULL,    // NULL = process stderr
 *        .raw_mode     = true,    // turn off canonical/echo on tty
 *    };
 *    vm_host_install_stdio_ex(&sys, &sio);
 *
 *  raw_mode only affects FILE*s that are TTYs. If stdin_src is a
 *  pipe or socket, raw_mode is a no-op (those streams are already
 *  byte-oriented). The original termios is saved at install time
 *  and restored automatically at process exit via atexit().
 *
 *  ---------------------------------------------------------------
 *  Non-blocking reads
 *  ---------------------------------------------------------------
 *
 *  SYS_READ is non-blocking: if no bytes are available right now,
 *  it returns 0 immediately rather than waiting. This is essential
 *  for game-loop / TUI patterns: the guest polls input every
 *  frame, doing other work when there's none. To pace yourself,
 *  pair SYS_READ with SYS_YIELD inside the poll loop.
 *
 *  Implementation: the FILE*'s underlying fd is put into
 *  non-blocking mode via fcntl(O_NONBLOCK) at install time. EOF
 *  on the underlying stream is reported as VM_EIO, distinguishable
 *  from a 0-byte "nothing ready yet" return.
 *
 *  ---------------------------------------------------------------
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef VM_HOST_STDIO_H
#define VM_HOST_STDIO_H

#include <stdbool.h>
#include <stdio.h>
#include "vm/vm_system.h"

/* ============================================================
 *  Configuration
 * ============================================================ */

typedef struct {
    /* Where SYS_READ bytes come from. NULL = host stdin. */
    FILE *stdin_src;

    /* Where SYS_WRITE bytes go (fd=1). NULL = host stdout. */
    FILE *stdout_dest;

    /* Where SYS_WRITE bytes go (fd=2). NULL = host stderr. */
    FILE *stderr_dest;

    /* If true AND stdin_src is a TTY, put it into raw mode:
     *   - no canonical (line) input — every keystroke delivered
     *   - no local echo
     *   - signals (Ctrl-C, Ctrl-Z, Ctrl-\) NOT generated; those
     *     bytes are delivered to the guest like any other input
     *   - input timer set so reads return immediately if nothing
     *     is queued
     *
     * The original termios state is saved at install and restored
     * at process exit (via atexit) — even on abnormal exit paths
     * like uncaught signals (provided the signal handler returns
     * normally and lets atexit run).
     *
     * Ignored (no-op) if stdin_src is not a TTY. */
    bool raw_mode;

    /* Optional fd overrides. When >= 0, the bridge uses these fds
     * directly for read()/write() instead of asking fileno() on
     * the FILE*s. Useful when the FILE* doesn't have a backing
     * fd (e.g., when wrapping a Win32 HANDLE via fopencookie),
     * or when you want to bypass stdio buffering for I/O while
     * keeping fflush() useful for prompt-flush semantics.
     *
     * Defaults to -1 (use fileno on the FILE*) which is the
     * historical behavior. */
    int stdin_fd_override;
    int stdout_fd_override;
    int stderr_fd_override;
} VmHostStdioConfig;

/* ============================================================
 *  Install
 * ============================================================ */

/* Install SYS_READ and SYS_WRITE handlers with default config:
 *   - stdin from process stdin
 *   - stdout/stderr to process stdout/stderr
 *   - raw_mode disabled
 *
 * Returns true on success, false if a handler slot is already
 * occupied or sys is NULL. */
bool vm_host_install_stdio(VmSystem *sys);

/* Install with a specific config. Same return semantics. Pass
 * NULL for cfg to get the same defaults as vm_host_install_stdio. */
bool vm_host_install_stdio_ex(VmSystem *sys,
                              const VmHostStdioConfig *cfg);

/* Toggle raw mode on the installed stdin fd at runtime.
 *
 *   enable=true   put the tty into raw mode (no echo, no canonical
 *                 line buffering, no signal generation, immediate
 *                 byte-at-a-time delivery)
 *   enable=false  restore the saved termios (cooked mode)
 *
 * Returns true on success, false if no stdio is installed, the
 * fd isn't a tty, or the underlying tcsetattr call fails.
 *
 * Used by the SYS_TTY_SET_RAW syscall to let a guest opt into
 * raw mode while it runs (e.g., a game) and restore cooked mode
 * before exiting. The original termios is preserved across
 * toggles, so a guest can flip raw on/off many times. */
bool vm_host_stdio_set_raw_mode(bool enable);

/* Read up to `cap` bytes from the installed stdin fd into `buf`,
 * non-blocking. Returns the number of bytes read, 0 if nothing
 * is available, or -1 on error / no stdio installed.
 *
 * Used by the TUI input parser (vm_host_tui) to consume bytes
 * without going through the SYS_READ guest path. This shares
 * the same stdin fd that SYS_READ uses, so a guest that calls
 * SYS_READ while another VM holds the canvas could race — but
 * the canvas-owner-only rule for SYS_TUI_POLL_EVENT plus the
 * fact that snake-style guests don't mix SYS_READ with TUI
 * input mean this isn't a practical concern. Documented for
 * future awareness. */
int vm_host_stdio_read_bytes_nonblock(void *buf, unsigned cap);

#endif /* VM_HOST_STDIO_H */
