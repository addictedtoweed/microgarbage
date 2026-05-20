/* tui.c — implementation of the TUI library.
 *
 * Round L: back-buffered canvas with smart row-batched emit,
 * optional diff against front buffer, tiles with transparency,
 * grab (region capture), text block helper, mouse parsing, and
 * synchronized-output support.
 *
 * Memory layout (worst case, with default TUI_MAX_* and tile
 * arena defaults):
 *   - Back canvas:  TUI_MAX_ROWS * TUI_MAX_COLS * 6 = ~40 KB
 *   - Front canvas: ditto = ~40 KB (only used by tui_present_diff)
 *   - Tile arena:   16 KB
 *   - Output buffer: 4 KB
 *   - Input parser:  ~256 bytes
 *   Total: ~100 KB in BSS on the default settings.
 *
 * For smaller targets, override TUI_MAX_ROWS/COLS or
 * TUI_TILE_ARENA_BYTES at compile time.
 *
 * Public domain (CC0).
 */

#include "tui.h"

/* ============================================================
 *  Syscall stubs
 * ============================================================ */

#define SYS_READ              63
#define SYS_WRITE             64
#define SYS_FFLUSH            82
#define SYS_TTY_SET_RAW     1105

static inline int sys_read(int fd, void *buf, unsigned n) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)buf;
    register unsigned a2 asm("a2") = n;
    register int      a7 asm("a7") = SYS_READ;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int sys_write(int fd, const void *buf, unsigned n) {
    register int      a0 asm("a0") = fd;
    register unsigned a1 asm("a1") = (unsigned)(unsigned long)buf;
    register unsigned a2 asm("a2") = n;
    register int      a7 asm("a7") = SYS_WRITE;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static inline int sys_fflush(int fd) {
    register int a0 asm("a0") = fd;
    register int a7 asm("a7") = SYS_FFLUSH;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline int sys_tty_set_raw(int enable) {
    register int a0 asm("a0") = enable;
    register int a7 asm("a7") = SYS_TTY_SET_RAW;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

/* ============================================================
 *  Globals
 * ============================================================ */

static unsigned g_flags = 0;
static bool     g_initialized = false;
static bool     g_raw_active = false;

/* Active canvas dimensions (set at init, <= compile-time max). */
static int g_rows = TUI_MAX_ROWS;
static int g_cols = TUI_MAX_COLS;

/* Pen (current fg/bg/attrs for tui_putc, tui_puts, Unicode helpers,
 * tile_fill defaults). */
static TuiColor g_pen_fg = TUI_DEFAULT_COLOR;
static TuiColor g_pen_bg = TUI_DEFAULT_COLOR;
static unsigned g_pen_attrs = TUI_ATTR_NONE;

/* Notional cursor (1-indexed). The terminal cursor IS this when
 * we present, but between presents we maintain it ourselves. */
static int g_cur_row = 1;
static int g_cur_col = 1;

/* Clip rect (1-indexed). */
static int g_clip_r = 1, g_clip_c = 1, g_clip_h = TUI_MAX_ROWS, g_clip_w = TUI_MAX_COLS;

/* ============================================================
 *  Back / front canvas
 *
 *  Static-allocated, BSS-resident. The default at startup is
 *  zero-initialized — c=0, fg=0 (TUI_BLACK), bg=0, attrs=0.
 *  tui_init() rewrites with default-color blanks so the first
 *  present after init shows a clean screen.
 * ============================================================ */

static TuiCell g_canvas[TUI_MAX_ROWS][TUI_MAX_COLS];
static TuiCell g_front [TUI_MAX_ROWS][TUI_MAX_COLS];
/* g_front_valid: tui_present_diff needs g_front to be initialized
 * with "what the terminal currently shows." We set this to false
 * at init (forcing the first present_diff to behave like full
 * present) and to true after any present call. */
static bool g_front_valid = false;

static TuiCell make_cell(char c, TuiColor fg, TuiColor bg,
                         unsigned attrs, unsigned flags) {
    TuiCell out;
    out.c = c;
    out.fg = (uint16_t)fg;
    out.bg = (uint16_t)bg;
    out.attrs = (uint8_t)attrs;
    out.flags = (uint8_t)flags;
    return out;
}

static TuiCell blank_cell(void) {
    return make_cell(' ', TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR,
                     TUI_ATTR_NONE, 0);
}

static bool cells_equal(TuiCell a, TuiCell b) {
    return a.c == b.c && a.fg == b.fg && a.bg == b.bg &&
           a.attrs == b.attrs;
    /* flags ignored — canvas cells have no meaningful flags */
}

/* ============================================================
 *  Tile arena
 *
 *  A simple bump allocator over a static byte arena, plus a
 *  slot table mapping tile IDs to their cell offsets + dims.
 *  Destroying a tile frees its slot ID for reuse but doesn't
 *  reclaim the underlying bytes; for the use cases we have
 *  (a handful of long-lived tiles per game) this is fine.
 *  Round M or later may swap in a freelist if needed.
 * ============================================================ */

typedef struct {
    int    rows, cols;     /* 0 == slot unused */
    size_t offset;         /* byte offset into the arena */
} TileSlot;

static uint8_t  g_tile_arena[TUI_TILE_ARENA_BYTES];
static size_t   g_tile_arena_used = 0;
static TileSlot g_tile_slots[TUI_TILE_MAX_COUNT];

static TuiCell *tile_cells(TuiTileId tile) {
    if (tile < 0 || tile >= TUI_TILE_MAX_COUNT) return NULL;
    TileSlot *s = &g_tile_slots[tile];
    if (s->rows <= 0 || s->cols <= 0) return NULL;
    return (TuiCell *)(g_tile_arena + s->offset);
}

static TileSlot *tile_slot(TuiTileId tile) {
    if (tile < 0 || tile >= TUI_TILE_MAX_COUNT) return NULL;
    TileSlot *s = &g_tile_slots[tile];
    if (s->rows <= 0 || s->cols <= 0) return NULL;
    return s;
}

TuiTileId tui_tile_create(int rows, int cols) {
    if (rows <= 0 || cols <= 0) return TUI_TILE_NONE;
    size_t bytes = (size_t)rows * (size_t)cols * sizeof(TuiCell);
    if (g_tile_arena_used + bytes > TUI_TILE_ARENA_BYTES) {
        return TUI_TILE_NONE;
    }

    /* Find a free slot. */
    int slot = -1;
    for (int i = 0; i < TUI_TILE_MAX_COUNT; i++) {
        if (g_tile_slots[i].rows == 0) { slot = i; break; }
    }
    if (slot < 0) return TUI_TILE_NONE;

    g_tile_slots[slot].rows = rows;
    g_tile_slots[slot].cols = cols;
    g_tile_slots[slot].offset = g_tile_arena_used;
    g_tile_arena_used += bytes;

    /* Initialize to transparent blank cells. The caller will
     * stamp on top of this; cells they don't touch stay
     * transparent — usually the desired default for sprites. */
    TuiCell *p = tile_cells(slot);
    TuiCell tc = make_cell(' ', TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR,
                           TUI_ATTR_NONE, TUI_CELL_TRANSPARENT);
    for (int i = 0; i < rows * cols; i++) p[i] = tc;
    return slot;
}

void tui_tile_destroy(TuiTileId tile) {
    if (tile < 0 || tile >= TUI_TILE_MAX_COUNT) return;
    /* The arena bytes remain allocated; we just mark the slot
     * unused so future tile_create can reuse the slot ID. */
    g_tile_slots[tile].rows = 0;
    g_tile_slots[tile].cols = 0;
    g_tile_slots[tile].offset = 0;
}

void tui_tile_set(TuiTileId tile, int row, int col,
                  char c, TuiColor fg, TuiColor bg, unsigned attrs) {
    TileSlot *s = tile_slot(tile);
    if (!s) return;
    if (row < 0 || row >= s->rows || col < 0 || col >= s->cols) return;
    TuiCell *p = tile_cells(tile);
    p[row * s->cols + col] = make_cell(c, fg, bg, attrs, 0);
}

void tui_tile_set_transparent(TuiTileId tile, int row, int col) {
    TileSlot *s = tile_slot(tile);
    if (!s) return;
    if (row < 0 || row >= s->rows || col < 0 || col >= s->cols) return;
    TuiCell *p = tile_cells(tile);
    p[row * s->cols + col].flags |= TUI_CELL_TRANSPARENT;
}

void tui_tile_fill(TuiTileId tile, char c, TuiColor fg, TuiColor bg,
                   unsigned attrs) {
    TileSlot *s = tile_slot(tile);
    if (!s) return;
    TuiCell *p = tile_cells(tile);
    TuiCell tc = make_cell(c, fg, bg, attrs, 0);
    int n = s->rows * s->cols;
    for (int i = 0; i < n; i++) p[i] = tc;
}

/* ============================================================
 *  Output buffer (sys_write coalescing)
 * ============================================================ */

#define OUT_BUF_CAP 4096
static char g_out_buf[OUT_BUF_CAP];
static unsigned g_out_pos = 0;

static void out_flush(void) {
    if (g_out_pos == 0) return;
    sys_write(1, g_out_buf, g_out_pos);
    g_out_pos = 0;
}

static void out_byte(char c) {
    if (g_out_pos >= OUT_BUF_CAP) out_flush();
    g_out_buf[g_out_pos++] = c;
}

static void out_bytes(const char *p, unsigned n) {
    if (g_out_pos + n <= OUT_BUF_CAP) {
        for (unsigned i = 0; i < n; i++) g_out_buf[g_out_pos + i] = p[i];
        g_out_pos += n;
        return;
    }
    out_flush();
    if (n >= OUT_BUF_CAP) {
        sys_write(1, p, n);
        return;
    }
    for (unsigned i = 0; i < n; i++) g_out_buf[g_out_pos + i] = p[i];
    g_out_pos += n;
}

static void out_str(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    out_bytes(s, n);
}

static void out_dec(unsigned v) {
    char buf[12];
    char *p = buf + sizeof(buf);
    *--p = '\0';
    if (v == 0) { *--p = '0'; }
    else while (v) { *--p = (char)('0' + (v % 10)); v /= 10; }
    out_str(p);
}

/* ============================================================
 *  Bounds helpers
 * ============================================================ */

static bool in_canvas(int row, int col) {
    return row >= 1 && row <= g_rows && col >= 1 && col <= g_cols;
}

static bool in_clip(int row, int col) {
    return row >= g_clip_r && row < g_clip_r + g_clip_h &&
           col >= g_clip_c && col < g_clip_c + g_clip_w;
}

/* Combined canvas + clip check. */
static bool drawable(int row, int col) {
    return in_canvas(row, col) && in_clip(row, col);
}

/* ============================================================
 *  Public API: lifecycle
 * ============================================================ */

bool tui_init(unsigned flags, int rows, int cols) {
    if (g_initialized) return true;

    if (rows <= 0) rows = TUI_MAX_ROWS;
    if (cols <= 0) cols = TUI_MAX_COLS;
    if (rows > TUI_MAX_ROWS || cols > TUI_MAX_COLS) return false;

    g_flags = flags;
    g_rows = rows;
    g_cols = cols;
    g_pen_fg = TUI_DEFAULT_COLOR;
    g_pen_bg = TUI_DEFAULT_COLOR;
    g_pen_attrs = TUI_ATTR_NONE;
    g_cur_row = 1;
    g_cur_col = 1;
    g_clip_r = 1; g_clip_c = 1;
    g_clip_h = g_rows; g_clip_w = g_cols;

    /* Initialize canvas to default-color blanks. */
    TuiCell b = blank_cell();
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++)
            g_canvas[r][c] = b;
    /* Front buffer: not yet valid. First present_diff will treat
     * the screen as "everything must be drawn." */
    g_front_valid = false;

    /* Tile arena: reset on every init. */
    g_tile_arena_used = 0;
    for (int i = 0; i < TUI_TILE_MAX_COUNT; i++) {
        g_tile_slots[i].rows = 0;
        g_tile_slots[i].cols = 0;
        g_tile_slots[i].offset = 0;
    }

    /* Terminal setup. */
    if (flags & TUI_USE_RAW) {
        int r = sys_tty_set_raw(1);
        g_raw_active = (r == 0);
    }
    if (flags & TUI_USE_ALT_SCREEN) {
        out_str("\x1b[?1049h\x1b[H\x1b[2J");
    }
    if (flags & TUI_HIDE_CURSOR) {
        out_str("\x1b[?25l");
    }
    if (flags & TUI_USE_MOUSE) {
        /* Mode 1006 = SGR-encoded mouse reports (the modern,
         * unambiguous form). Mode 1002 = button-event motion
         * (drag); we use this instead of 1003 (all motion) to
         * keep the event stream manageable. */
        out_str("\x1b[?1006h\x1b[?1002h");
    }
    /* Note: TUI_USE_SYNC_OUTPUT is per-frame (wraps the present
     * emission), not a persistent terminal state. We don't emit
     * anything for it here. */

    out_flush();
    g_initialized = true;
    return true;
}

void tui_shutdown(void) {
    if (!g_initialized) return;

    out_str("\x1b[0m");
    if (g_flags & TUI_USE_MOUSE) {
        out_str("\x1b[?1002l\x1b[?1006l");
    }
    if (g_flags & TUI_HIDE_CURSOR) {
        out_str("\x1b[?25h");
    }
    if (g_flags & TUI_USE_ALT_SCREEN) {
        out_str("\x1b[?1049l");
    }
    out_flush();

    if (g_raw_active) {
        sys_tty_set_raw(0);
        g_raw_active = false;
    }
    g_initialized = false;
    g_flags = 0;
}

int tui_rows(void) { return g_rows; }
int tui_cols(void) { return g_cols; }

/* ============================================================
 *  Clip
 * ============================================================ */

void tui_set_clip(int row, int col, int h, int w) {
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    if (h < 0) h = 0;
    if (w < 0) w = 0;
    if (row + h - 1 > g_rows) h = g_rows - row + 1;
    if (col + w - 1 > g_cols) w = g_cols - col + 1;
    g_clip_r = row;
    g_clip_c = col;
    g_clip_h = h;
    g_clip_w = w;
}

void tui_clear_clip(void) {
    g_clip_r = 1;
    g_clip_c = 1;
    g_clip_h = g_rows;
    g_clip_w = g_cols;
}

/* ============================================================
 *  Pen
 * ============================================================ */

void tui_set_fg(TuiColor c)    { g_pen_fg = c; }
void tui_set_bg(TuiColor c)    { g_pen_bg = c; }
void tui_set_attr(unsigned a)  { g_pen_attrs = a; }
void tui_reset(void) {
    g_pen_fg = TUI_DEFAULT_COLOR;
    g_pen_bg = TUI_DEFAULT_COLOR;
    g_pen_attrs = TUI_ATTR_NONE;
}

/* ============================================================
 *  Drawing — write to canvas
 * ============================================================ */

void tui_set_cell(int row, int col, char c,
                  TuiColor fg, TuiColor bg, unsigned attrs) {
    if (!drawable(row, col)) return;
    g_canvas[row - 1][col - 1] = make_cell(c, fg, bg, attrs, 0);
}

void tui_move(int row, int col) {
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    if (row > g_rows) row = g_rows;
    if (col > g_cols) col = g_cols;
    g_cur_row = row;
    g_cur_col = col;
}

void tui_putc(char c) {
    if (drawable(g_cur_row, g_cur_col)) {
        g_canvas[g_cur_row - 1][g_cur_col - 1] =
            make_cell(c, g_pen_fg, g_pen_bg, g_pen_attrs, 0);
    }
    g_cur_col++;
    /* Don't wrap to next row — let the caller manage line breaks
     * explicitly. */
}

void tui_puts(const char *s) {
    if (!s) return;
    while (*s) tui_putc(*s++);
}

void tui_clear(void) {
    /* Clear the clipped portion of the canvas using current bg. */
    TuiCell tc = make_cell(' ', TUI_DEFAULT_COLOR, g_pen_bg,
                           TUI_ATTR_NONE, 0);
    for (int r = g_clip_r; r < g_clip_r + g_clip_h && r <= g_rows; r++) {
        for (int c = g_clip_c; c < g_clip_c + g_clip_w && c <= g_cols; c++) {
            g_canvas[r - 1][c - 1] = tc;
        }
    }
}

void tui_fill_rect(int row, int col, int h, int w, char c) {
    TuiCell tc = make_cell(c, g_pen_fg, g_pen_bg, g_pen_attrs, 0);
    for (int r = row; r < row + h; r++) {
        for (int x = col; x < col + w; x++) {
            if (drawable(r, x)) g_canvas[r - 1][x - 1] = tc;
        }
    }
}

void tui_text_block(int row, int col, int h, int w,
                    const char *text) {
    if (!text || h <= 0 || w <= 0) return;
    int cur_r = row;
    int cur_c = col;
    while (*text && cur_r < row + h) {
        if (*text == '\n') {
            cur_r++;
            cur_c = col;
            text++;
            continue;
        }
        if (cur_c < col + w && drawable(cur_r, cur_c)) {
            g_canvas[cur_r - 1][cur_c - 1] =
                make_cell(*text, g_pen_fg, g_pen_bg, g_pen_attrs, 0);
        }
        cur_c++;
        text++;
    }
}

/* ============================================================
 *  Box drawing
 *
 *  Unicode helpers stamp UTF-8 bytes into the canvas. Since
 *  TuiCell.c is one byte, we use a workaround: stamp the first
 *  byte as the cell's c and rely on the present pass to emit
 *  the full 3-byte sequence for box-drawing chars. This is a
 *  bit of a hack — it means box cells can't share their fg/bg
 *  storage in cleanly-readable ways — but it preserves the
 *  6-byte cell size.
 *
 *  Implementation: we use a small sentinel byte in c to mean
 *  "this is a Unicode box char N", and a table lookup at present
 *  time. The sentinels are in the C0 range (1-31) so they can't
 *  collide with printable ASCII.
 * ============================================================ */

/* Box sentinels (in c field). 0 = empty/none. */
#define BOX_TL_S  0x01  /* ┌ */
#define BOX_TR_S  0x02  /* ┐ */
#define BOX_BL_S  0x03  /* └ */
#define BOX_BR_S  0x04  /* ┘ */
#define BOX_H_S   0x05  /* ─ */
#define BOX_V_S   0x06  /* │ */
#define BOX_TL_D  0x07  /* ╔ */
#define BOX_TR_D  0x0E  /* ╗ */
#define BOX_BL_D  0x0F  /* ╚ */
#define BOX_BR_D  0x10  /* ╝ */
#define BOX_H_D   0x11  /* ═ */
#define BOX_V_D   0x12  /* ║ */

/* Map sentinel → UTF-8 string. NULL for non-sentinel values. */
static const char *box_glyph(unsigned char b) {
    switch (b) {
        case BOX_TL_S: return "\xE2\x94\x8C";
        case BOX_TR_S: return "\xE2\x94\x90";
        case BOX_BL_S: return "\xE2\x94\x94";
        case BOX_BR_S: return "\xE2\x94\x98";
        case BOX_H_S:  return "\xE2\x94\x80";
        case BOX_V_S:  return "\xE2\x94\x82";
        case BOX_TL_D: return "\xE2\x95\x94";
        case BOX_TR_D: return "\xE2\x95\x97";
        case BOX_BL_D: return "\xE2\x95\x9A";
        case BOX_BR_D: return "\xE2\x95\x9D";
        case BOX_H_D:  return "\xE2\x95\x90";
        case BOX_V_D:  return "\xE2\x95\x91";
        default:       return NULL;
    }
}

static void stamp_box_cell(int row, int col, char sentinel) {
    if (!drawable(row, col)) return;
    g_canvas[row - 1][col - 1] =
        make_cell(sentinel, g_pen_fg, g_pen_bg, g_pen_attrs, 0);
}

static void box_impl(int row, int col, int h, int w,
                     char tl, char tr, char bl, char br,
                     char hh, char v) {
    if (h < 2 || w < 2) return;
    stamp_box_cell(row, col, tl);
    for (int i = 1; i < w - 1; i++) stamp_box_cell(row, col + i, hh);
    stamp_box_cell(row, col + w - 1, tr);
    for (int r = 1; r < h - 1; r++) {
        stamp_box_cell(row + r, col, v);
        stamp_box_cell(row + r, col + w - 1, v);
    }
    stamp_box_cell(row + h - 1, col, bl);
    for (int i = 1; i < w - 1; i++) stamp_box_cell(row + h - 1, col + i, hh);
    stamp_box_cell(row + h - 1, col + w - 1, br);
}

void tui_box_single(int row, int col, int h, int w) {
    box_impl(row, col, h, w,
             BOX_TL_S, BOX_TR_S, BOX_BL_S, BOX_BR_S, BOX_H_S, BOX_V_S);
}

void tui_box_double(int row, int col, int h, int w) {
    box_impl(row, col, h, w,
             BOX_TL_D, BOX_TR_D, BOX_BL_D, BOX_BR_D, BOX_H_D, BOX_V_D);
}

void tui_box_ascii(int row, int col, int h, int w) {
    box_impl(row, col, h, w, '+', '+', '+', '+', '-', '|');
}

/* ============================================================
 *  Tile blit / grab
 * ============================================================ */

void tui_blit_tile(TuiTileId tile, int dest_row, int dest_col) {
    TileSlot *s = tile_slot(tile);
    if (!s) return;
    TuiCell *p = tile_cells(tile);
    for (int r = 0; r < s->rows; r++) {
        for (int c = 0; c < s->cols; c++) {
            TuiCell tc = p[r * s->cols + c];
            if (tc.flags & TUI_CELL_TRANSPARENT) continue;
            int dr = dest_row + r;
            int dc = dest_col + c;
            if (!drawable(dr, dc)) continue;
            /* Clear the flags on canvas write so subsequent
             * operations see a clean cell. */
            tc.flags = 0;
            g_canvas[dr - 1][dc - 1] = tc;
        }
    }
}

void tui_grab(int src_row, int src_col, int h, int w,
              TuiTileId dest_tile) {
    TileSlot *s = tile_slot(dest_tile);
    if (!s) return;
    TuiCell *p = tile_cells(dest_tile);
    /* Clamp h, w to the smaller of (requested, tile size). */
    if (h > s->rows) h = s->rows;
    if (w > s->cols) w = s->cols;
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            int sr = src_row + r;
            int sc = src_col + c;
            if (in_canvas(sr, sc)) {
                TuiCell tc = g_canvas[sr - 1][sc - 1];
                tc.flags = 0;
                p[r * s->cols + c] = tc;
            } else {
                /* Off-canvas cells captured as transparent. */
                p[r * s->cols + c] = make_cell(
                    ' ', TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR,
                    TUI_ATTR_NONE, TUI_CELL_TRANSPARENT);
            }
        }
    }
}

