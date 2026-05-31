/* ============================================================
 *  vm_host_tui.c — terminal-canvas service for guest VMs
 *
 *  See vm/vm_host_tui.h for the public contract.
 *
 *  This module owns the canvas state (back + front buffers,
 *  clip rect, pen, terminal raw-mode tracking) and exposes
 *  SYS_TUI_* syscalls that operate on it.
 *
 *  Implementation note: the canvas dimensions, cell layout,
 *  SGR-emit logic, and command-buffer parsing are all here in
 *  host RAM. Guests are reduced to ~200 bytes of "command
 *  buffer builder" code in their tui.c.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

/* vm_host_tui_internal.h pulls in vm/vm_host_tui.h, vm/vm_core.h,
 * vm/vm_host_transport.h, and vm/vm_system.h; we just need the
 * extra ones here. */
#include "vm_host_tui_internal.h"

#include "vm/vm_ecall.h"
#include "vm/vm_host_stdio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 *  Module state
 * ============================================================ */

/* Legacy output transport hook. If set, canvas output goes through
 * this function. The active VmHostTransport (vm_host_transport.h)
 * takes precedence — the hook remains for hosts that haven't
 * migrated to the full transport interface. */
static VmTuiOutputFn g_out_fn  = NULL;
static void         *g_out_ctx = NULL;

void vm_host_tui_set_output(VmTuiOutputFn fn, void *ctx) {
    g_out_fn  = fn;
    g_out_ctx = ctx;
}

/* hresolve_transport, hflush_stdout_if_default, and hwrite are
 * declared in vm_host_tui_internal.h and defined further down. */

/* ============================================================
 *  Per-VM TUI session state
 *
 *  All per-shell TUI state lives in VmTuiSession. The pool of
 *  sessions is keyed by (vm_id) so multiple shells can each drive
 *  their own independent canvas + input parser.
 *
 *  Why this layout:
 *    - Each session owns a back+front buffer pair (~42 KB each).
 *      Two simultaneous sessions = two canvases on different
 *      terminals; they never interfere.
 *    - Each session owns its own input-parser state machine so
 *      CSI/mouse sequences interleaved across transports stay
 *      coherent.
 *    - Each session owns its raw-mode flag because each transport
 *      may need raw mode independently (one pty may be raw, one
 *      TCP socket is irrelevant, etc.).
 *
 *  What stays global (intentionally):
 *    - g_out_fn / g_out_ctx           — legacy hook fallback
 *    - g_out_buf / g_out_pos          — write cache (just a buffer
 *                                        between hwrite calls)
 *    - g_tile_arena / g_tile_slots    — already vm-keyed via the
 *                                        slot table; arena bytes
 *                                        are pooled across all VMs
 *
 *  The forward decl `cur_session()` returns the current session
 *  for the calling context: a (vm_id -> session) lookup that
 *  ECALL handlers use instead of touching session state directly.
 * ============================================================ */

/* HostCell + the g_* session-state shims come from
 * vm_host_tui_internal.h, included at the top of this file.
 * The VmTuiSession struct itself lives in vm_host_tui.h (the
 * host needs its size to declare a pool). IN_BUF_CAP /
 * MAX_CSI_PARAMS also come from the public header. */

/* ============================================================
 *  Session pool
 *
 *  The host provides the backing storage for sessions via
 *  vm_host_tui_set_pool(pool, count). This matches the platform's
 *  caller-feeds-memory philosophy (slab, bump, trashdrive all work
 *  this way): the build that knows its RAM budget decides how many
 *  sessions to allow, and where that memory lives.
 *
 *    Dev host:   static VmTuiSession pool[16];   (~1.7 MB, free)
 *    MCU+SDRAM:  pool in external SDRAM section   (16 sessions, cheap)
 *    MCU bare:   static VmTuiSession pool[2];     (~222 KB internal)
 *
 *  If the host never calls set_pool, we fall back to a single
 *  built-in session (g_fallback_session) so single-shell demos
 *  work unchanged.
 *
 *  Session-to-VM binding: each session's owner_vm records which VM
 *  owns it. cur_session() resolves the session for the VM that's
 *  currently executing a TUI ECALL (tracked by g_current_vm, set
 *  at handler entry). A VM's session is allocated lazily on its
 *  first SYS_TUI_INIT and freed at SYS_TUI_SHUTDOWN / VM exit.
 * ============================================================ */

static VmTuiSession  g_fallback_session;
static VmTuiSession *g_pool       = NULL;
static unsigned      g_pool_count = 0;

/* The VM currently executing a TUI ECALL. Set by tui_enter at the
 * top of every handler, cleared by tui_leave on exit. UINT16_MAX
 * when no handler is active — which only happens during the atexit
 * / unload paths, where we use the most-recently-active session.
 *
 * Declared extern in vm_host_tui_internal.h; lives here. */
uint16_t vm_host_tui_g_current_vm = UINT16_MAX;

/* One-time init of a session to its default (cleared) state. */
static void session_reset(VmTuiSession *s) {
    memset(s, 0, sizeof(*s));
    s->owner_vm        = UINT16_MAX;
    s->initialized     = false;
    s->flags           = 0;
    s->rows            = VM_TUI_MAX_ROWS;
    s->cols            = VM_TUI_MAX_COLS;
    s->front_valid     = false;
    s->raw_mode_we_set = false;
    s->pen_fg          = VM_TUI_DEFAULT_COLOR;
    s->pen_bg          = VM_TUI_DEFAULT_COLOR;
    s->pen_attrs       = VM_TUI_ATTR_NONE;
    s->cur_row         = 1;
    s->cur_col         = 1;
    s->clip_r          = 1;
    s->clip_c          = 1;
    s->clip_h          = VM_TUI_MAX_ROWS;
    s->clip_w          = VM_TUI_MAX_COLS;
    /* in_* and csi_* are zeroed by memset, which matches their
     * defaults (IN_STATE_GROUND == 0). */
}

