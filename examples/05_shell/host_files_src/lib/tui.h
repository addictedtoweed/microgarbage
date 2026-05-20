/* tui.h — Guest-side terminal UI library for microgarbage.
 *
 * ============================================================
 *  Layered terminal UI for guest programs
 *
 *  Goals:
 *    - Tier 1: cursor positioning, basic 8/16 colors, attributes,
 *      raw-mode input parsing. Works on any ANSI/VT100 terminal.
 *    - Tier 2: hooks for alt-screen, 24-bit color, mouse,
 *      bracketed paste, synchronized output. Sent blindly when
 *      enabled — modern terminals interpret, older terminals
 *      silently drop. No probing.
 *
 *  Lifecycle:
 *    tui_init(flags)        sets terminal up per flags
 *    ... draw, poll events ...
 *    tui_shutdown()         restores terminal
 *
 *  Drawing:
 *    All drawing is direct-write in round K (no back buffer).
 *    Round L will add a double-buffered canvas with diff/present.
 *    The cell-based API (tui_set_cell) is the primitive every
 *    higher-level helper sits on, so the round L upgrade is
 *    transparent to user code.
 *
 *  Coordinates:
 *    1-indexed, ANSI convention. Row 1 is the top line, col 1 is
 *    the leftmost column. Out-of-clip operations are silently
 *    discarded (no error, no partial write).
 *
 *  Clip rectangle:
 *    All drawing is clipped to a per-process rectangle. Defaults
 *    to the full known terminal area. Set via tui_set_clip;
 *    cleared back to full with tui_clear_clip. Used by the
 *    future window manager to draw into a window's content area
 *    without bleeding into chrome or adjacent windows.
 *
 *  Input:
 *    tui_poll_event(&ev) is non-blocking. Returns false if no
 *    event is ready right now. Game loops poll, render, sleep.
 *    The parser maintains a small state machine across calls,
 *    so partial escape sequences split across multiple reads
 *    are assembled correctly.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_TUI_H
#define MICROGARBAGE_TUI_H

#include <stdbool.h>
#include <stdint.h>

/* ============================================================
 *  Init flags
 * ============================================================ */

typedef enum {
    /* Switch to the terminal's alternate screen buffer at init,
     * back to the main buffer at shutdown. Preserves the user's
     * scrollback. Recommended for any full-screen TUI. */
    TUI_USE_ALT_SCREEN      = (1u << 0),

    /* Put the host's terminal into raw mode for the duration.
     * Disables echo, line buffering, and signal generation on
     * Ctrl-C / Ctrl-Z / Ctrl-\. The bytes still arrive as input
     * events; the program decides what to do with them.
     *
     * In pipe mode the host's terminal-control syscall is a
     * no-op (the pipe is already byte-at-a-time), but this is
     * still the right flag to set — it signals intent. */
    TUI_USE_RAW             = (1u << 1),

    /* Hide the terminal cursor at init, show again at shutdown.
     * Most game/menu UIs want this off; line editors want it on. */
    TUI_HIDE_CURSOR         = (1u << 2),

    /* (Reserved for round L) Enable SGR mouse reporting:
     *   - click and release of left/middle/right buttons
     *   - drag (motion with button held)
     *   - scroll wheel
     * Events arrive via TUI_EV_MOUSE. */
    TUI_USE_MOUSE           = (1u << 3),

    /* (Reserved for round L) Wrap output between tui_present()
     * calls in CSI ?2026h / CSI ?2026l so the terminal doesn't
     * repaint mid-frame. Eliminates tearing on terminals that
     * support DEC synchronized output (Windows Terminal 1.16+,
     * Kitty, WezTerm, foot, recent xterm). Silent no-op
     * elsewhere. */
    TUI_USE_SYNC_OUTPUT     = (1u << 4),

    /* (Reserved for round L) Enable bracketed paste mode. Pasted
     * text arrives as TUI_EV_PASTE events instead of TUI_EV_KEY
     * sequences. Lets a TUI distinguish typed input from paste
     * (e.g., refuse Ctrl-C in pasted text). */
    TUI_USE_BRACKETED_PASTE = (1u << 5),
} TuiInitFlags;

/* ============================================================
 *  Colors and attributes
 * ============================================================ */

/* Basic 16-color palette. Maps to ANSI codes 30-37 (normal) and
 * 90-97 (bright). On 8-color-only terminals the bright variants
 * silently fall back to bold + normal. */
