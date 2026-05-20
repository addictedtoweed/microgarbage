/* tui.h — Guest-side terminal UI library for microgarbage.
 *
 * ============================================================
 *  Round L: back-buffered canvas, tiles, grab, mouse, sync-out
 *
 *  Architecture:
 *
 *    All drawing now writes into a static back-buffer "canvas"
 *    of TuiCell. Game code freely overwrites cells, blits tiles,
 *    paints text blocks. Nothing reaches the terminal until
 *    tui_present() is called.
 *
 *    tui_present()      — emit the entire canvas as a smart
 *                         row-by-row pass that batches adjacent
 *                         cells sharing fg/bg/attrs into single
 *                         SGR + run sequences. One cursor move
 *                         per row, not per cell. Optionally
 *                         wrapped in CSI ?2026h/l (synchronized
 *                         output) when TUI_USE_SYNC_OUTPUT is on.
 *
 *    tui_present_diff() — same canvas, but compares each cell
 *                         to a remembered front buffer and only
 *                         emits cells that actually changed.
 *                         Lower output volume; uses 2x canvas
 *                         RAM. Useful for low-bandwidth UART
 *                         deployments. The full and diff paths
 *                         can be mixed: the front buffer is
 *                         updated by either call.
 *
 *  Tiles:
 *
 *    A TuiTile is a small rectangle of TuiCell, allocated from
 *    a tiny static arena. Use them to:
 *      - Pre-render expensive content (border + chrome of a
 *        tetris playfield) once and blit it each frame
 *      - Represent moving sprites (a falling tetris piece)
 *        with per-cell transparency
 *      - Capture a region of the canvas (tui_grab) for later
 *        repositioning (block move)
 *
 *  Coordinates:
 *    1-indexed, ANSI convention. Row 1 is the top line, col 1
 *    is the leftmost column. Out-of-canvas operations are
 *    silently dropped.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_TUI_H
#define MICROGARBAGE_TUI_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* ============================================================
 *  Compile-time tuning
 *
 *  These can be overridden at compile time with -DTUI_MAX_ROWS=...
 *  etc. The defaults target a typical PuTTY window comfortably
 *  without exhausting guest RAM. A guest can also pass smaller
 *  dimensions to tui_init() to operate on a subset of the canvas
 *  for better performance.
 * ============================================================ */

#ifndef TUI_MAX_ROWS
#define TUI_MAX_ROWS  30
#endif
#ifndef TUI_MAX_COLS
#define TUI_MAX_COLS  80
#endif

#ifndef TUI_TILE_ARENA_BYTES
/* Total bytes available for all tiles combined. A tetris piece
 * is 4×4 = 16 cells × 6 bytes = 96 bytes; a window chrome tile
 * might be 24×12 = 288 cells × 6 bytes = 1.7 KB. 8 KB holds
 * several such tiles. Override for tile-heavy programs. */
#define TUI_TILE_ARENA_BYTES  8192
#endif

#ifndef TUI_TILE_MAX_COUNT
/* Number of distinct tile handles. */
#define TUI_TILE_MAX_COUNT  32
#endif

/* ============================================================
 *  Init flags
 * ============================================================ */

