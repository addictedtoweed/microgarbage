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

#include "vm/vm_host_tui.h"
#include "vm/vm_ecall.h"
#include "vm/vm_core.h"
#include "vm/vm_host_stdio.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

/* ============================================================
 *  Module state
 * ============================================================ */

/* Suppress -Wunused-result on write(). We're best-effort here:
 * a closed terminal means nothing reaches the user anyway. */
static void hwrite(int fd, const void *p, size_t n) {
    ssize_t r = write(fd, p, n);
    (void)r;
}

/* Canvas owner. UINT16_MAX = no owner; SYS_TUI_INIT records the
 * caller's vm_id, SYS_TUI_SHUTDOWN clears it. Non-owners get
 * -EBUSY from any TUI syscall (except POLL_EVENT, which returns
 * 0 for non-owners — they just see no events). */
static uint16_t g_owner_vm    = UINT16_MAX;
static bool     g_initialized = false;
static unsigned g_flags       = 0;

/* Active canvas dimensions (1..VM_TUI_MAX_*). Set at init. */
static int g_rows = VM_TUI_MAX_ROWS;
static int g_cols = VM_TUI_MAX_COLS;

/* Did we (the TUI module) toggle raw mode on at init? If so we
 * undo it at shutdown. If the caller's tty was already raw and
 * we didn't change it, we leave it alone. */
static bool g_raw_mode_we_set = false;

/* Forward decls for atexit hook + raw mode reset. */
static void do_shutdown(void);

/* Pen — current fg/bg/attr for OP_PUTC / OP_PUTS. */
static uint16_t g_pen_fg    = VM_TUI_DEFAULT_COLOR;
static uint16_t g_pen_bg    = VM_TUI_DEFAULT_COLOR;
static uint8_t  g_pen_attrs = VM_TUI_ATTR_NONE;

/* Notional cursor (1-indexed). */
static int g_cur_row = 1;
static int g_cur_col = 1;

/* Clip rect (1-indexed). h/w default to full canvas. */
static int g_clip_r = 1, g_clip_c = 1;
static int g_clip_h = VM_TUI_MAX_ROWS, g_clip_w = VM_TUI_MAX_COLS;

/* The TuiCell layout matches the guest's: 6 bytes packed.
 *
 *   c       1 byte    char
 *   fg      2 bytes   color (0..15, 256=default)
 *   bg      2 bytes   color
 *   attrs   1 byte    bitmask
 *   flags   1 byte    cell flags (unused on canvas cells)
 */
typedef struct {
    char     c;
    uint16_t fg;
    uint16_t bg;
    uint8_t  attrs;
    uint8_t  flags;
} __attribute__((packed)) HostCell;

/* Back and front buffers. */
static HostCell g_canvas[VM_TUI_MAX_ROWS][VM_TUI_MAX_COLS];
static HostCell g_front [VM_TUI_MAX_ROWS][VM_TUI_MAX_COLS];
static bool     g_front_valid = false;

/* ============================================================
 *  Internal: bounds + drawable check
 * ============================================================ */

static bool in_canvas(int row, int col) {
    return row >= 1 && row <= g_rows && col >= 1 && col <= g_cols;
}

static bool in_clip(int row, int col) {
    return row >= g_clip_r && row < g_clip_r + g_clip_h &&
           col >= g_clip_c && col < g_clip_c + g_clip_w;
}