void vm_host_tui_set_pool(VmTuiSession *pool, unsigned count) {
    g_pool       = pool;
    g_pool_count = count;
    for (unsigned i = 0; i < count; i++) session_reset(&pool[i]);
}

/* The fallback session is statically zero-initialized, which
 * means its owner_vm starts at 0 — but 0 is a VALID vm_id, so we
 * must explicitly mark it free (UINT16_MAX) before first use.
 * This flag drives a one-time fixup. */
static bool g_fallback_inited = false;

static void fallback_init_once(void) {
    if (!g_fallback_inited) {
        g_fallback_session.owner_vm = UINT16_MAX;
        g_fallback_inited = true;
    }
}

/* Find the session owned by vm_id, or NULL if none. */
static VmTuiSession *session_find(uint16_t vm_id) {
    if (!g_pool) {
        fallback_init_once();
        return (g_fallback_session.owner_vm == vm_id)
                   ? &g_fallback_session : NULL;
    }
    for (unsigned i = 0; i < g_pool_count; i++) {
        if (g_pool[i].owner_vm == vm_id) return &g_pool[i];
    }
    return NULL;
}

/* Allocate a session for vm_id (or return its existing one).
 * Returns NULL if the pool is full. */
static VmTuiSession *session_alloc(uint16_t vm_id) {
    VmTuiSession *existing = session_find(vm_id);
    if (existing) return existing;

    if (!g_pool) {
        /* Single fallback session. Available only if unowned. */
        if (g_fallback_session.owner_vm == UINT16_MAX) {
            session_reset(&g_fallback_session);
            return &g_fallback_session;
        }
        return NULL;
    }
    for (unsigned i = 0; i < g_pool_count; i++) {
        if (g_pool[i].owner_vm == UINT16_MAX) {
            session_reset(&g_pool[i]);
            return &g_pool[i];
        }
    }
    return NULL;   /* pool full */
}

/* The session for whichever VM is currently in a TUI ECALL.
 * Falls back to the fallback session (or pool slot 0) when no
 * handler context is active or the VM has no session yet — this
 * keeps init-time and teardown-time accesses safe.
 *
 * Declared in vm_host_tui_internal.h; non-static so the split-off
 * input parser / tile subsystem can reach it through the g_* shims. */
VmTuiSession *cur_session(void) {
    if (vm_host_tui_g_current_vm != UINT16_MAX) {
        VmTuiSession *s = session_find(vm_host_tui_g_current_vm);
        if (s) return s;
    }
    /* No active session for the current VM. Return a stable
     * scratch session so accesses don't crash; its owner_vm
     * stays UINT16_MAX so it's never mistaken for a real one. */
    return g_pool ? &g_pool[0] : &g_fallback_session;
}

/* tui_enter/tui_leave are inline in vm_host_tui_internal.h. */

/* ============================================================
 *  Transport-routing helpers (used by every hwrite-callsite
 *  in this file). Defined here, after g_session, so the
 *  forward decls at the top can resolve.
 * ============================================================ */

/* Resolve the transport for the currently-active session: prefer
 * the per-VM binding (looked up via the session's owner_vm) and
 * fall back to the process default. When no session has an owner
 * yet (TUI never initialized), falls back to the default transport
 * so init-time output still works. */
VmHostTransport *hresolve_transport(void) {
    VmTuiSession *s = cur_session();
    if (s->owner_vm != UINT16_MAX) {
        VmHostTransport *t = vm_host_get_transport_for_vm(s->owner_vm);
        if (t) return t;
    }
    return vm_host_get_transport();
}

/* Flush stdout only when we're using it. When a transport (or
 * the legacy hook) is in play, stdout isn't on the output path
 * at all — fflushing it would be a no-op at best and could mix
 * unrelated stdio writes into our canvas frame at worst. */
void hflush_stdout_if_default(void) {
    if (hresolve_transport()) return;
    if (g_out_fn) return;
    fflush(stdout);
}

/* Suppress -Wunused-result on write(). We're best-effort here:
 * a closed terminal means nothing reaches the user anyway. The
 * `fd` argument is the no-transport fallback target (always 1
 * in practice). When a transport is installed, output routes
 * through transport->write regardless of `fd`. */
void hwrite(int fd, const void *p, size_t n) {
    VmHostTransport *t = hresolve_transport();
    if (t && t->write) {
        (void)t->write(t, p, (unsigned)n);
        return;
    }
    if (g_out_fn) {
        (void)g_out_fn(p, n, g_out_ctx);
        return;
    }
    ssize_t r = write(fd, p, n);
    (void)r;
}

/* Forward decl for atexit hook + raw mode reset. */
static void do_shutdown(void);
static void tui_atexit_shutdown_all(void);

/* g_* session-state shims live in vm_host_tui_internal.h so the
 * split-off input parser and tile subsystem share one definition.
 * They resolve through cur_session(), which returns the session
 * owned by the VM currently in a TUI ECALL.
 *
 * Care: any local named after a shim macro silently rewrites.
 * Checked at refactor time; no collisions. */

/* ============================================================
 *  Internal: bounds + drawable check
 * ============================================================ */

/* in_canvas / drawable are non-static (declared in
 * vm_host_tui_internal.h) so the split-off tile module can use
 * them for blit/grab bounds checks. in_clip stays static — only
 * draw paths inside this file consult the clip rect. */
bool in_canvas(int row, int col) {
    return row >= 1 && row <= g_rows && col >= 1 && col <= g_cols;
}

static bool in_clip(int row, int col) {
    return row >= g_clip_r && row < g_clip_r + g_clip_h &&
           col >= g_clip_c && col < g_clip_c + g_clip_w;
}