/* ============================================================
 *  Present — emit canvas to terminal
 *
 *  Smart row-by-row pass: for each row, emit one cursor-move
 *  followed by runs of cells sharing fg/bg/attrs as single
 *  SGR + content sequences.
 *
 *  Box-drawing sentinels (cell.c in the C0 sentinel range) are
 *  expanded to their full UTF-8 sequences during emission.
 * ============================================================ */

static int fg_code(TuiColor c) {
    if (c == TUI_DEFAULT_COLOR) return 39;
    if ((unsigned)c <= 7)  return 30 + (int)c;
    if ((unsigned)c <= 15) return 90 + ((int)c - 8);
    return 39;
}
static int bg_code(TuiColor c) {
    if (c == TUI_DEFAULT_COLOR) return 49;
    if ((unsigned)c <= 7)  return 40 + (int)c;
    if ((unsigned)c <= 15) return 100 + ((int)c - 8);
    return 49;
}

/* Emit a full SGR reset + attrs + fg + bg sequence. Resetting
 * first is shorter than tracking which attrs need explicit-off
 * codes; the cost is a few bytes per transition. */
static void emit_sgr(TuiColor fg, TuiColor bg, unsigned attrs) {
    out_str("\x1b[0");
    if (attrs & TUI_ATTR_BOLD)      out_str(";1");
    if (attrs & TUI_ATTR_DIM)       out_str(";2");
    if (attrs & TUI_ATTR_UNDERLINE) out_str(";4");
    if (attrs & TUI_ATTR_REVERSE)   out_str(";7");
    if (fg != TUI_DEFAULT_COLOR) {
        out_byte(';');
        out_dec((unsigned)fg_code(fg));
    }
    if (bg != TUI_DEFAULT_COLOR) {
        out_byte(';');
        out_dec((unsigned)bg_code(bg));
    }
    out_byte('m');
}