typedef enum {
    TUI_BLACK = 0, TUI_RED, TUI_GREEN, TUI_YELLOW,
    TUI_BLUE, TUI_MAGENTA, TUI_CYAN, TUI_WHITE,
    TUI_BRIGHT_BLACK, TUI_BRIGHT_RED, TUI_BRIGHT_GREEN,
    TUI_BRIGHT_YELLOW, TUI_BRIGHT_BLUE, TUI_BRIGHT_MAGENTA,
    TUI_BRIGHT_CYAN, TUI_BRIGHT_WHITE,
    /* Sentinel: "use the terminal's default". Most terminals
     * draw default-fg as white-ish and default-bg as black-ish
     * but the user's actual scheme may differ (a light theme,
     * a transparent terminal, etc.). When you want to NOT
     * override, use this. */
    TUI_DEFAULT_COLOR = 256
} TuiColor;

typedef enum {
    TUI_ATTR_NONE      = 0,
    TUI_ATTR_BOLD      = (1u << 0),
    TUI_ATTR_DIM       = (1u << 1),
    TUI_ATTR_UNDERLINE = (1u << 2),
    TUI_ATTR_REVERSE   = (1u << 3),
    /* TUI_ATTR_ITALIC, BLINK, STRIKETHROUGH: well-supported but
     * less universally interpreted; reserve for round L if
     * games want them. */
} TuiAttr;

/* ============================================================
 *  Events
 * ============================================================ */

/* Symbolic keys above the ASCII range (>= 256). Below 0x20 are
 * the standard control bytes (Ctrl-A = 1, Ctrl-C = 3, etc.);
 * 0x20-0x7E are printable; 0x7F is backspace on most terminals
 * but distinct from TUI_KEY_BACKSPACE which represents the key
 * whether it sent 0x7F or 0x08. */
enum {
    TUI_KEY_NONE        = 0,
    TUI_KEY_ENTER       = 256,
    TUI_KEY_BACKSPACE,
    TUI_KEY_TAB,
    TUI_KEY_ESCAPE,
    TUI_KEY_UP,
    TUI_KEY_DOWN,
    TUI_KEY_LEFT,
    TUI_KEY_RIGHT,
    TUI_KEY_HOME,
    TUI_KEY_END,
    TUI_KEY_PAGE_UP,
    TUI_KEY_PAGE_DOWN,
    TUI_KEY_INSERT,
    TUI_KEY_DELETE,
    TUI_KEY_F1,
    TUI_KEY_F2,
    TUI_KEY_F3,
    TUI_KEY_F4,
    TUI_KEY_F5,
    TUI_KEY_F6,
    TUI_KEY_F7,
    TUI_KEY_F8,
    TUI_KEY_F9,
    TUI_KEY_F10,
    TUI_KEY_F11,
    TUI_KEY_F12,
};

enum {
    TUI_MOD_NONE  = 0,
    TUI_MOD_SHIFT = (1u << 0),
    TUI_MOD_ALT   = (1u << 1),
    TUI_MOD_CTRL  = (1u << 2),
};

typedef enum {
    TUI_EV_NONE = 0,
    TUI_EV_KEY,
    /* Reserved for round L: */
    TUI_EV_MOUSE,
    TUI_EV_PASTE,
    TUI_EV_RESIZE,
} TuiEventKind;

typedef struct {
    /* The decoded key — either a byte (printable / control) or
     * a TUI_KEY_* symbolic value. */
    int  key;
    /* Modifier flags. Note: most terminals don't report
     * Shift/Ctrl/Alt for printable letters reliably (Ctrl+a
     * arrives as the byte 0x01 with no separate modifier
     * info). For special keys (arrows, F-keys, etc.) modern
     * terminals do report modifiers via CSI 1;5A and friends —
     * the parser handles this where it can. */
    int  mods;
    /* The raw byte that triggered the event, when applicable.
     * 0 for symbolic keys that came from multi-byte escape
     * sequences. Useful for code that wants vi-mode-style raw
     * key handling. */
    char raw;
} TuiKeyEvent;

typedef struct {
    TuiEventKind  kind;
    TuiKeyEvent   key;
    /* Mouse/paste/resize: future rounds. */
} TuiEvent;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Initialize the TUI subsystem with the given flags. Sends the
 * setup escape sequences, sets terminal raw mode if requested,
 * and arms internal state.
 *
 * Returns true on success. On failure (rare — bad flags, write
 * error to stdout) returns false; the terminal state may be
 * partially modified. Best practice: call tui_shutdown() either
 * way to leave the terminal clean. */
bool tui_init(unsigned flags);

/* Tear down: send the matching teardown sequences for whatever
 * was enabled by tui_init, restore the cursor, leave raw mode.
 * Idempotent — safe to call from multiple cleanup paths.
 *
 * If a guest exits without calling this, the host's own cleanup
 * (in main.c) will best-effort restore the cursor and exit
 * raw mode, but app-specific state (alt screen, mouse mode,
 * etc.) will leak. Calling tui_shutdown() explicitly is good
 * practice. */