bool drawable(int row, int col) {
    return in_canvas(row, col) && in_clip(row, col);
}

/* ============================================================
 *  Internal: cell ops
 * ============================================================ */

static HostCell make_cell(char ch, uint16_t fg, uint16_t bg, uint8_t attrs) {
    HostCell c;
    c.c = ch;
    c.fg = fg;
    c.bg = bg;
    c.attrs = attrs;
    c.flags = 0;
    return c;
}

static void set_cell(int row, int col, HostCell cell) {
    if (!drawable(row, col)) return;
    g_canvas[row - 1][col - 1] = cell;
}

/* Fill the entire canvas with default blanks. */
static void canvas_clear(void) {
    HostCell blank = make_cell(' ', VM_TUI_DEFAULT_COLOR,
                                VM_TUI_DEFAULT_COLOR, VM_TUI_ATTR_NONE);
    for (int r = 0; r < g_rows; r++) {
        for (int c = 0; c < g_cols; c++) {
            g_canvas[r][c] = blank;
        }
    }
}

/* ============================================================
 *  Lifecycle
 * ============================================================ */

static int do_init(uint16_t vm_id, int rows, int cols, unsigned flags) {
    /* Each VM gets its OWN session. Allocate (or find) this VM's
     * session and make it current before touching any shim-macro
     * state. Multiple VMs can each own a session simultaneously,
     * one per transport; the only refusal here is pool exhaustion. */
    tui_enter(vm_id);
    VmTuiSession *s = session_alloc(vm_id);
    if (!s) {
        /* Pool full — no free session for this VM. */
        tui_leave();
        return -VM_EBUSY;
    }
    s->owner_vm = vm_id;

    /* Register an atexit hook on the FIRST init we ever do.
     * If the host process exits while a guest still owns a
     * canvas (Ctrl-C, fatal error, etc.), this restores the
     * terminal so the parent shell doesn't inherit alt-screen,
     * raw mode, mouse reporting, or hidden cursor.
     *
     * The atexit hook shuts down ALL active sessions, not just the
     * "current" one — at process exit there may be several, and
     * g_current_vm is meaningless. */
    static bool atexit_registered = false;
    if (!atexit_registered) {
        atexit(tui_atexit_shutdown_all);
        atexit_registered = true;
    }

    if (rows <= 0 || rows > VM_TUI_MAX_ROWS) rows = VM_TUI_MAX_ROWS;
    if (cols <= 0 || cols > VM_TUI_MAX_COLS) cols = VM_TUI_MAX_COLS;

    g_owner_vm    = vm_id;
    g_initialized = true;
    g_flags       = flags;
    g_rows        = rows;
    g_cols        = cols;
    g_pen_fg      = VM_TUI_DEFAULT_COLOR;
    g_pen_bg      = VM_TUI_DEFAULT_COLOR;
    g_pen_attrs   = VM_TUI_ATTR_NONE;
    g_cur_row     = 1;
    g_cur_col     = 1;
    g_clip_r = 1; g_clip_c = 1;
    g_clip_h = rows; g_clip_w = cols;
    g_front_valid = false;

    canvas_clear();
    memset(g_front, 0, sizeof(g_front));

    /* Reset input parser state (declared in vm_host_tui_internal.h). */
    vm_host_tui_input_reset_();

    /* Terminal setup. Each flag is best-effort: if writes fail
     * (closed terminal), we just ignore. */
    if (flags & VM_TUI_USE_ALT_SCREEN) {
        const char *s = "\x1b[?1049h";
        hwrite(1, s, 8);
    }
    if (flags & VM_TUI_HIDE_CURSOR) {
        const char *s = "\x1b[?25l";
        hwrite(1, s, 6);
    }
    if (flags & (VM_TUI_USE_MOUSE | VM_TUI_USE_MOUSE_MOTION)) {
        /* SGR encoding (1006) always. Tracking mode depends on the
         * flag: any-motion (1003) when the UI follows the bare
         * cursor, button-motion (1002) otherwise. 1003 reports every
         * pointer move; 1002 only reports motion while a button is
         * held — which is wrong for a "move to steer" game and is
         * what made car.elf appear unresponsive (and leak stray
         * reports as text) under PuTTY. */
        if (flags & VM_TUI_USE_MOUSE_MOTION) {
            const char *s = "\x1b[?1003h\x1b[?1006h";
            hwrite(1, s, 16);
        } else {
            const char *s = "\x1b[?1002h\x1b[?1006h";
            hwrite(1, s, 16);
        }
    }
    /* TUI_USE_RAW: switch the controlling tty into raw mode so
     * keystrokes and mouse events reach us byte-by-byte instead
     * of being line-buffered and echoed back. The shell host
     * usually already has raw mode on, but a spawned game can't
     * count on that — and if we entered alt-screen with cooked
     * mode, the user's keypresses would echo onto the screen
     * over our rendering.
     *
     * Prefer this SESSION's transport->set_raw so that two
     * simultaneous TUI sessions on different transports each
     * manipulate their own line discipline independently. Falls
     * back to the legacy global stdio raw-mode toggle when no
     * session-bound transport supports set_raw. */
    if (flags & VM_TUI_USE_RAW) {
        VmHostTransport *t = hresolve_transport();
        if (t && t->set_raw) {
            g_raw_mode_we_set = (t->set_raw(t, true) >= 0);
        } else {
            g_raw_mode_we_set = vm_host_stdio_set_raw_mode(true);
        }
    } else {
        g_raw_mode_we_set = false;
    }

    return 0;
}