static void emit_move(int row, int col) {
    out_str("\x1b[");
    out_dec((unsigned)row);
    out_byte(';');
    out_dec((unsigned)col);
    out_byte('H');
}

static void emit_cell_char(TuiCell tc) {
    const char *gly = box_glyph((unsigned char)tc.c);
    if (gly) {
        out_str(gly);
    } else if ((unsigned char)tc.c < 0x20) {
        /* Non-printable, non-box: emit a space rather than
         * sending a stray control byte to the terminal. */
        out_byte(' ');
    } else {
        out_byte(tc.c);
    }
}

/* Emit the entire row [r] of the canvas as a sequence of
 * batched runs. Bypasses cell-equal-to-front comparison; used by
 * tui_present (full redraw). */
static void emit_full_row(int r) {
    emit_move(r, 1);
    /* Initialize "current attrs" to an obviously-wrong value so
     * the first cell forces an SGR emission. */
    TuiColor cur_fg = (TuiColor)0xFFFF;
    TuiColor cur_bg = (TuiColor)0xFFFF;
    unsigned cur_attrs = 0xFFFFu;

    for (int c = 0; c < g_cols; c++) {
        TuiCell tc = g_canvas[r - 1][c];
        if ((TuiColor)tc.fg != cur_fg ||
            (TuiColor)tc.bg != cur_bg ||
            (unsigned)tc.attrs != cur_attrs) {
            emit_sgr((TuiColor)tc.fg, (TuiColor)tc.bg, tc.attrs);
            cur_fg = (TuiColor)tc.fg;
            cur_bg = (TuiColor)tc.bg;
            cur_attrs = tc.attrs;
        }
        emit_cell_char(tc);
    }
}