void tui_shutdown(void);

/* ============================================================
 *  Clip rectangle
 * ============================================================ */

/* Set the clip rectangle. Subsequent drawing primitives are
 * clipped to this region. Coordinates are 1-indexed.
 *
 * Out-of-rect writes are silently dropped — no error code,
 * no partial output. This is what makes window-content drawing
 * trivial: the wm sets the clip to the window's content area
 * before calling the app's draw callback. */
void tui_set_clip(int row, int col, int h, int w);

/* Clear the clip back to "the full known terminal area" (the
 * conservative default of 24 rows × 80 cols if we haven't
 * detected the actual size yet). */
void tui_clear_clip(void);

/* ============================================================
 *  Drawing primitives
 * ============================================================ */

/* Move the cursor to (row, col), 1-indexed. Clipped. */
void tui_move(int row, int col);

/* Write one character at the current cursor. Advances the
 * cursor one cell to the right.
 *
 * Note: the "current cursor" is the terminal's cursor, not a
 * library-tracked position. In round L when the back buffer
 * arrives, the library will track position internally and this
 * comment becomes obsolete. */
void tui_putc(char c);

/* Write a NUL-terminated string at the current cursor position.
 * Embedded newlines do NOT advance the row; they're written
 * literally. To draw multiple lines use multiple tui_move +
 * tui_puts pairs. */
void tui_puts(const char *s);

/* Set foreground / background colors. Uses the basic 16-color
 * palette via classic ANSI codes; round L's 24-bit RGB API
 * comes alongside the back buffer.
 *
 * TUI_DEFAULT_COLOR means "don't override — use whatever the
 * terminal has set." */
void tui_set_fg(TuiColor c);
void tui_set_bg(TuiColor c);

/* Set rendering attributes. Replaces any previously-set ones
 * — pass TUI_ATTR_NONE to clear all. */
void tui_set_attr(unsigned attrs);

/* Reset all colors and attributes to terminal defaults.
 * Equivalent to: tui_set_fg(TUI_DEFAULT_COLOR);
 *                tui_set_bg(TUI_DEFAULT_COLOR);
 *                tui_set_attr(TUI_ATTR_NONE);
 * but emits a single CSI 0 m. */
void tui_reset(void);

/* The fundamental drawing primitive: write one cell at (row, col)
 * with the given character, fg, bg, and attrs.
 *
 * All higher-level helpers sit on top of this. In round K it
 * emits a sequence of cursor-move + attribute + character to
 * stdout. In round L it will write to a back buffer and the
 * actual terminal update happens at tui_present(). */
void tui_set_cell(int row, int col, char c,
                  TuiColor fg, TuiColor bg, unsigned attrs);

/* Clear the entire (clipped) drawing area. Uses current bg
 * color, so set the bg first if you want a colored fill. */
void tui_clear(void);

/* ============================================================
 *  Higher-level helpers
 * ============================================================ */

/* Draw a single-line box (Unicode box-drawing chars) at
 * (row, col) with height h and width w. Border uses current
 * fg/bg/attrs. Interior is NOT cleared — use tui_clear or
 * tui_fill_rect first if you want the contents cleared. */
void tui_box_single(int row, int col, int h, int w);

/* Draw a double-line box at the same coordinates. */
void tui_box_double(int row, int col, int h, int w);

/* Fill a rectangle with character c, using current fg/bg/attrs. */
void tui_fill_rect(int row, int col, int h, int w, char c);

/* ============================================================
 *  Frame control
 * ============================================================ */

/* Mark the end of a frame's drawing. In round K this is just
 * fflush (a no-op on the pipe transport). In round L this is
 * where the back-buffer diff is computed and emitted, optionally
 * wrapped in synchronized-output markers. */
void tui_present(void);

/* ============================================================
 *  Input
 * ============================================================ */

/* Poll for one input event. Non-blocking — returns false if no
 * event is currently available. The parser keeps state across
 * calls, so partial escape sequences from earlier reads are
 * remembered and completed when more bytes arrive.
 *
 * Typical game-loop pattern:
 *
 *     while (running) {
 *         TuiEvent ev;
 *         while (tui_poll_event(&ev)) {
 *             handle(&ev);
 *         }
 *         do_frame();
 *         tui_present();
 *         sys_yield_until_reload();
 *     }
 *
 * Round L will add tui_poll_event_blocking with a timeout for
 * non-game guests that don't have their own tick source. */
bool tui_poll_event(TuiEvent *out);

#endif  /* MICROGARBAGE_TUI_H */