static void do_shutdown(void) {
    if (!g_initialized) return;

    /* Restore terminal state in reverse order. Disable SGR (1006)
     * plus BOTH tracking modes — we don't track which was on, and
     * disabling an inactive mode is harmless. */
    if (g_flags & (VM_TUI_USE_MOUSE | VM_TUI_USE_MOUSE_MOTION)) {
        const char *s = "\x1b[?1006l\x1b[?1003l\x1b[?1002l";
        hwrite(1, s, 24);
    }
    if (g_flags & VM_TUI_HIDE_CURSOR) {
        const char *s = "\x1b[?25h";
        hwrite(1, s, 6);
    }
    if (g_flags & VM_TUI_USE_ALT_SCREEN) {
        const char *s = "\x1b[?1049l";
        hwrite(1, s, 8);
    } else {
        /* Reset SGR and put cursor at the start of a fresh line. */
        const char *s = "\x1b[0m\r\n";
        hwrite(1, s, 6);
    }
    hflush_stdout_if_default();

    /* If we put the tty into raw mode, take it out so the user's
     * shell gets a normal cooked-mode terminal back when we
     * exit. If the caller already had it raw, leave it.
     *
     * Like do_init, prefer the session's transport->set_raw if
     * available so we toggle the same line discipline we toggled
     * on at init. */
    if (g_raw_mode_we_set) {
        VmHostTransport *t = hresolve_transport();
        if (t && t->set_raw) {
            (void)t->set_raw(t, false);
        } else {
            vm_host_stdio_set_raw_mode(false);
        }
        g_raw_mode_we_set = false;
    }

    g_initialized = false;
    g_owner_vm    = UINT16_MAX;
    g_flags       = 0;
}

/* Shut down every active session. Called from atexit at process
 * exit, when several sessions may be live and there's no single
 * "current" VM. Iterates the pool (or the lone fallback session),
 * making each owned session current in turn and tearing it down so
 * its terminal state (alt-screen, raw mode, mouse, cursor) is
 * restored. */
static void tui_atexit_shutdown_all(void) {
    if (!g_pool) {
        if (g_fallback_session.owner_vm != UINT16_MAX) {
            tui_enter(g_fallback_session.owner_vm);
            do_shutdown();
            tui_leave();
        }
        return;
    }
    for (unsigned i = 0; i < g_pool_count; i++) {
        if (g_pool[i].owner_vm != UINT16_MAX) {
            tui_enter(g_pool[i].owner_vm);
            do_shutdown();
            tui_leave();
        }
    }
}

/* ============================================================
 *  Output buffer (we batch writes during present)
 * ============================================================ */

#define OUT_BUF_CAP 4096
static char     g_out_buf[OUT_BUF_CAP];
static unsigned g_out_pos = 0;

static void out_flush(void) {
    if (g_out_pos == 0) return;
    hwrite(1, g_out_buf, g_out_pos);
    g_out_pos = 0;
}

static void out_bytes(const char *p, unsigned n) {
    if (g_out_pos + n <= OUT_BUF_CAP) {
        memcpy(g_out_buf + g_out_pos, p, n);
        g_out_pos += n;
        return;
    }
    out_flush();
    if (n >= OUT_BUF_CAP) {
        hwrite(1, p, n);
        return;
    }
    memcpy(g_out_buf + g_out_pos, p, n);
    g_out_pos += n;
}

static void out_str(const char *s) {
    out_bytes(s, (unsigned)strlen(s));
}

static void out_dec(unsigned v) {
    char buf[12];
    char *p = buf + sizeof(buf);
    if (v == 0) { *--p = '0'; }
    else while (v) { *--p = (char)('0' + (v % 10)); v /= 10; }
    out_bytes(p, (unsigned)((buf + sizeof(buf)) - p));
}

/* ============================================================
 *  SGR / cursor emit
 * ============================================================ */

static void emit_move(int row, int col) {
    out_str("\x1b[");
    out_dec((unsigned)row);
    out_str(";");
    out_dec((unsigned)col);
    out_str("H");
}

/* Emit an SGR sequence resetting + setting fg/bg/attrs. */
static void emit_sgr(uint16_t fg, uint16_t bg, uint8_t attrs) {
    out_str("\x1b[0");   /* reset, then set */
    if (attrs & VM_TUI_ATTR_BOLD)      out_str(";1");
    if (attrs & VM_TUI_ATTR_DIM)       out_str(";2");
    if (attrs & VM_TUI_ATTR_UNDERLINE) out_str(";4");
    if (attrs & VM_TUI_ATTR_REVERSE)   out_str(";7");
    if (fg != VM_TUI_DEFAULT_COLOR) {
        /* 0..7   standard palette (SGR 30..37)
         * 8..15  bright palette   (SGR 90..97)
         * 16..255 xterm 256-color (SGR 38;5;N) */
        if (fg < 8)         { out_str(";3"); out_dec(fg); }
        else if (fg < 16)   { out_str(";9"); out_dec(fg - 8); }
        else if (fg < 256)  { out_str(";38;5;"); out_dec(fg); }
    }
    if (bg != VM_TUI_DEFAULT_COLOR) {
        if (bg < 8)         { out_str(";4"); out_dec(bg); }
        else if (bg < 16)   { out_str(";10"); out_dec(bg - 8); }
        else if (bg < 256)  { out_str(";48;5;"); out_dec(bg); }
    }
    out_str("m");
}

/* ============================================================
 *  Box-drawing glyphs (UTF-8 strings or ASCII chars)
 * ============================================================ */

typedef struct {
    const char *h;  /* horizontal */
    const char *v;  /* vertical */
    const char *tl; /* top-left corner */
    const char *tr;
    const char *bl;
    const char *br;
} BoxStyle;

static const BoxStyle g_box_single = {
    .h = "\xe2\x94\x80",  .v = "\xe2\x94\x82",
    .tl = "\xe2\x94\x8c", .tr = "\xe2\x94\x90",
    .bl = "\xe2\x94\x94", .br = "\xe2\x94\x98",
};