/* Emit only the cells in row r that differ from front, batching
 * runs of consecutive different cells together. */
static void emit_diff_row(int r) {
    TuiColor cur_fg = (TuiColor)0xFFFF;
    TuiColor cur_bg = (TuiColor)0xFFFF;
    unsigned cur_attrs = 0xFFFFu;
    bool cursor_placed = false;
    int  last_col_emitted = -2;   /* track contiguity */

    for (int c = 0; c < g_cols; c++) {
        TuiCell back = g_canvas[r - 1][c];
        TuiCell fr   = g_front [r - 1][c];
        if (cells_equal(back, fr)) continue;

        /* Cell differs. Either start a new run (move cursor) or
         * continue the current run (no move needed). */
        if (!cursor_placed || c != last_col_emitted + 1) {
            emit_move(r, c + 1);
            cursor_placed = true;
            /* After a move, the previous SGR state is still in
             * effect but we don't know if this cell shares it,
             * so force-emit on the first cell of each run. */
            cur_fg = (TuiColor)0xFFFF;
        }
        if ((TuiColor)back.fg != cur_fg ||
            (TuiColor)back.bg != cur_bg ||
            (unsigned)back.attrs != cur_attrs) {
            emit_sgr((TuiColor)back.fg, (TuiColor)back.bg, back.attrs);
            cur_fg = (TuiColor)back.fg;
            cur_bg = (TuiColor)back.bg;
            cur_attrs = back.attrs;
        }
        emit_cell_char(back);
        last_col_emitted = c;
    }
}