static bool drawable(int row, int col) {
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
    /* If someone else owns the canvas, refuse. If the same VM
     * inits twice, treat as idempotent (no-op). */
    if (g_owner_vm != UINT16_MAX && g_owner_vm != vm_id) {
        return -VM_EBUSY;
    }

    /* Register an atexit hook on the FIRST init we ever do.
     * If the host process exits while a guest still owns the
     * canvas (Ctrl-C, fatal error, etc.), this restores the
     * terminal so the parent shell doesn't inherit alt-screen,
     * raw mode, mouse reporting, or hidden cursor. */
    static bool atexit_registered = false;
    if (!atexit_registered) {
        atexit(do_shutdown);
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

    /* Reset input parser state (defined later in this file). */
    extern void vm_host_tui_input_reset_(void);
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
    if (flags & VM_TUI_USE_MOUSE) {
        /* SGR mouse (1006) + button-motion (1002). */
        const char *s = "\x1b[?1002h\x1b[?1006h";
        hwrite(1, s, 16);
    }
    /* TUI_USE_RAW: switch the controlling tty into raw mode so
     * keystrokes and mouse events reach us byte-by-byte instead
     * of being line-buffered and echoed back. The shell host
     * usually already has raw mode on, but a spawned game can't
     * count on that — and if we entered alt-screen with cooked
     * mode, the user's keypresses would echo onto the screen
     * over our rendering.
     *
     * vm_host_stdio_set_raw_mode silently returns false if the
     * fd isn't a TTY or termios isn't available (Cygwin
     * native-Windows binaries on a non-pty handle, e.g.); in
     * those cases the user's terminal handles things on its
     * own and we just live with whatever cooking it applies. */
    if (flags & VM_TUI_USE_RAW) {
        g_raw_mode_we_set = vm_host_stdio_set_raw_mode(true);
    } else {
        g_raw_mode_we_set = false;
    }

    return 0;
}

static void do_shutdown(void) {
    if (!g_initialized) return;

    /* Restore terminal state in reverse order. */
    if (g_flags & VM_TUI_USE_MOUSE) {
        const char *s = "\x1b[?1006l\x1b[?1002l";
        hwrite(1, s, 16);
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
    fflush(stdout);

    /* If we put the tty into raw mode, take it out so the user's
     * shell gets a normal cooked-mode terminal back when we
     * exit. If the caller already had it raw, leave it. */
    if (g_raw_mode_we_set) {
        vm_host_stdio_set_raw_mode(false);
        g_raw_mode_we_set = false;
    }

    g_initialized = false;
    g_owner_vm    = UINT16_MAX;
    g_flags       = 0;
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
    fflush(stdout);
    copy_back_to_front();
}

static void do_present_diff(void) {
    if (!g_front_valid) { do_present(); return; }
    if (g_flags & VM_TUI_USE_SYNC_OUTPUT) out_str("\x1b[?2026h");
    for (int r = 1; r <= g_rows; r++) emit_diff_row(r);
    out_str("\x1b[0m");
    if (g_flags & VM_TUI_USE_SYNC_OUTPUT) out_str("\x1b[?2026l");
    out_flush();
    fflush(stdout);
    copy_back_to_front();
}

/* ============================================================
 *  Input parser
 *
 *  Reads bytes from host stdin (non-blocking), maintains a small
 *  ring buffer, and runs a CSI / SS3 / escape-sequence state
 *  machine. Decoded events are written to the guest's buffer
 *  via the VmTuiEventRecord wire format.
 *
 *  Ported from the guest-side tui.c (round L) — same architecture:
 *  IN_STATE_GROUND / IN_STATE_ESC / IN_STATE_CSI / IN_STATE_CSI_O,
 *  CSI parameter list, SGR mouse (mode 1006), arrows, function
 *  keys F1-F12, modifiers via the second CSI parameter.
 *
 *  Local event struct (host-side intermediate representation).
 *  Mapped to VmTuiEventRecord wire format at SYS_TUI_POLL_EVENT
 *  return.
 * ============================================================ */

typedef struct {
    uint8_t kind;       /* VM_TUI_EVK_* */
    int     key;        /* decoded key (or VM_TUI_KEY_*) */
    int     mods;       /* TUI_MOD_* bitmask */
    int     row;        /* mouse coord (1-indexed) */
    int     col;
    int     button;     /* VM_TUI_MB_* */
    bool    press;      /* mouse press vs release */
    bool    drag;       /* mouse motion-with-button-held */
} InEvent;

/* Mod flags from the CSI second parameter — same encoding the
 * guest tui.h uses (shift=1, alt=2, ctrl=4). */
#define MOD_SHIFT (1u << 0)
#define MOD_ALT   (1u << 1)
#define MOD_CTRL  (1u << 2)

#define IN_BUF_CAP 256

static uint8_t  g_in_buf[IN_BUF_CAP];
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
    return g_in_buf[g_in_head + offset];
}

static int in_buf_pop(void) {
    if (g_in_head >= g_in_tail) return -1;
    int b = g_in_buf[g_in_head++];
    if (g_in_head == g_in_tail) g_in_head = g_in_tail = 0;
    return b;
}

static void in_buf_refill(void) {
    if (g_in_head == g_in_tail) g_in_head = g_in_tail = 0;
    unsigned avail = IN_BUF_CAP - g_in_tail;
    if (avail == 0) return;
    int r = vm_host_stdio_read_bytes_nonblock(g_in_buf + g_in_tail, avail);
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

static int csi_mod_decode(int m) {
    int out = 0;
    if (m <= 1) return out;
    m -= 1;
    if (m & 1) out |= MOD_SHIFT;
    if (m & 2) out |= MOD_ALT;
    if (m & 4) out |= MOD_CTRL;
    return out;
}

static void make_key(InEvent *ev, int key, int mods) {
    ev->kind = VM_TUI_EVK_KEY;
    ev->key  = key;
    ev->mods = mods;
}

static bool finish_mouse(char final, InEvent *out) {
    if (g_csi_n_params < 3) {
        csi_reset(); g_in_state = IN_STATE_GROUND;
        return false;
    }
    int b = g_csi_params[0];
    int col = g_csi_params[1];
    int row = g_csi_params[2];

    int button;
    bool drag = false;

    if (b & 64) {
        if ((b & 3) == 0) button = VM_TUI_MB_WHEEL_UP;
        else if ((b & 3) == 1) button = VM_TUI_MB_WHEEL_DOWN;
        else { csi_reset(); g_in_state = IN_STATE_GROUND; return false; }
    } else {
        switch (b & 3) {
            case 0: button = VM_TUI_MB_LEFT;   break;
            case 1: button = VM_TUI_MB_MIDDLE; break;
            case 2: button = VM_TUI_MB_RIGHT;  break;
            default:
                csi_reset(); g_in_state = IN_STATE_GROUND;
                return false;
        }
        if (b & 32) drag = true;
    }

    int mods = 0;
    if (b & 4)  mods |= MOD_SHIFT;
    if (b & 8)  mods |= MOD_ALT;
    if (b & 16) mods |= MOD_CTRL;

    out->kind   = VM_TUI_EVK_MOUSE;
    out->row    = row;
    out->col    = col;
    out->button = button;
    out->mods   = mods;
    out->press  = (final == 'M');
    out->drag   = drag;

    csi_reset(); g_in_state = IN_STATE_GROUND;
    return true;
}

static bool finish_csi(char final, InEvent *out) {
    csi_commit_param();

    /* Mouse sequence: CSI < ... M-or-m */
    if (g_csi_intermediate == '<' && (final == 'M' || final == 'm')) {
        return finish_mouse(final, out);
    }

    int mods = (g_csi_n_params >= 2) ? csi_mod_decode(g_csi_params[1]) : 0;
    int sym = 0;
    switch (final) {
        case 'A': sym = VM_TUI_KEY_UP;    break;
        case 'B': sym = VM_TUI_KEY_DOWN;  break;
        case 'C': sym = VM_TUI_KEY_RIGHT; break;
        case 'D': sym = VM_TUI_KEY_LEFT;  break;
        case 'H': sym = VM_TUI_KEY_HOME;  break;
        case 'F': sym = VM_TUI_KEY_END;   break;
    }
    if (sym) {
        make_key(out, sym, mods);
        csi_reset(); g_in_state = IN_STATE_GROUND;
        return true;
    }

    if (final == '~' && g_csi_n_params >= 1) {
        int p = g_csi_params[0];
        switch (p) {
            case 1:  sym = VM_TUI_KEY_HOME; break;
            case 2:  sym = VM_TUI_KEY_INSERT; break;
            case 3:  sym = VM_TUI_KEY_DELETE; break;
            case 4:  sym = VM_TUI_KEY_END; break;
            case 5:  sym = VM_TUI_KEY_PAGE_UP; break;
            case 6:  sym = VM_TUI_KEY_PAGE_DOWN; break;
            case 15: sym = VM_TUI_KEY_F1 + 4; break;       /* F5 */
            case 17: sym = VM_TUI_KEY_F1 + 5; break;
            case 18: sym = VM_TUI_KEY_F1 + 6; break;
            case 19: sym = VM_TUI_KEY_F1 + 7; break;
            case 20: sym = VM_TUI_KEY_F1 + 8; break;
            case 21: sym = VM_TUI_KEY_F1 + 9; break;
            case 23: sym = VM_TUI_KEY_F1 + 10; break;
            case 24: sym = VM_TUI_KEY_F1 + 11; break;
        }
        if (sym) {
            make_key(out, sym, mods);
            csi_reset(); g_in_state = IN_STATE_GROUND;
            return true;
        }
    }

    csi_reset(); g_in_state = IN_STATE_GROUND;
    return false;
}

static bool finish_csi_o(char final, InEvent *out) {
    int sym = 0;
    switch (final) {
        case 'A': sym = VM_TUI_KEY_UP; break;
        case 'B': sym = VM_TUI_KEY_DOWN; break;
        case 'C': sym = VM_TUI_KEY_RIGHT; break;
        case 'D': sym = VM_TUI_KEY_LEFT; break;
        case 'H': sym = VM_TUI_KEY_HOME; break;
        case 'F': sym = VM_TUI_KEY_END; break;
        case 'P': sym = VM_TUI_KEY_F1; break;
        case 'Q': sym = VM_TUI_KEY_F1 + 1; break;
        case 'R': sym = VM_TUI_KEY_F1 + 2; break;
        case 'S': sym = VM_TUI_KEY_F1 + 3; break;
    }
    g_in_state = IN_STATE_GROUND;
    if (sym) {
        make_key(out, sym, 0);
        return true;
    }
    return false;
}

/* Pump the state machine; produces at most one event. Returns
 * true if an event was produced. */
static bool poll_input(InEvent *out) {
    out->kind = VM_TUI_EVK_NONE;
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
                        make_key(out, VM_TUI_KEY_ENTER, 0);
                        return true;
                    case '\t':
                        make_key(out, VM_TUI_KEY_TAB, 0);
                        return true;
                    case 0x7F:
                    case 0x08:
                        make_key(out, VM_TUI_KEY_BACKSPACE, 0);
                        return true;
                    default:
                        make_key(out, b, 0);
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
                        make_key(out, b, MOD_ALT);
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

    /* Bare ESC: after two consecutive polls with no follow-up
     * bytes, treat as ESC-key-pressed. */
    if (g_in_state == IN_STATE_ESC) {
        g_esc_idle_polls++;
        if (g_esc_idle_polls >= 2) {
            g_in_state = IN_STATE_GROUND;
            g_esc_idle_polls = 0;
            make_key(out, VM_TUI_KEY_ESCAPE, 0);
            return true;
        }
    }

    return false;
}

/* Marshal a host-side InEvent into the guest-visible wire
 * record. The structure layout is fixed by vm_host_tui.h. */
static void marshal_event(const InEvent *src, VmTuiEventRecord *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->kind   = src->kind;
    dst->key    = (uint16_t)src->key;
    dst->mods   = (uint8_t)src->mods;
    dst->button = (uint8_t)src->button;
    dst->row    = (uint16_t)src->row;
    dst->col    = (uint16_t)src->col;
    uint8_t flags = 0;
    if (src->press) flags |= VM_TUI_EVF_PRESS;
    if (src->drag)  flags |= VM_TUI_EVF_DRAG;
    dst->flags = flags;
}

/* Public-named-but-internal: reset the input parser. Called by
 * do_init via an extern forward declaration so do_init can sit
 * above the parser globals without re-ordering everything. */
void vm_host_tui_input_reset_(void) {
    g_in_head = 0; g_in_tail = 0;
    g_in_state = IN_STATE_GROUND;
    csi_reset();
    g_esc_idle_polls = 0;
}

/* Test-only: inject bytes into the input ring buffer as if they
 * arrived from stdin. Returns the number of bytes accepted (0
 * if the buffer is full). Used by test_vm_host_tui.c to exercise
 * the parser without a real TTY. */
unsigned vm_host_tui_test_inject_input_(const void *bytes, unsigned n) {
    const uint8_t *b = (const uint8_t *)bytes;
    if (g_in_head == g_in_tail) g_in_head = g_in_tail = 0;
    unsigned avail = IN_BUF_CAP - g_in_tail;
    if (n > avail) n = avail;
    for (unsigned i = 0; i < n; i++) g_in_buf[g_in_tail + i] = b[i];
    g_in_tail += n;
    return n;
}

/* ============================================================
 *  Tile subsystem (round T.3b)
 *
 *  Storage:
 *    g_tile_arena   one shared cell pool, served bump-style
 *                   with no per-tile free (matches the guest-
 *                   side tile arena's behavior). VM unload
 *                   compacts the arena by reclaiming every
 *                   range that belonged to the dead VM.
 *
 *    g_tile_slots   one row per (vm, slot) pair. Each row holds
 *                   the slot's metadata plus the arena range
 *                   it occupies.
 *
 *  Handle encoding:
 *    bits 31..16  generation counter (wraps; aliasing is
 *                 effectively impossible inside a 16-bit window)
 *    bits 15..8   vm_id
 *    bits  7..0   slot index within the VM
 *
 *  The vm_id bits are baked into the handle so a guest can't
 *  pass another VM's handle and operate on its tiles.
 * ============================================================ */

typedef struct {
    bool     in_use;
    uint16_t generation;
    uint16_t rows, cols;
    uint32_t arena_off;     /* byte offset into g_tile_arena */
    uint32_t arena_len;     /* bytes (rows * cols * sizeof(HostCell)) */
} TileSlot;

static HostCell  g_tile_arena[VM_TUI_TILE_ARENA_BYTES / sizeof(HostCell)];
static uint32_t  g_tile_arena_used = 0;
static TileSlot  g_tile_slots[VM_SCHED_MAX_VMS][VM_TUI_TILES_PER_VM];

static uint32_t pack_handle(uint16_t vm_id, uint8_t slot, uint16_t gen) {
    return ((uint32_t)gen << 16) | ((uint32_t)vm_id << 8) | slot;
}

/* Returns a pointer to the slot, or NULL if the handle is bad.
 * Checks: vm_id matches the caller, slot is in range, slot is
 * in_use, generation matches. */
static TileSlot *resolve_handle(uint16_t caller_vm, uint32_t handle) {
    uint8_t  slot   = (uint8_t)(handle & 0xff);
    uint16_t hvm    = (uint16_t)((handle >> 8) & 0xff);
    uint16_t gen    = (uint16_t)(handle >> 16);
    if (hvm != caller_vm) return NULL;
    if (slot >= VM_TUI_TILES_PER_VM) return NULL;
    TileSlot *s = &g_tile_slots[caller_vm][slot];
    if (!s->in_use) return NULL;
    if (s->generation != gen) return NULL;
    return s;
}

static HostCell *tile_cells(TileSlot *s) {
    return &g_tile_arena[s->arena_off / sizeof(HostCell)];
}

/* Allocate a tile of the given dimensions for vm_id. Returns
 * handle on success, or 0 if no slot or no arena. (0 is never
 * a valid handle — slot 0 with generation 0 is excluded by
 * forcing the initial generation to 1.) */
static uint32_t tile_create(uint16_t vm_id, int rows, int cols) {
    if (rows <= 0 || cols <= 0) return 0;
    if (rows > VM_TUI_MAX_ROWS || cols > VM_TUI_MAX_COLS) return 0;

    uint32_t need = (uint32_t)rows * (uint32_t)cols * (uint32_t)sizeof(HostCell);
    if (g_tile_arena_used + need > sizeof(g_tile_arena)) return 0;

    /* Find a free slot. */
    uint8_t slot = 0xff;
    for (uint8_t i = 0; i < VM_TUI_TILES_PER_VM; i++) {
        if (!g_tile_slots[vm_id][i].in_use) { slot = i; break; }
    }
    if (slot == 0xff) return 0;

    TileSlot *s = &g_tile_slots[vm_id][slot];
    s->in_use = true;
    s->generation = (uint16_t)(s->generation + 1);
    if (s->generation == 0) s->generation = 1;    /* never give out gen=0 */
    s->rows = (uint16_t)rows;
    s->cols = (uint16_t)cols;
    s->arena_off = g_tile_arena_used;
    s->arena_len = need;
    g_tile_arena_used += need;

    /* Initialize as fully transparent cells. */
    HostCell *cells = tile_cells(s);
    HostCell blank = {0};
    blank.c = ' ';
    blank.fg = VM_TUI_DEFAULT_COLOR;
    blank.bg = VM_TUI_DEFAULT_COLOR;
    blank.attrs = 0;
    blank.flags = VM_TUI_CELL_TRANSPARENT;
    for (uint32_t i = 0; i < (uint32_t)rows * (uint32_t)cols; i++) {
        cells[i] = blank;
    }

    return pack_handle(vm_id, slot, s->generation);
}

static int32_t tile_destroy(uint16_t vm_id, uint32_t handle) {
    TileSlot *s = resolve_handle(vm_id, handle);
    if (!s) return -VM_EBADF;
    s->in_use = false;
    /* Arena memory is not reclaimed individually; we'll compact
     * on full release_for_vm. */
    return 0;
}

/* Release all tiles for one VM and reclaim their arena bytes.
 * Called from vm_host_tui_release_for_vm. */
static void tile_release_for_vm(uint16_t vm_id) {
    for (uint8_t i = 0; i < VM_TUI_TILES_PER_VM; i++) {
        g_tile_slots[vm_id][i].in_use = false;
    }
    /* Compact the arena: walk all VMs and move surviving allocations
     * down. This is O(total tiles * arena_size) in the worst case,
     * but tile creates/destroys are rare events. */
    uint32_t new_used = 0;
    for (uint16_t v = 0; v < VM_SCHED_MAX_VMS; v++) {
        for (uint8_t i = 0; i < VM_TUI_TILES_PER_VM; i++) {
            TileSlot *s = &g_tile_slots[v][i];
            if (!s->in_use) continue;
            if (s->arena_off != new_used) {
                memmove(&g_tile_arena[new_used / sizeof(HostCell)],
                        &g_tile_arena[s->arena_off / sizeof(HostCell)],
                        s->arena_len);
                s->arena_off = new_used;
            }
            new_used += s->arena_len;
        }
    }
    g_tile_arena_used = new_used;
}

static int32_t tile_set(TileSlot *s, int row, int col,
                         char c, uint16_t fg, uint16_t bg, uint8_t attrs) {
    if (row < 1 || row > s->rows || col < 1 || col > s->cols) return 0;
    HostCell *cells = tile_cells(s);
    HostCell *cell = &cells[(row - 1) * s->cols + (col - 1)];
    cell->c = c;
    cell->fg = fg;
    cell->bg = bg;
    cell->attrs = attrs;
    cell->flags = 0;
    return 0;
}

static void tile_fill(TileSlot *s, char c, uint16_t fg, uint16_t bg, uint8_t attrs) {
    HostCell *cells = tile_cells(s);
    HostCell tmpl = { .c = c, .fg = fg, .bg = bg, .attrs = attrs, .flags = 0 };
    uint32_t n = (uint32_t)s->rows * (uint32_t)s->cols;
    for (uint32_t i = 0; i < n; i++) cells[i] = tmpl;
}

static int32_t tile_set_transparent(TileSlot *s, int row, int col) {
    if (row < 1 || row > s->rows || col < 1 || col > s->cols) return 0;
    HostCell *cells = tile_cells(s);
    cells[(row - 1) * s->cols + (col - 1)].flags |= VM_TUI_CELL_TRANSPARENT;
    return 0;
}

static int32_t tile_blit(TileSlot *s, int dest_row, int dest_col) {
    HostCell *cells = tile_cells(s);
    for (int r = 0; r < s->rows; r++) {
        for (int c = 0; c < s->cols; c++) {
            HostCell *src = &cells[r * s->cols + c];
            if (src->flags & VM_TUI_CELL_TRANSPARENT) continue;
            int dr = dest_row + r;
            int dc = dest_col + c;
            if (!drawable(dr, dc)) continue;
            g_canvas[dr - 1][dc - 1] = *src;
            /* Clear transparent bit on the canvas — it has no
             * meaning on canvas cells. */
            g_canvas[dr - 1][dc - 1].flags = 0;
        }
    }
    return 0;
}

static int32_t tile_grab(TileSlot *s, int src_row, int src_col, int h, int w) {
    /* Copy a rectangle from the canvas into the tile, clamping
     * to both the tile dims and canvas bounds. Cells outside
     * the canvas become transparent in the destination. */
    if (h > s->rows) h = s->rows;
    if (w > s->cols) w = s->cols;
    HostCell *cells = tile_cells(s);
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            HostCell *dst = &cells[r * s->cols + c];
            int sr = src_row + r;
            int sc = src_col + c;
            if (in_canvas(sr, sc)) {
                *dst = g_canvas[sr - 1][sc - 1];
                dst->flags = 0;   /* never transparent after grab */
            } else {
                dst->c = ' ';
                dst->fg = VM_TUI_DEFAULT_COLOR;
                dst->bg = VM_TUI_DEFAULT_COLOR;
                dst->attrs = 0;
                dst->flags = VM_TUI_CELL_TRANSPARENT;
            }
        }
    }
    return 0;
}