typedef enum {
    /* Switch to the terminal's alternate screen buffer at init,
     * back to the main buffer at shutdown. Preserves the user's
     * scrollback. Recommended for any full-screen TUI. */
    TUI_USE_ALT_SCREEN      = (1u << 0),

    /* Put the host's terminal into raw mode for the duration.
     * Disables echo, line buffering, and signal generation. In
     * pipe mode this is effectively no-op (the pipe is already
     * byte-at-a-time) but signals intent. */
    TUI_USE_RAW             = (1u << 1),

    /* Hide the terminal cursor at init, show again at shutdown.
     * Most game/menu UIs want this off; line editors want it on. */
    TUI_HIDE_CURSOR         = (1u << 2),

    /* Enable SGR mouse reporting (modes 1006 + 1002):
     *   - press / release of left, middle, right
     *   - motion while a button is held (drag)
     *   - scroll wheel up/down
     * Events arrive via TUI_EV_MOUSE. */
    TUI_USE_MOUSE           = (1u << 3),

    /* Wrap tui_present() output in CSI ?2026h / CSI ?2026l so
     * the terminal doesn't repaint mid-frame. Eliminates tearing
     * on terminals that support DEC synchronized output (Windows
     * Terminal 1.16+, kitty, WezTerm, foot, recent xterm).
     * Silent no-op on terminals that don't. */
    TUI_USE_SYNC_OUTPUT     = (1u << 4),

    /* (Reserved for future round) Enable bracketed paste mode. */
    TUI_USE_BRACKETED_PASTE = (1u << 5),
} TuiInitFlags;

/* ============================================================
 *  Colors and attributes
 * ============================================================ */

/* Basic 16-color palette. Maps to ANSI codes 30-37 / 90-97. */
typedef enum {
    TUI_BLACK = 0, TUI_RED, TUI_GREEN, TUI_YELLOW,
    TUI_BLUE, TUI_MAGENTA, TUI_CYAN, TUI_WHITE,
    TUI_BRIGHT_BLACK, TUI_BRIGHT_RED, TUI_BRIGHT_GREEN,
    TUI_BRIGHT_YELLOW, TUI_BRIGHT_BLUE, TUI_BRIGHT_MAGENTA,
    TUI_BRIGHT_CYAN, TUI_BRIGHT_WHITE,
    /* Sentinel: "use the terminal's default" — don't override. */
    TUI_DEFAULT_COLOR = 256
} TuiColor;

typedef enum {
    TUI_ATTR_NONE      = 0,
    TUI_ATTR_BOLD      = (1u << 0),
    TUI_ATTR_DIM       = (1u << 1),
    TUI_ATTR_UNDERLINE = (1u << 2),
    TUI_ATTR_REVERSE   = (1u << 3),
} TuiAttr;

/* ============================================================
 *  Cell
 *
 *  The unit of canvas / tile storage. Six bytes per cell, packed
 *  for compactness — a max-size canvas (132 × 50 = 6600 cells)
 *  fits in ~40 KB.
 *
 *  The `flags` byte holds bookkeeping bits:
 *
 *    TUI_CELL_TRANSPARENT — when used in a tile, means "leave
 *                           the underlying canvas cell alone
 *                           during blit". Has no meaning on a
 *                           canvas cell.
 *    (Other bits reserved for future use: wide-character
 *    continuation, sixel/pixel cells, etc.)
 * ============================================================ */

#define TUI_CELL_TRANSPARENT  (1u << 0)

typedef struct {
    char     c;        /* ASCII or first byte of UTF-8. Multi-byte
                        * support is via packing the codepoint
                        * into c + extending the cell — not done
                        * in round L; box-drawing helpers emit
                        * UTF-8 directly during present. */
    uint16_t fg;       /* TuiColor value (0-15 or DEFAULT_COLOR) */
    uint16_t bg;
    uint8_t  attrs;    /* TuiAttr bitmask */
    uint8_t  flags;    /* TUI_CELL_* bitmask */
} TuiCell;

/* ============================================================
 *  Events
 * ============================================================ */

/* Symbolic keys above the ASCII range (>= 256). */
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
    TUI_KEY_F1, TUI_KEY_F2, TUI_KEY_F3,  TUI_KEY_F4,
    TUI_KEY_F5, TUI_KEY_F6, TUI_KEY_F7,  TUI_KEY_F8,
    TUI_KEY_F9, TUI_KEY_F10, TUI_KEY_F11, TUI_KEY_F12,
};

enum {
    TUI_MOD_NONE  = 0,
    TUI_MOD_SHIFT = (1u << 0),
    TUI_MOD_ALT   = (1u << 1),
    TUI_MOD_CTRL  = (1u << 2),
};