static const BoxStyle g_box_double = {
    .h = "\xe2\x95\x90",  .v = "\xe2\x95\x91",
    .tl = "\xe2\x95\x94", .tr = "\xe2\x95\x97",
    .bl = "\xe2\x95\x9a", .br = "\xe2\x95\x9d",
};

static const BoxStyle g_box_ascii = {
    .h = "-",  .v = "|",
    .tl = "+", .tr = "+",
    .bl = "+", .br = "+",
};

/* For canvas-cell representation: each box character lives in
 * one HostCell. The 'c' field is just the first ASCII byte
 * (single-style box drawing uses '-', '|', '+' on ASCII;
 * non-ASCII glyphs encode their full UTF-8 in a separate
 * mapping at emit time). For T.3 we use a simplification:
 * ASCII box style only fully cell-encodes; single/double
 * style fall back to ASCII at the cell level but emit the
 * Unicode glyph at present time.
 *
 * To keep the cell format compact (single byte for c), we
 * tag the cell's `flags` with a small "box-style" value when
 * the cell came from a box draw. At emit time we look at the
 * flag and pick the appropriate Unicode glyph.
 *
 * Simpler approach for T.3: just use ASCII in the cell, and
 * let the guest call box_single/box_double if they want the
 * fancier output via direct present. To keep this short, we
 * use ASCII-equivalent chars in the canvas — emit doesn't
 * need a special path. We use single-byte ASCII for all
 * box draws; the guest can call BOX with style=2 (ASCII) for
 * 100% fidelity, or accept that single/double become ASCII
 * in this simplified initial version. (Full Unicode box
 * support is on the T.3a backlog.) */
static void apply_box_ascii_only(int row, int col, int h, int w,
                                   const BoxStyle *style) {
    (void)style;   /* T.3: we emit ASCII only at the cell level */
    if (h < 2 || w < 2) return;
    HostCell corner = make_cell('+', g_pen_fg, g_pen_bg, g_pen_attrs);
    HostCell hbar   = make_cell('-', g_pen_fg, g_pen_bg, g_pen_attrs);
    HostCell vbar   = make_cell('|', g_pen_fg, g_pen_bg, g_pen_attrs);
    /* Corners */
    set_cell(row,         col,         corner);
    set_cell(row,         col + w - 1, corner);
    set_cell(row + h - 1, col,         corner);
    set_cell(row + h - 1, col + w - 1, corner);
    /* Edges */
    for (int c = col + 1; c < col + w - 1; c++) {
        set_cell(row,         c, hbar);
        set_cell(row + h - 1, c, hbar);
    }
    for (int r = row + 1; r < row + h - 1; r++) {
        set_cell(r, col,         vbar);
        set_cell(r, col + w - 1, vbar);
    }
}

/* ============================================================
 *  Draw-command parser
 *
 *  Walks the buffer, applying each op to the canvas. Returns
 *  the number of bytes successfully consumed; on a malformed
 *  op the parser stops at that byte and returns -EINVAL.
 * ============================================================ */

/* Read helpers: bounds-checked little-endian decoders.
 * Each returns false if there aren't enough bytes left. */
static bool rd_u8 (const uint8_t **p, const uint8_t *end, uint8_t  *out) {
    if (*p >= end) return false;
    *out = **p; (*p)++;
    return true;
}
static bool rd_u16(const uint8_t **p, const uint8_t *end, uint16_t *out) {
    if (*p + 2 > end) return false;
    *out = (uint16_t)((*p)[0] | ((*p)[1] << 8));
    *p += 2;
    return true;
}