/* ============================================================
 *  ECALL handlers
 * ============================================================ */

static void handle_tui_init(VmCpu *cpu, void *system) {
    (void)system;
    int rows = (int)cpu->regs[VM_REG_A0];
    int cols = (int)cpu->regs[VM_REG_A1];
    unsigned flags = cpu->regs[VM_REG_A2];
    int r = do_init(cpu->vm_id, rows, cols, flags);
    cpu->regs[VM_REG_A0] = (uint32_t)r;
}

static void handle_tui_shutdown(VmCpu *cpu, void *system) {
    (void)system;
    /* Only owner can shut down. Non-owners no-op succeed. */
    if (g_owner_vm == cpu->vm_id) do_shutdown();
    cpu->regs[VM_REG_A0] = 0;
}

static void handle_tui_get_dims(VmCpu *cpu, void *system) {
    (void)system;
    if (!g_initialized) { cpu->regs[VM_REG_A0] = 0; return; }
    cpu->regs[VM_REG_A0] = (uint32_t)((g_rows << 16) | g_cols);
}

static void handle_tui_present(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    do_present();
    cpu->regs[VM_REG_A0] = 0;
}

static void handle_tui_present_diff(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    do_present_diff();
    cpu->regs[VM_REG_A0] = 0;
}

static void handle_tui_poll_event(VmCpu *cpu, void *system) {
    (void)system;
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
    if (!poll_input(&ev)) {
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
    marshal_event(&ev, rec);
    cpu->regs[VM_REG_A0] = 1;
}

static void handle_tui_flush_draw(VmCpu *cpu, void *system) {
    (void)system;
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

/* ============================================================
 *  Tile ECALL handlers
 * ============================================================ */

static void handle_tile_create(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    int rows = (int)cpu->regs[VM_REG_A0];
    int cols = (int)cpu->regs[VM_REG_A1];
    uint32_t h = tile_create(cpu->vm_id, rows, cols);
    if (h == 0) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_ENOMEM;
        return;
    }
    cpu->regs[VM_REG_A0] = h;
}

static void handle_tile_destroy(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    int32_t r = tile_destroy(cpu->vm_id, cpu->regs[VM_REG_A0]);
    cpu->regs[VM_REG_A0] = (uint32_t)r;
}

/* TILE_SET arg layout:
 *   a0 = handle
 *   a1 = (row << 16) | col
 *   a2 = (c << 8) | attrs
 *   a3 = fg
 *   a4 = bg
 */
static void handle_tile_set(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    uint32_t handle = cpu->regs[VM_REG_A0];
    uint32_t rc     = cpu->regs[VM_REG_A1];
    uint32_t ca     = cpu->regs[VM_REG_A2];
    uint16_t fg     = (uint16_t)cpu->regs[VM_REG_A3];
    uint16_t bg     = (uint16_t)cpu->regs[VM_REG_A4];

    TileSlot *s = resolve_handle(cpu->vm_id, handle);
    if (!s) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF; return; }

    int row = (int)(rc >> 16);
    int col = (int)(rc & 0xffff);
    char c  = (char)((ca >> 8) & 0xff);
    uint8_t attrs = (uint8_t)(ca & 0xff);

    cpu->regs[VM_REG_A0] = (uint32_t)tile_set(s, row, col, c, fg, bg, attrs);
}