/* Mouse buttons. The "none" value is used for motion-with-no-
 * button (a "hover" report — only emitted if mode 1003 is on,
 * which we don't currently enable). */
enum {
    TUI_MB_NONE   = 0,
    TUI_MB_LEFT   = 1,
    TUI_MB_MIDDLE = 2,
    TUI_MB_RIGHT  = 3,
    TUI_MB_WHEEL_UP   = 4,
    TUI_MB_WHEEL_DOWN = 5,
};

typedef enum {
    TUI_EV_NONE = 0,
    TUI_EV_KEY,
    TUI_EV_MOUSE,
    /* Reserved for later rounds */
    TUI_EV_PASTE,
    TUI_EV_RESIZE,
} TuiEventKind;

typedef struct {
    /* Decoded key — printable byte, control byte, or TUI_KEY_*. */
    int  key;
    int  mods;
    /* Raw byte that triggered the event (0 for symbolic). */
    char raw;
} TuiKeyEvent;

typedef struct {
    /* 1-indexed terminal coordinates the mouse is over. */
    int  row, col;
    /* TUI_MB_* button identifier. */
    int  button;
    /* Modifier flags (TUI_MOD_*). */
    int  mods;
    /* True if this is a press (or motion-with-button-held);
     * false if it's a release. Wheel events are always "press". */
    bool press;
    /* True if the button is being held (drag). False for clicks. */
    bool drag;
} TuiMouseEvent;

typedef struct {
    TuiEventKind   kind;
    TuiKeyEvent    key;
    TuiMouseEvent  mouse;
} TuiEvent;

/* ============================================================
 *  Lifecycle
 * ============================================================ */

/* Initialize the TUI subsystem with the given flags and active
 * canvas dimensions.
 *
 *   rows, cols — the active canvas size. Must be > 0 and <= the
 *                compile-time TUI_MAX_ROWS / TUI_MAX_COLS. Use 0
 *                for either to get the compile-time max.
 *
 * Returns true on success, false if the dimensions are out of
 * range or some setup step failed. */
bool tui_init(unsigned flags, int rows, int cols);

/* Tear down: matching teardown sequences, restore cursor, leave
 * raw mode. Idempotent. */
void tui_shutdown(void);

/* Get the active canvas dimensions. Useful for code that wants
 * to lay itself out relative to the actual screen. */
int  tui_rows(void);
int  tui_cols(void);

/* ============================================================
 *  Clip rectangle
 *
 *  All drawing primitives are clipped to this region. The wm
 *  in round M sets this to a window's content area before
 *  calling the window's draw callback.
 * ============================================================ */

void tui_set_clip(int row, int col, int h, int w);
void tui_clear_clip(void);     /* full canvas */

/* ============================================================
 *  Canvas access
 *
 *  All drawing in round L writes into a static back-buffer
 *  canvas. The terminal sees nothing until tui_present().
 * ============================================================ */

/* The fundamental drawing primitive: stamp one cell on the
 * canvas. Round L's tui_set_cell ignores the alpha flag on the
 * input (you can't write "transparent" to a canvas — there's no
 * underlying layer); use it on tiles instead. */
void tui_set_cell(int row, int col, char c,
                  TuiColor fg, TuiColor bg, unsigned attrs);

/* Move the (notional) cursor for subsequent tui_putc/tui_puts.
 * In round L this is library-tracked, not terminal-tracked. */
void tui_move(int row, int col);

/* Write one character at the current notional cursor. */
void tui_putc(char c);

/* Write a NUL-terminated string at the current notional cursor.
 * Newlines do NOT advance the row; they're written literally. */
void tui_puts(const char *s);

/* Active color/attribute for the cursor-pen. Subsequent
 * tui_putc, tui_puts, and Unicode box helpers use these. */
void tui_set_fg(TuiColor c);
void tui_set_bg(TuiColor c);
void tui_set_attr(unsigned attrs);
void tui_reset(void);     /* fg=default, bg=default, attrs=NONE */