static void copy_back_to_front(void) {
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++)
            g_front[r][c] = g_canvas[r][c];
    g_front_valid = true;
}

void tui_present(void) {
    if (g_flags & TUI_USE_SYNC_OUTPUT) {
        out_str("\x1b[?2026h");
    }
    /* Home cursor before painting — defensive in case the
     * terminal's cursor moved between presents. */
    emit_move(1, 1);
    for (int r = 1; r <= g_rows; r++) {
        emit_full_row(r);
    }
    out_str("\x1b[0m");
    if (g_flags & TUI_USE_SYNC_OUTPUT) {
        out_str("\x1b[?2026l");
    }
    out_flush();
    sys_fflush(1);
    copy_back_to_front();
}

void tui_present_diff(void) {
    if (!g_front_valid) {
        /* First call after init — front buffer is uninitialized
         * (or invalidated). Do a full present so that subsequent
         * calls have a valid baseline. */
        tui_present();
        return;
    }
    if (g_flags & TUI_USE_SYNC_OUTPUT) {
        out_str("\x1b[?2026h");
    }
    for (int r = 1; r <= g_rows; r++) {
        emit_diff_row(r);
    }
    out_str("\x1b[0m");
    if (g_flags & TUI_USE_SYNC_OUTPUT) {
        out_str("\x1b[?2026l");
    }
    out_flush();
    sys_fflush(1);
    copy_back_to_front();
}