/* TILE_FILL arg layout:
 *   a0 = handle
 *   a1 = (c << 8) | attrs
 *   a2 = fg
 *   a3 = bg
 */
static void handle_tile_fill(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    uint32_t handle = cpu->regs[VM_REG_A0];
    uint32_t ca     = cpu->regs[VM_REG_A1];
    uint16_t fg     = (uint16_t)cpu->regs[VM_REG_A2];
    uint16_t bg     = (uint16_t)cpu->regs[VM_REG_A3];

    TileSlot *s = resolve_handle(cpu->vm_id, handle);
    if (!s) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF; return; }

    char c = (char)((ca >> 8) & 0xff);
    uint8_t attrs = (uint8_t)(ca & 0xff);
    tile_fill(s, c, fg, bg, attrs);
    cpu->regs[VM_REG_A0] = 0;
}

static void handle_tile_set_transparent(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    uint32_t handle = cpu->regs[VM_REG_A0];
    int row = (int)cpu->regs[VM_REG_A1];
    int col = (int)cpu->regs[VM_REG_A2];
    TileSlot *s = resolve_handle(cpu->vm_id, handle);
    if (!s) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF; return; }
    cpu->regs[VM_REG_A0] = (uint32_t)tile_set_transparent(s, row, col);
}

static void handle_tile_blit(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    uint32_t handle = cpu->regs[VM_REG_A0];
    int dest_row = (int)cpu->regs[VM_REG_A1];
    int dest_col = (int)cpu->regs[VM_REG_A2];
    TileSlot *s = resolve_handle(cpu->vm_id, handle);
    if (!s) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF; return; }
    cpu->regs[VM_REG_A0] = (uint32_t)tile_blit(s, dest_row, dest_col);
}

