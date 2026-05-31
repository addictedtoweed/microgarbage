/* ============================================================
 *  host_util.h — small portable utilities for the shell host.
 *
 *  Four leaf helpers that don't belong in any of the subsystem
 *  modules but that main() reaches for during bring-up:
 *
 *    host_mkdir              — single-API mkdir across mingw/POSIX
 *    host_exe_dir            — directory containing argv[0], for
 *                              resolving host_files/ relative to
 *                              the executable instead of $PWD
 *    warn_if_no_real_console — user-facing advice when the native
 *                              Windows build is launched under a
 *                              Cygwin/mintty pty (Ctrl-C edge case)
 *    load_file               — slurp a file into a malloc'd buffer
 *
 *  These are all self-contained: no host globals, no other module's
 *  state. New util helpers belong here unless they pull in heavy
 *  dependencies, in which case give them their own module.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef HOST_UTIL_H
#define HOST_UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Wrap mkdir so callers can pass a POSIX mode regardless of platform.
 * On mingw the mode argument is ignored. Returns 0 on success, -1 on
 * failure (errno set as the underlying mkdir would). */
int host_mkdir(const char *path, int mode);

/* Fill `out` with the directory containing this executable (no
 * trailing separator). Returns true on success; on failure leaves
 * `out` empty (callers fall back to the plain relative path, i.e.
 * CWD behaviour). Windows: GetModuleFileNameA. Linux: /proc/self/exe.
 * Cygwin / other POSIX: returns false (no behaviour change). */
bool host_exe_dir(char *out, size_t out_sz);

/* On native Windows, warn the user if stdin isn't a real console
 * (the classic "host.exe launched under mintty" case where Ctrl-C
 * doesn't arrive as CTRL_C_EVENT). No-op on every other platform.
 * The message tells them how to get working Ctrl-C. */
void warn_if_no_real_console(void);

/* Read `path` into a freshly malloc'd buffer. On success: *out_buf
 * holds the buffer (caller frees), *out_size holds its byte count,
 * returns 0. On failure: returns -1 and prints "host: cannot open
 * 'PATH'" on stderr; *out_buf and *out_size are not written. */
int load_file(const char *path, uint8_t **out_buf, size_t *out_size);

#endif /* HOST_UTIL_H */
