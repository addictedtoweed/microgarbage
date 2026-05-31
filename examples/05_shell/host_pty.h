/* ============================================================
 *  host_pty.h — POSIX pseudoterminal transport for the shell host.
 *
 *  --pty mode. Allocates a POSIX pty pair; the slave path (e.g.
 *  /dev/pts/7) is printed to stderr so the user can attach a
 *  terminal emulator (`screen /dev/pts/7`, `minicom -D ...`).
 *  Single-instance: one pty for the whole host. Mutually exclusive
 *  with --tcp= in main().
 *
 *  POSIX-only — Linux, BSD, macOS, Cygwin. Native Windows has
 *  ConPTY but that's a different API and out of scope; on those
 *  platforms PTY_MODE_SUPPORTED stays undefined and main() rejects
 *  --pty at startup.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HOST_PTY_H
#define HOST_PTY_H

#include <stdbool.h>

/* PTY transport requires posix_openpt + grantpt + unlockpt + ptsname.
 * Available on Linux, BSD, macOS, and Cygwin (POSIX-compliant). Not
 * available on mingw / native Windows builds — those have ConPTY,
 * which is a completely different API and is not in scope. */
#if !defined(_WIN32)
#  define PTY_MODE_SUPPORTED 1
#endif

#ifdef PTY_MODE_SUPPORTED

struct VmSystem;

/* Open a pty pair, print the slave path on stderr, set the slave
 * line discipline to raw (what TUI demos want), and install the
 * pty as the process-default transport via vm_host_set_transport.
 * Returns true on success. Stdio handlers must have been installed
 * by the caller already — this function only sets the transport. */
bool pty_install(struct VmSystem *sys);

#endif /* PTY_MODE_SUPPORTED */

#endif /* HOST_PTY_H */