/* TILE_GRAB arg layout:
 *   a0 = handle
 *   a1 = (src_row << 16) | src_col
 *   a2 = (h << 16) | w
 */
static void handle_tile_grab(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    uint32_t handle = cpu->regs[VM_REG_A0];
    uint32_t rc     = cpu->regs[VM_REG_A1];
    uint32_t hw     = cpu->regs[VM_REG_A2];
    TileSlot *s = resolve_handle(cpu->vm_id, handle);
    if (!s) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF; return; }
    int src_row = (int)(rc >> 16);
    int src_col = (int)(rc & 0xffff);
    int h = (int)(hw >> 16);
    int w = (int)(hw & 0xffff);
    cpu->regs[VM_REG_A0] = (uint32_t)tile_grab(s, src_row, src_col, h, w);
}

static void handle_tile_dims(VmCpu *cpu, void *system) {
    (void)system;
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    TileSlot *s = resolve_handle(cpu->vm_id, cpu->regs[VM_REG_A0]);
    if (!s) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF; return; }
    cpu->regs[VM_REG_A0] = ((uint32_t)s->rows << 16) | (uint32_t)s->cols;
}

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

    /* Tile handlers. Failures unwind back through the same fail
     * chain. */
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_CREATE,
                           handle_tile_create)) goto fail_flush;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_DESTROY,
                           handle_tile_destroy)) goto fail_tcreate;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_SET,
                           handle_tile_set)) goto fail_tdestroy;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_FILL,
                           handle_tile_fill)) goto fail_tset;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_SET_TRANSPARENT,
                           handle_tile_set_transparent)) goto fail_tfill;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_BLIT,
                           handle_tile_blit)) goto fail_tstrans;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_GRAB,
                           handle_tile_grab)) goto fail_tblit;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_DIMS,
                           handle_tile_dims)) goto fail_tgrab;

    /* Register the auto-release hook so a guest that exits without
     * calling SYS_TUI_SHUTDOWN doesn't permanently lock the canvas
     * and so tiles get reclaimed automatically. */
    vm_system_register_unload_hook(sys, tui_unload_adapter, NULL);

    return true;

fail_tgrab:    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_GRAB);
fail_tblit:    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_BLIT);
fail_tstrans:  vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_SET_TRANSPARENT);
fail_tfill:    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_FILL);
fail_tset:     vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_SET);
fail_tdestroy: vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_DESTROY);
fail_tcreate:  vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_CREATE);
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
    tile_release_for_vm(vm_id);
    if (g_owner_vm == vm_id) do_shutdown();
}