static int32_t parse_draw_commands(const uint8_t *buf, uint32_t len) {
    const uint8_t *p   = buf;
    const uint8_t *end = buf + len;

    while (p < end) {
        uint8_t op;
        if (!rd_u8(&p, end, &op)) return -VM_EINVAL;
        if (op == VM_TUI_OP_END) break;

        switch (op) {
            case VM_TUI_OP_SET_CELL: {
                uint16_t row, col, fg, bg;
                uint8_t  c, attrs;
                if (!rd_u16(&p, end, &row))   return -VM_EINVAL;
                if (!rd_u16(&p, end, &col))   return -VM_EINVAL;
                if (!rd_u8 (&p, end, &c))     return -VM_EINVAL;
                if (!rd_u16(&p, end, &fg))    return -VM_EINVAL;
                if (!rd_u16(&p, end, &bg))    return -VM_EINVAL;
                if (!rd_u8 (&p, end, &attrs)) return -VM_EINVAL;
                set_cell((int)row, (int)col,
                          make_cell((char)c, fg, bg, attrs));
                break;
            }
            case VM_TUI_OP_FILL_RECT: {
                uint16_t row, col, h, w;
                uint8_t  c;
                if (!rd_u16(&p, end, &row)) return -VM_EINVAL;
                if (!rd_u16(&p, end, &col)) return -VM_EINVAL;
                if (!rd_u16(&p, end, &h))   return -VM_EINVAL;
                if (!rd_u16(&p, end, &w))   return -VM_EINVAL;
                if (!rd_u8 (&p, end, &c))   return -VM_EINVAL;
                HostCell cell = make_cell((char)c, g_pen_fg, g_pen_bg, g_pen_attrs);
                for (int r = (int)row; r < (int)row + (int)h; r++) {
                    for (int x = (int)col; x < (int)col + (int)w; x++) {
                        set_cell(r, x, cell);
                    }
                }
                break;
            }
            case VM_TUI_OP_PRINT: {
                uint16_t row, col, fg, bg, slen;
                uint8_t  attrs;
                if (!rd_u16(&p, end, &row))   return -VM_EINVAL;
                if (!rd_u16(&p, end, &col))   return -VM_EINVAL;
                if (!rd_u16(&p, end, &fg))    return -VM_EINVAL;
                if (!rd_u16(&p, end, &bg))    return -VM_EINVAL;
                if (!rd_u8 (&p, end, &attrs)) return -VM_EINVAL;
                if (!rd_u16(&p, end, &slen))  return -VM_EINVAL;
                if (p + slen > end)           return -VM_EINVAL;
                int dest_col = (int)col;
                for (uint16_t i = 0; i < slen; i++) {
                    set_cell((int)row, dest_col + (int)i,
                              make_cell((char)p[i], fg, bg, attrs));
                }
                p += slen;
                break;
            }
            case VM_TUI_OP_BOX: {
                uint16_t row, col, h, w;
                uint8_t  style;
                if (!rd_u16(&p, end, &row))   return -VM_EINVAL;
                if (!rd_u16(&p, end, &col))   return -VM_EINVAL;
                if (!rd_u16(&p, end, &h))     return -VM_EINVAL;
                if (!rd_u16(&p, end, &w))     return -VM_EINVAL;
                if (!rd_u8 (&p, end, &style)) return -VM_EINVAL;
                const BoxStyle *s = &g_box_ascii;
                if (style == 0) s = &g_box_single;
                else if (style == 1) s = &g_box_double;
                apply_box_ascii_only((int)row, (int)col, (int)h, (int)w, s);
                break;
            }
            case VM_TUI_OP_MOVE: {
                uint16_t row, col;
                if (!rd_u16(&p, end, &row)) return -VM_EINVAL;
                if (!rd_u16(&p, end, &col)) return -VM_EINVAL;
                g_cur_row = (int)row;
                g_cur_col = (int)col;
                break;
            }
            case VM_TUI_OP_SET_FG: {
                uint16_t v;
                if (!rd_u16(&p, end, &v)) return -VM_EINVAL;
                g_pen_fg = v;
                break;
            }
            case VM_TUI_OP_SET_BG: {
                uint16_t v;
                if (!rd_u16(&p, end, &v)) return -VM_EINVAL;
                g_pen_bg = v;
                break;
            }
            case VM_TUI_OP_SET_ATTR: {
                uint8_t v;
                if (!rd_u8(&p, end, &v)) return -VM_EINVAL;
                g_pen_attrs = v;
                break;
            }
            case VM_TUI_OP_CLEAR:
                canvas_clear();
                break;
            case VM_TUI_OP_SET_CLIP: {
                uint16_t row, col, h, w;
                if (!rd_u16(&p, end, &row)) return -VM_EINVAL;
                if (!rd_u16(&p, end, &col)) return -VM_EINVAL;
                if (!rd_u16(&p, end, &h))   return -VM_EINVAL;
                if (!rd_u16(&p, end, &w))   return -VM_EINVAL;
                g_clip_r = (int)row; g_clip_c = (int)col;
                g_clip_h = (int)h;   g_clip_w = (int)w;
                break;
            }
            case VM_TUI_OP_CLEAR_CLIP:
                g_clip_r = 1; g_clip_c = 1;
                g_clip_h = g_rows; g_clip_w = g_cols;
                break;
            case VM_TUI_OP_PUTC: {
                uint8_t c;
                if (!rd_u8(&p, end, &c)) return -VM_EINVAL;
                set_cell(g_cur_row, g_cur_col,
                          make_cell((char)c, g_pen_fg, g_pen_bg, g_pen_attrs));
                g_cur_col++;
                break;
            }
            case VM_TUI_OP_PUTS: {
                uint16_t slen;
                if (!rd_u16(&p, end, &slen)) return -VM_EINVAL;
                if (p + slen > end)          return -VM_EINVAL;
                for (uint16_t i = 0; i < slen; i++) {
                    set_cell(g_cur_row, g_cur_col + (int)i,
                              make_cell((char)p[i], g_pen_fg, g_pen_bg, g_pen_attrs));
                }
                g_cur_col += (int)slen;
                p += slen;
                break;
            }
            default:
                return -VM_EINVAL;
        }
    }
    return (int32_t)(p - buf);
}

/* ============================================================
 *  Present + present_diff
 * ============================================================ */

/* Unicode glyph table.
 *
 * Cell `c` values in 0x00..0x7F are emitted as plain ASCII.
 * Values in 0x80..0xBF are indexed into this table and emitted
 * as their UTF-8 byte sequences. 0xC0..0xFF are reserved for
 * future expansion; today they emit as '?' fallback.
 *
 * This lets guests render box-drawing, half-blocks, and
 * geometric shapes without expanding the cell beyond 1 byte. */

typedef struct {
    const char *utf8;
    uint8_t     len;
} GlyphEntry;

static const GlyphEntry g_glyphs[64] = {
    /* 0x80 */ { "\xe2\x96\x88", 3 },  /* █  full block */
    /* 0x81 */ { "\xe2\x96\x80", 3 },  /* ▀  upper half block */
    /* 0x82 */ { "\xe2\x96\x84", 3 },  /* ▄  lower half block */
    /* 0x83 */ { "\xe2\x96\x8c", 3 },  /* ▌  left half block */
    /* 0x84 */ { "\xe2\x96\x90", 3 },  /* ▐  right half block */
    /* 0x85 */ { "\xe2\x96\x91", 3 },  /* ░  light shade */
    /* 0x86 */ { "\xe2\x96\x92", 3 },  /* ▒  medium shade */
    /* 0x87 */ { "\xe2\x96\x93", 3 },  /* ▓  dark shade */
    /* 0x88 */ { "\xe2\x97\x8f", 3 },  /* ●  bullet */
    /* 0x89 */ { "\xe2\x96\xb2", 3 },  /* ▲  up triangle */
    /* 0x8a */ { "\xe2\x96\xbc", 3 },  /* ▼  down triangle */
    /* 0x8b */ { "\xe2\x97\x86", 3 },  /* ◆  diamond */
    /* 0x8c */ { "\xe2\x97\x86", 3 },  /* (reserved, fallback to diamond) */
    /* 0x8d */ { "\xe2\x97\x86", 3 },
    /* 0x8e */ { "\xe2\x97\x86", 3 },
    /* 0x8f */ { "\xe2\x97\x86", 3 },
    /* 0x90 */ { "\xe2\x95\x90", 3 },  /* ═  double horizontal */
    /* 0x91 */ { "\xe2\x95\x91", 3 },  /* ║  double vertical */
    /* 0x92 */ { "\xe2\x97\x8b", 3 },  /* ○  open bullet */
    /* 0x93 */ { "\xe2\x96\xa0", 3 },  /* ■  filled square */
    /* 0x94 */ { "\xe2\x96\xa1", 3 },  /* □  empty square */
    /* 0x95 */ { "\xe2\x97\x80", 3 },  /* ◀  left triangle */
    /* 0x96 */ { "\xe2\x96\xb6", 3 },  /* ▶  right triangle */
    /* 0x97..0xbf: reserved, emit as '?' */
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
    { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 }, { 0, 0 },
};

