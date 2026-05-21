/* ============================================================
 *  vm_host_stdio_win32.c — Windows console helpers
 *
 *  This file is only compiled on Windows builds (mingw, MSVC).
 *  On Cygwin and on Linux/macOS we use the POSIX termios path
 *  in vm_host_stdio.c.
 *
 *  Two responsibilities:
 *
 *  1. attach_console_if_native
 *     Called very early by the host's main(). If the binary is
 *     a native-Windows GUI-subsystem binary (no console attached
 *     by the loader) and it has a parent process with a console,
 *     attach to that console so stdio works without popping a
 *     new conhost window. If there's no parent console, allocate
 *     one — for the "double-click from Explorer" case.
 *
 *     A console-subsystem binary launched from cmd or Windows
 *     Terminal already has a console attached and this function
 *     is a no-op.
 *
 *  2. enable_raw_mode / disable_raw_mode
 *     Used by vm_host_stdio.c when its termios path can't be
 *     applied to the input fd because the underlying handle is
 *     a real Windows console handle rather than a Cygwin pty.
 *
 *     We probe via GetConsoleMode; if it succeeds, we own the
 *     console and SetConsoleMode is the right API. If it fails,
 *     we return false and the caller falls back to termios (or
 *     gives up if that also fails).
 *
 *  All entry points are exposed via the prototypes in
 *  vm_host_stdio.h, which are #ifdef _WIN32 — so calling code
 *  in vm_host_stdio.c doesn't need to know whether we're
 *  available.
 *
 *  Public domain (CC0).
 * ============================================================ */

#ifndef _WIN32
/* On non-Windows builds this file is empty so it can sit in
 * VM_CORE_SRCS without breaking the build. */
typedef int vm_host_stdio_win32_not_compiled_on_this_platform;

#else

#include "vm/vm_host_stdio.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <io.h>           /* _fileno, _get_osfhandle */
#include <fcntl.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

/* ============================================================
 *  Saved state for restore
 * ============================================================ */

static bool   g_saved_valid = false;
static DWORD  g_saved_in_mode = 0;
static DWORD  g_saved_out_mode = 0;
static HANDLE g_saved_in_handle = INVALID_HANDLE_VALUE;
static HANDLE g_saved_out_handle = INVALID_HANDLE_VALUE;

/* ============================================================
 *  Public: attach to parent's console, or allocate one.
 *
 *  Returns:
 *     1 = attached or allocated, we own the console
 *     0 = a console was already attached when called (e.g.,
 *         the binary was launched from cmd; nothing to do)
 *    -1 = no parent console AND AllocConsole failed
 *
 *  After a successful attach/alloc we rebind the C runtime's
 *  stdin/stdout/stderr to the console — without that step,
 *  printf and the rest still write to wherever they were
 *  originally pointed.
 * ============================================================ */
int vm_host_stdio_win32_attach_console_if_native(void) {
    /* Is there already a console attached to this process?
     * GetConsoleWindow returns NULL if not. We test by trying
     * GetStdHandle + GetConsoleMode on it. */
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD probe;
    if (h != INVALID_HANDLE_VALUE && h != NULL &&
        GetConsoleMode(h, &probe)) {
        /* Already have a working console. */
        return 0;
    }

    /* Try to attach to the parent process's console. This works
     * when, e.g., the binary is launched from cmd or PowerShell
     * but was compiled as a GUI subsystem app — uncommon for us
     * but harmless. */
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        /* Successfully attached. Rebind FILE*. */
        freopen("CONIN$",  "r", stdin);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        return 1;
    }

    /* No parent console — caller was double-clicked from
     * Explorer. Allocate a new one so the user sees something
     * instead of a flash-and-exit. */
    if (AllocConsole()) {
        freopen("CONIN$",  "r", stdin);
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
        return 1;
    }

    return -1;
}

/* ============================================================
 *  Public: probe whether `fd` corresponds to a Windows console
 *  handle. Returns true if so. False if it's a pipe, file,
 *  Cygwin pty, or invalid.
 * ============================================================ */
bool vm_host_stdio_win32_is_console(int fd) {
    if (fd < 0) return false;
    intptr_t raw = _get_osfhandle(fd);
    if (raw == -1 || raw == -2) return false;
    HANDLE h = (HANDLE)raw;
    DWORD mode;
    return GetConsoleMode(h, &mode) ? true : false;
}