/* ============================================================
 *  Input parsing
 *
 *  Same architecture as round K's parser, extended to handle
 *  SGR mouse events: CSI < B ; C ; R M (press) and ... m (release).
 * ============================================================ */

#define IN_BUF_CAP 64
static char     g_in_buf[IN_BUF_CAP];
static unsigned g_in_head = 0;
static unsigned g_in_tail = 0;

enum {
    IN_STATE_GROUND = 0,
    IN_STATE_ESC,
    IN_STATE_CSI,
    IN_STATE_CSI_O,
};
static int g_in_state = IN_STATE_GROUND;

#define MAX_CSI_PARAMS 6
static int  g_csi_params[MAX_CSI_PARAMS];
static int  g_csi_n_params = 0;
static int  g_csi_curr = 0;
static bool g_csi_has_curr = false;
static char g_csi_intermediate = 0;
static int  g_esc_idle_polls = 0;

static int in_buf_used(void) { return (int)(g_in_tail - g_in_head); }

static int in_buf_peek(unsigned offset) {
    if (g_in_head + offset >= g_in_tail) return -1;
    return (unsigned char)g_in_buf[g_in_head + offset];
}

static int in_buf_pop(void) {
    if (g_in_head >= g_in_tail) return -1;
    int b = (unsigned char)g_in_buf[g_in_head++];
    if (g_in_head == g_in_tail) g_in_head = g_in_tail = 0;
    return b;
}