/* Emit one cell's character: plain byte if < 0x80, glyph
 * lookup if in 0x80..0xBF, '?' fallback otherwise. */
static void emit_cell_char(char c) {
    unsigned char uc = (unsigned char)c;
    if (uc < 0x80) {
        out_bytes(&c, 1);
        return;
    }
    if (uc >= 0x80 && uc < 0xC0) {
        const GlyphEntry *g = &g_glyphs[uc - 0x80];
        if (g->utf8 && g->len) {
            out_bytes(g->utf8, g->len);
            return;
        }
    }
    /* Fallback. */
    const char qmark = '?';
    out_bytes(&qmark, 1);
}

static bool cells_equal(HostCell a, HostCell b) {
    return a.c == b.c && a.fg == b.fg && a.bg == b.bg && a.attrs == b.attrs;
}

static void emit_full_row(int r) {
    emit_move(r, 1);
    uint16_t cur_fg = 0xFFFF, cur_bg = 0xFFFF;
    uint8_t  cur_attrs = 0xFFu;
    for (int c = 0; c < g_cols; c++) {
        HostCell tc = g_canvas[r - 1][c];
        if (tc.fg != cur_fg || tc.bg != cur_bg || tc.attrs != cur_attrs) {
            emit_sgr(tc.fg, tc.bg, tc.attrs);
            cur_fg = tc.fg; cur_bg = tc.bg; cur_attrs = tc.attrs;
        }
        emit_cell_char(tc.c);
    }
}

static void emit_diff_row(int r) {
    uint16_t cur_fg = 0xFFFF, cur_bg = 0xFFFF;
    uint8_t  cur_attrs = 0xFFu;
    bool cursor_placed = false;
    int  last_col_emitted = -2;
    for (int c = 0; c < g_cols; c++) {
        HostCell back = g_canvas[r - 1][c];
        HostCell fr   = g_front [r - 1][c];
        if (cells_equal(back, fr)) continue;
        if (!cursor_placed || c != last_col_emitted + 1) {
            emit_move(r, c + 1);
            cursor_placed = true;
            cur_fg = 0xFFFF;
        }
        if (back.fg != cur_fg || back.bg != cur_bg || back.attrs != cur_attrs) {
            emit_sgr(back.fg, back.bg, back.attrs);
            cur_fg = back.fg; cur_bg = back.bg; cur_attrs = back.attrs;
        }
        emit_cell_char(back.c);
        last_col_emitted = c;
    }
}

static void copy_back_to_front(void) {
    memcpy(g_front, g_canvas, sizeof(g_front));
    g_front_valid = true;
}

static void do_present(void) {
    if (g_flags & VM_TUI_USE_SYNC_OUTPUT) out_str("\x1b[?2026h");
    emit_move(1, 1);
    for (int r = 1; r <= g_rows; r++) emit_full_row(r);
    out_str("\x1b[0m");
    if (g_flags & VM_TUI_USE_SYNC_OUTPUT) out_str("\x1b[?2026l");
    out_flush();
    hflush_stdout_if_default();
    copy_back_to_front();
}

static void do_present_diff(void) {
    if (!g_front_valid) { do_present(); return; }
    if (g_flags & VM_TUI_USE_SYNC_OUTPUT) out_str("\x1b[?2026h");
    for (int r = 1; r <= g_rows; r++) emit_diff_row(r);
    out_str("\x1b[0m");
    if (g_flags & VM_TUI_USE_SYNC_OUTPUT) out_str("\x1b[?2026l");
    out_flush();
    hflush_stdout_if_default();
    copy_back_to_front();
}

/* Input parser (state machine + ring drain) now lives in
 * vm_host_tui_input.c. Shared via vm_host_tui_internal.h. */

/* Tile subsystem (storage + tile_create/destroy/blit/grab/etc
 * primitives) now lives in vm_host_tui_tile.c. */

/* ============================================================
 *  ECALL handlers
 * ============================================================ */

static void handle_tui_init(VmCpu *cpu, void *system) {
    (void)system;
    tui_enter(cpu->vm_id);
    int rows = (int)cpu->regs[VM_REG_A0];
    int cols = (int)cpu->regs[VM_REG_A1];
    unsigned flags = cpu->regs[VM_REG_A2];
    int r = do_init(cpu->vm_id, rows, cols, flags);
    cpu->regs[VM_REG_A0] = (uint32_t)r;
}

static void handle_tui_shutdown(VmCpu *cpu, void *system) {
    (void)system;
    tui_enter(cpu->vm_id);
    /* Only owner can shut down. Non-owners no-op succeed. */
    if (g_owner_vm == cpu->vm_id) do_shutdown();
    cpu->regs[VM_REG_A0] = 0;
}