/* Clear the entire (clipped) canvas region. Uses current bg. */
void tui_clear(void);

/* Fill a rectangle on the canvas with character c, using current
 * fg/bg/attrs. */
void tui_fill_rect(int row, int col, int h, int w, char c);

/* Multi-line text helper. Writes `text` into the rectangle
 * (row, col, h, w), wrapping on '\n' to subsequent rows. Lines
 * longer than `w` are truncated. Empty lines are honored. Uses
 * current fg/bg/attrs. */
void tui_text_block(int row, int col, int h, int w,
                    const char *text);

/* ============================================================
 *  Box drawing
 *
 *  Three styles, same coordinates. Unicode variants are 3 bytes
 *  per glyph (UTF-8); ASCII is 1 byte per glyph (faster on slow
 *  transports, more compatible).
 * ============================================================ */

void tui_box_single(int row, int col, int h, int w);
void tui_box_double(int row, int col, int h, int w);
void tui_box_ascii(int row, int col, int h, int w);

/* ============================================================
 *  Tiles
 *
 *  A TuiTile is a small rectangle of cells with optional
 *  transparency. Allocate from the tile arena, populate, blit
 *  onto the canvas. Tiles are freeable (tui_tile_destroy) but
 *  the arena doesn't truly reclaim space — destroy is a hint
 *  for the slot to be reused but tile arena memory is permanent
 *  until tui_shutdown.
 *
 *  Tile handle is an opaque ID — pass it around by value.
 * ============================================================ */

typedef int TuiTileId;
#define TUI_TILE_NONE  (-1)

/* Create a new tile with the given dimensions. Returns a handle
 * or TUI_TILE_NONE on failure (out of arena or out of slots). */
TuiTileId tui_tile_create(int rows, int cols);

/* Release a tile slot. The arena memory it occupied isn't
 * reclaimed in round L — but the slot ID becomes reusable, so a
 * game that creates and destroys many small tiles still works. */
void tui_tile_destroy(TuiTileId tile);

/* Set a cell within a tile. */
void tui_tile_set(TuiTileId tile, int row, int col,
                  char c, TuiColor fg, TuiColor bg,
                  unsigned attrs);

/* Set a tile cell as transparent. Subsequent blits will not
 * touch the underlying canvas cell at this position. */
void tui_tile_set_transparent(TuiTileId tile, int row, int col);

/* Fill a tile entirely with one character (and current fg/bg/
 * attrs as set via the pen). */
void tui_tile_fill(TuiTileId tile, char c,
                   TuiColor fg, TuiColor bg, unsigned attrs);

/* Blit a tile onto the canvas at (dest_row, dest_col). Respects
 * the canvas clip rect and per-cell transparency. */
void tui_blit_tile(TuiTileId tile, int dest_row, int dest_col);

/* Capture a rectangular region of the canvas into a tile. The
 * destination tile must already exist and be at least as large
 * as (h, w). Out-of-canvas cells become transparent in the
 * destination. */
void tui_grab(int src_row, int src_col, int h, int w,
              TuiTileId dest_tile);

/* ============================================================
 *  Frame control
 * ============================================================ */

/* Emit the entire canvas to the terminal as a smart batched
 * pass: one cursor-move per row, runs of same-attribute cells
 * coalesced into single SGR + content emissions. The previous
 * frame's state (used by tui_present_diff) is updated. */
void tui_present(void);

/* Like tui_present, but compares each canvas cell to the
 * remembered front buffer and only emits cells that differ.
 * Significantly lower byte volume for low-change-rate scenes;
 * uses one extra canvas worth of RAM. */
void tui_present_diff(void);

/* ============================================================
 *  Input
 * ============================================================ */

/* Poll for one input event. Non-blocking. */
bool tui_poll_event(TuiEvent *out);

#endif  /* MICROGARBAGE_TUI_H */