static void in_buf_refill(void) {
    if (g_in_head == g_in_tail) g_in_head = g_in_tail = 0;
    unsigned avail = IN_BUF_CAP - g_in_tail;
    if (avail == 0) return;
    int r = sys_read(0, g_in_buf + g_in_tail, avail);
    if (r > 0) g_in_tail += (unsigned)r;
}

static void csi_reset(void) {
    g_csi_n_params = 0;
    g_csi_curr = 0;
    g_csi_has_curr = false;
    g_csi_intermediate = 0;
}

static void csi_commit_param(void) {
    if (g_csi_has_curr && g_csi_n_params < MAX_CSI_PARAMS) {
        g_csi_params[g_csi_n_params++] = g_csi_curr;
    }
    g_csi_curr = 0;
    g_csi_has_curr = false;
}

static int csi_mod_to_tui(int m) {
    int out = 0;
    if (m <= 1) return out;
    m -= 1;
    if (m & 1) out |= TUI_MOD_SHIFT;
    if (m & 2) out |= TUI_MOD_ALT;
    if (m & 4) out |= TUI_MOD_CTRL;
    return out;
}

static void make_key(TuiEvent *ev, int key, char raw) {
    ev->kind = TUI_EV_KEY;
    ev->key.key = key;
    ev->key.mods = 0;
    ev->key.raw = raw;
}

/* SGR mouse: CSI < <button> ; <col> ; <row> M-or-m
 *
 * Button byte encodes:
 *   bits 0-1: button (0=left, 1=middle, 2=right, 3=release-in-x10 form)
 *   bit 2: shift
 *   bit 3: alt
 *   bit 4: ctrl
 *   bit 5: motion
 *   bit 6: wheel (button 64=up, 65=down)
 * In SGR mode, final 'M' = press; 'm' = release. */
static bool finish_mouse(char final, TuiEvent *out) {
    if (g_csi_n_params < 3) {
        csi_reset();
        g_in_state = IN_STATE_GROUND;
        return false;
    }
    int b = g_csi_params[0];
    int col = g_csi_params[1];
    int row = g_csi_params[2];

    int button;
    bool drag = false;

    if (b & 64) {
        /* Wheel events: 64 = up, 65 = down (plus 66/67 for
         * horizontal scroll which we don't expose). */
        if ((b & 3) == 0) button = TUI_MB_WHEEL_UP;
        else if ((b & 3) == 1) button = TUI_MB_WHEEL_DOWN;
        else { csi_reset(); g_in_state = IN_STATE_GROUND; return false; }
    } else {
        switch (b & 3) {
            case 0: button = TUI_MB_LEFT;   break;
            case 1: button = TUI_MB_MIDDLE; break;
            case 2: button = TUI_MB_RIGHT;  break;
            default:
                /* In SGR mode, the "release" encoding doesn't use
                 * button-3-as-release (that's only in x10 mode).
                 * 3 here is unknown. */
                csi_reset(); g_in_state = IN_STATE_GROUND;
                return false;
        }
        if (b & 32) drag = true;   /* motion bit */
    }

    int mods = 0;
    if (b & 4)  mods |= TUI_MOD_SHIFT;
    if (b & 8)  mods |= TUI_MOD_ALT;
    if (b & 16) mods |= TUI_MOD_CTRL;

    out->kind = TUI_EV_MOUSE;
    out->mouse.row = row;
    out->mouse.col = col;
    out->mouse.button = button;
    out->mouse.mods = mods;
    out->mouse.press = (final == 'M');
    out->mouse.drag = drag;

    csi_reset();
    g_in_state = IN_STATE_GROUND;
    return true;
}