/* ============================================================
 *  Public: enable raw mode on a Windows console.
 *
 *  Returns true if we successfully switched into raw mode,
 *  false if `fd` isn't a Windows console (caller should fall
 *  back to termios) or if SetConsoleMode failed.
 *
 *  We save the previous mode so disable_raw_mode can restore it.
 *  Only one fd's state is saved at a time; calling enable twice
 *  with different fds is unsupported (and unlikely in practice).
 * ============================================================ */
bool vm_host_stdio_win32_enable_raw_mode(int fd) {
    if (fd < 0) return false;
    intptr_t raw_in = _get_osfhandle(fd);
    if (raw_in == -1 || raw_in == -2) return false;
    HANDLE hin = (HANDLE)raw_in;

    DWORD in_mode;
    if (!GetConsoleMode(hin, &in_mode)) {
        /* Not a console handle. Caller falls back. */
        return false;
    }

    HANDLE hout = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD out_mode = 0;
    bool have_out = (hout != INVALID_HANDLE_VALUE && hout != NULL &&
                     GetConsoleMode(hout, &out_mode));

    /* Save state. */
    g_saved_in_mode    = in_mode;
    g_saved_in_handle  = hin;
    if (have_out) {
        g_saved_out_mode    = out_mode;
        g_saved_out_handle  = hout;
    } else {
        g_saved_out_handle  = INVALID_HANDLE_VALUE;
    }
    g_saved_valid = true;

    /* Input: clear line/echo/processed; set VT input so the
     * console emits VT escape sequences (arrows, etc.) instead
     * of high-bit characters. */
    DWORD new_in = in_mode;
    new_in &= ~((DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT |
                        ENABLE_PROCESSED_INPUT));
    new_in |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    /* ENABLE_WINDOW_INPUT lets us see resize events as records;
     * we don't currently consume them, but enabling it does no
     * harm — non-VT records just get filtered later. Skip for
     * now to keep the read path simple. */
    if (!SetConsoleMode(hin, new_in)) {
        g_saved_valid = false;
        return false;
    }

    /* Output: enable VT processing so our ESC[...m sequences
     * actually do something. Older Windows 10 versions need
     * this; newer ones often default to on but it's idempotent. */
    if (have_out) {
        DWORD new_out = out_mode;
        new_out |= ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                   DISABLE_NEWLINE_AUTO_RETURN;
        /* If this fails (very old Windows), we still have a
         * usable input mode — VT output won't work but the
         * rest of the system will. Soldier on. */
        (void)SetConsoleMode(hout, new_out);
    }

    return true;
}

bool vm_host_stdio_win32_disable_raw_mode(void) {
    if (!g_saved_valid) return false;
    SetConsoleMode(g_saved_in_handle, g_saved_in_mode);
    if (g_saved_out_handle != INVALID_HANDLE_VALUE) {
        SetConsoleMode(g_saved_out_handle, g_saved_out_mode);
    }
    g_saved_valid = false;
    return true;
}

/* ============================================================
 *  Public: non-blocking read for Windows console.
 *
 *  ReadFile on a console handle in raw + VT input mode returns
 *  byte sequences in VT form, but it blocks. We need
 *  non-blocking semantics.
 *
 *  WaitForSingleObject on a console handle is signaled when
 *  there's input ready. Use it as a poll.
 *
 *  Returns bytes read (>= 0), or -1 on error.
 *  Returns 0 if no data ready.
 * ============================================================ */
int vm_host_stdio_win32_read_bytes_nonblock(int fd, void *buf, unsigned cap) {
    if (fd < 0 || cap == 0) return 0;
    intptr_t raw = _get_osfhandle(fd);
    if (raw == -1 || raw == -2) return -1;
    HANDLE h = (HANDLE)raw;

    /* Only valid for console handles in our use case. The caller
     * (vm_host_stdio.c) checks the kind first. */
    DWORD waited = WaitForSingleObject(h, 0);
    if (waited != WAIT_OBJECT_0) {
        return 0;
    }

    /* There may be non-key events (focus, window resize). They
     * also signal the wait. Use PeekConsoleInput to filter — if
     * the next record isn't a key/buffer event with VT bytes,
     * drain it and return 0 so the caller polls again next
     * frame. Practical effect: a single ReadFile is enough
     * because VT input mode delivers character bytes; the rare
     * resize/focus event drains a slot without producing bytes.
     *
     * For simplicity in this initial implementation, just call
     * ReadFile and trust VT input mode to give us bytes. If we
     * see this be wrong in practice we'll add the PeekConsoleInput
     * filtering. */
    DWORD got = 0;
    if (!ReadFile(h, buf, cap, &got, NULL)) {
        return -1;
    }
    return (int)got;
}

#endif /* _WIN32 */