static void handle_tui_get_dims(VmCpu *cpu, void *system) {
    (void)system;
    tui_enter(cpu->vm_id);
    if (!g_initialized) { cpu->regs[VM_REG_A0] = 0; return; }
    cpu->regs[VM_REG_A0] = (uint32_t)((g_rows << 16) | g_cols);
}

static void handle_tui_present(VmCpu *cpu, void *system) {
    (void)system;
    tui_enter(cpu->vm_id);
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    do_present();
    cpu->regs[VM_REG_A0] = 0;
}

static void handle_tui_present_diff(VmCpu *cpu, void *system) {
    (void)system;
    tui_enter(cpu->vm_id);
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    do_present_diff();
    cpu->regs[VM_REG_A0] = 0;
}

static void handle_tui_poll_event(VmCpu *cpu, void *system) {
    (void)system;
    tui_enter(cpu->vm_id);
    uint32_t out_addr = cpu->regs[VM_REG_A0];

    if (g_owner_vm != cpu->vm_id) {
        /* Non-owners see no events. We don't return -EBUSY here
         * because games typically poll in a tight loop; returning
         * 0 means "no event right now" which is the natural
         * behavior for a non-owner anyway. */
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    InEvent ev = {0};
    if (!vm_host_tui_poll_input(&ev)) {
        cpu->regs[VM_REG_A0] = 0;
        return;
    }

    /* Write the event into the guest's buffer. */
    VmTuiEventRecord *rec = (VmTuiEventRecord *)vm_translate_write(
        cpu, out_addr, sizeof(VmTuiEventRecord));
    if (!rec) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
        return;
    }
    vm_host_tui_marshal_event(&ev, rec);
    cpu->regs[VM_REG_A0] = 1;
}

static void handle_tui_flush_draw(VmCpu *cpu, void *system) {
    (void)system;
    tui_enter(cpu->vm_id);
    uint32_t buf_addr = cpu->regs[VM_REG_A0];
    uint32_t buf_len  = cpu->regs[VM_REG_A1];

    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    if (buf_len == 0) { cpu->regs[VM_REG_A0] = 0; return; }
    if (buf_len > VM_TUI_MAX_FLUSH_BYTES) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EINVAL;
        return;
    }

    /* Pull the entire buffer into a host-side copy so the parser
     * doesn't have to re-translate per-byte. */
    const uint8_t *guest = (const uint8_t *)vm_translate_read(
        cpu, buf_addr, buf_len);
    if (!guest) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EFAULT;
        return;
    }

    static uint8_t scratch[VM_TUI_MAX_FLUSH_BYTES];
    memcpy(scratch, guest, buf_len);

    int32_t r = parse_draw_commands(scratch, buf_len);
    if (r < 0) cpu->regs[VM_REG_A0] = (uint32_t)r;
    else       cpu->regs[VM_REG_A0] = 0;
}

/* SYS_TUI_TILE_* ECALL handlers now live in vm_host_tui_tile.c
 * alongside the primitives. Registered via
 * vm_host_tui_install_tile_handlers, called from the install
 * chain below. */

/* ============================================================
 *  Installation
 * ============================================================ */

/* Adapter: VmSystem unload hooks take (vm_id, userdata). The
 * TUI release function takes only vm_id, so we wrap it. */
static void tui_unload_adapter(uint16_t vm_id, void *userdata) {
    (void)userdata;
    vm_host_tui_release_for_vm(vm_id);
}

bool vm_host_install_tui(VmSystem *sys) {
    if (!sys || !sys->ecall_router) return false;

    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_INIT,
                           handle_tui_init)) goto fail;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_SHUTDOWN,
                           handle_tui_shutdown)) goto fail_init;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_GET_DIMS,
                           handle_tui_get_dims)) goto fail_shutdown;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_PRESENT,
                           handle_tui_present)) goto fail_dims;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_PRESENT_DIFF,
                           handle_tui_present_diff)) goto fail_present;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_POLL_EVENT,
                           handle_tui_poll_event)) goto fail_diff;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_FLUSH_DRAW,
                           handle_tui_flush_draw)) goto fail_poll;

    /* Tile handlers live in vm_host_tui_tile.c. They have their own
     * 8-step register/unregister chain inside that file. */
    if (!vm_host_tui_install_tile_handlers(sys)) goto fail_flush;

    /* Register the auto-release hook so a guest that exits without
     * calling SYS_TUI_SHUTDOWN doesn't permanently lock the canvas
     * and so tiles get reclaimed automatically. */
    vm_system_register_unload_hook(sys, tui_unload_adapter, NULL);

    return true;

fail_flush:    vm_ecall_unregister(sys->ecall_router, SYS_TUI_FLUSH_DRAW);
fail_poll:     vm_ecall_unregister(sys->ecall_router, SYS_TUI_POLL_EVENT);
fail_diff:     vm_ecall_unregister(sys->ecall_router, SYS_TUI_PRESENT_DIFF);
fail_present:  vm_ecall_unregister(sys->ecall_router, SYS_TUI_PRESENT);
fail_dims:     vm_ecall_unregister(sys->ecall_router, SYS_TUI_GET_DIMS);
fail_shutdown: vm_ecall_unregister(sys->ecall_router, SYS_TUI_SHUTDOWN);
fail_init:     vm_ecall_unregister(sys->ecall_router, SYS_TUI_INIT);
fail:          return false;
}

void vm_host_tui_release_for_vm(uint16_t vm_id) {
    vm_host_tui_tile_release_for_vm(vm_id);
    /* Resolve this VM's session before consulting/clearing it. */
    tui_enter(vm_id);
    if (session_find(vm_id) && g_owner_vm == vm_id) {
        do_shutdown();   /* clears owner_vm → frees the session slot */
    }
    tui_leave();
}