static bool finish_csi(char final, TuiEvent *out) {
    csi_commit_param();

    /* Mouse sequence: CSI < ... M-or-m */
    if (g_csi_intermediate == '<' && (final == 'M' || final == 'm')) {
        return finish_mouse(final, out);
    }

    int mods = (g_csi_n_params >= 2) ? csi_mod_to_tui(g_csi_params[1]) : 0;
    int sym = 0;
    switch (final) {
        case 'A': sym = TUI_KEY_UP;    break;
        case 'B': sym = TUI_KEY_DOWN;  break;
        case 'C': sym = TUI_KEY_RIGHT; break;
        case 'D': sym = TUI_KEY_LEFT;  break;
        case 'H': sym = TUI_KEY_HOME;  break;
        case 'F': sym = TUI_KEY_END;   break;
    }
    if (sym) {
        out->kind = TUI_EV_KEY;
        out->key.key = sym;
        out->key.mods = mods;
        out->key.raw = 0;
        csi_reset();
        g_in_state = IN_STATE_GROUND;
        return true;
    }

    if (final == '~' && g_csi_n_params >= 1) {
        int p = g_csi_params[0];
        switch (p) {
            case 1:  sym = TUI_KEY_HOME; break;
            case 2:  sym = TUI_KEY_INSERT; break;
            case 3:  sym = TUI_KEY_DELETE; break;
            case 4:  sym = TUI_KEY_END; break;
            case 5:  sym = TUI_KEY_PAGE_UP; break;
            case 6:  sym = TUI_KEY_PAGE_DOWN; break;
            case 15: sym = TUI_KEY_F5; break;
            case 17: sym = TUI_KEY_F6; break;
            case 18: sym = TUI_KEY_F7; break;
            case 19: sym = TUI_KEY_F8; break;
            case 20: sym = TUI_KEY_F9; break;
            case 21: sym = TUI_KEY_F10; break;
            case 23: sym = TUI_KEY_F11; break;
            case 24: sym = TUI_KEY_F12; break;
        }
        if (sym) {
            out->kind = TUI_EV_KEY;
            out->key.key = sym;
            out->key.mods = mods;
            out->key.raw = 0;
            csi_reset();
            g_in_state = IN_STATE_GROUND;
            return true;
        }
    }

    csi_reset();
    g_in_state = IN_STATE_GROUND;
    return false;
}

static bool finish_csi_o(char final, TuiEvent *out) {
    int sym = 0;
    switch (final) {
        case 'A': sym = TUI_KEY_UP; break;
        case 'B': sym = TUI_KEY_DOWN; break;
        case 'C': sym = TUI_KEY_RIGHT; break;
        case 'D': sym = TUI_KEY_LEFT; break;
        case 'H': sym = TUI_KEY_HOME; break;
        case 'F': sym = TUI_KEY_END; break;
        case 'P': sym = TUI_KEY_F1; break;
        case 'Q': sym = TUI_KEY_F2; break;
        case 'R': sym = TUI_KEY_F3; break;
        case 'S': sym = TUI_KEY_F4; break;
    }
    g_in_state = IN_STATE_GROUND;
    if (sym) {
        out->kind = TUI_EV_KEY;
        out->key.key = sym;
        out->key.mods = 0;
        out->key.raw = 0;
        return true;
    }
    return false;
}

bool tui_poll_event(TuiEvent *out) {
    if (!out) return false;
    out->kind = TUI_EV_NONE;
    in_buf_refill();

    while (in_buf_used() > 0) {
        int b;
        switch (g_in_state) {
            case IN_STATE_GROUND:
                b = in_buf_pop();
                if (b == 0x1b) {
                    g_in_state = IN_STATE_ESC;
                    g_esc_idle_polls = 0;
                    break;
                }
                switch (b) {
                    case '\r':
                    case '\n':
                        make_key(out, TUI_KEY_ENTER, (char)b);
                        return true;
                    case '\t':
                        make_key(out, TUI_KEY_TAB, '\t');
                        return true;
                    case 0x7F:
                    case 0x08:
                        make_key(out, TUI_KEY_BACKSPACE, (char)b);
                        return true;
                    default:
                        make_key(out, b, (char)b);
                        return true;
                }
                break;

            case IN_STATE_ESC:
                b = in_buf_pop();
                if (b == '[') {
                    g_in_state = IN_STATE_CSI;
                    csi_reset();
                } else if (b == 'O') {
                    g_in_state = IN_STATE_CSI_O;
                } else {
                    g_in_state = IN_STATE_GROUND;
                    if (b >= 0x20 && b < 0x7F) {
                        out->kind = TUI_EV_KEY;
                        out->key.key = b;
                        out->key.mods = TUI_MOD_ALT;
                        out->key.raw = (char)b;
                        return true;
                    }
                }
                break;

            case IN_STATE_CSI:
                b = in_buf_peek(0);
                if (b < 0) return false;
                if (b >= '0' && b <= '9') {
                    in_buf_pop();
                    g_csi_curr = g_csi_curr * 10 + (b - '0');
                    g_csi_has_curr = true;
                    break;
                }
                if (b == ';') {
                    in_buf_pop();
                    csi_commit_param();
                    break;
                }
                if (b == '<' || b == '?') {
                    in_buf_pop();
                    g_csi_intermediate = (char)b;
                    break;
                }
                if (b >= 0x40 && b <= 0x7E) {
                    in_buf_pop();
                    if (finish_csi((char)b, out)) return true;
                    break;
                }
                in_buf_pop();
                csi_reset();
                g_in_state = IN_STATE_GROUND;
                break;

            case IN_STATE_CSI_O:
                b = in_buf_pop();
                if (finish_csi_o((char)b, out)) return true;
                break;
        }
    }

    if (g_in_state == IN_STATE_ESC) {
        g_esc_idle_polls++;
        if (g_esc_idle_polls >= 2) {
            g_in_state = IN_STATE_GROUND;
            g_esc_idle_polls = 0;
            make_key(out, TUI_KEY_ESCAPE, 0x1b);
            return true;
        }
    }

    return false;
}
