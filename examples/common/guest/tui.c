/* lib/tui.c — guest-side shim for the host TUI service.
 *
 * The canvas and SGR-emit logic live in the host; this file is
 * a thin command-buffer builder + flush. The public API
 * (tui_init, tui_set_cell, etc.) is preserved for source
 * compatibility with guests written against the old client-side
 * library.
 *
 * Size impact: the original client-side library was ~1100 lines
 * + ~80 KB of canvas BSS per guest. This shim is ~300 lines and
 * ~2 KB of command-buffer BSS.
 *
 * Tiles: the tile API is preserved for source compatibility but
 * implemented entirely guest-side as small in-RAM cell arrays.
 * tui_blit_tile expands to a sequence of host SET_CELL commands.
 *
 * Public domain (CC0).
 */

#include "tui.h"
#include <stdint.h>
#include <stddef.h>

/* ============================================================
 *  Host syscall numbers (matched to host vm_ecall.h)
 * ============================================================ */

#define SYS_TUI_INIT       1132
#define SYS_TUI_SHUTDOWN   1133
#define SYS_TUI_GET_DIMS   1134
#define SYS_TUI_PRESENT    1135
#define SYS_TUI_PRESENT_DIFF 1136
#define SYS_TUI_POLL_EVENT 1137
#define SYS_TUI_FLUSH_DRAW 1138

/* Draw-command opcodes (must match host's parser). */
#define OP_END         0
#define OP_SET_CELL    1
#define OP_FILL_RECT   2
#define OP_PRINT       3
#define OP_BOX         4
#define OP_MOVE        5
#define OP_SET_FG      6
#define OP_SET_BG      7
#define OP_SET_ATTR    8
#define OP_CLEAR       9
#define OP_SET_CLIP   10
#define OP_CLEAR_CLIP 11
#define OP_PUTC       12
#define OP_PUTS       13

/* Event wire format (matches VmTuiEventRecord on the host). */
#define EVK_NONE   0
#define EVK_KEY    1
#define EVK_MOUSE  2

typedef struct {
    uint8_t  kind;
    uint8_t  reserved0;
    uint16_t key;
    uint8_t  mods;
    uint8_t  button;
    uint16_t row;
    uint16_t col;
    uint8_t  flags;
    uint8_t  reserved1;
    uint32_t reserved2;
} __attribute__((packed)) WireEvent;

#define EVF_PRESS  (1u << 0)
#define EVF_DRAG   (1u << 1)

/* ============================================================
 *  Syscall trampolines
 * ============================================================ */

static inline uint32_t sys0(uint32_t n) {
    register uint32_t a0 asm("a0");
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline uint32_t sys1(uint32_t n, uint32_t x0) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline uint32_t sys2(uint32_t n, uint32_t x0, uint32_t x1) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a7) : "memory");
    return a0;
}
static inline uint32_t sys3(uint32_t n, uint32_t x0, uint32_t x1, uint32_t x2) {
    register uint32_t a0 asm("a0") = x0;
    register uint32_t a1 asm("a1") = x1;
    register uint32_t a2 asm("a2") = x2;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

/* ============================================================
 *  Draw-command buffer
 *
 *  Sized to hold roughly a full-screen frame's worth of ops.
 *  At ~7 bytes per SET_CELL, a 30×80 canvas = 2400 cells =
 *  ~17 KB of SET_CELL commands. But typical frames touch a
 *  fraction of cells (snake: ~50 cells/frame). 2 KB is
 *  comfortable. The buffer auto-flushes when nearly full, so
 *  larger frames just incur an extra syscall — correctness is
 *  preserved.
 * ============================================================ */

#define CMD_BUF_CAP 2048
#define CMD_BUF_HEADROOM 64       /* never start an op with less left */

static uint8_t  g_cmd_buf[CMD_BUF_CAP];
static unsigned g_cmd_pos = 0;

static bool g_initialized = false;
static int  g_rows = TUI_MAX_ROWS;
static int  g_cols = TUI_MAX_COLS;

/* Forward */
static void flush_cmds(void);

static void put_u8(uint8_t v) {
    g_cmd_buf[g_cmd_pos++] = v;
}

static void put_u16(uint16_t v) {
    g_cmd_buf[g_cmd_pos++] = (uint8_t)(v & 0xff);
    g_cmd_buf[g_cmd_pos++] = (uint8_t)((v >> 8) & 0xff);
}

static void put_bytes(const void *p, unsigned n) {
    const uint8_t *src = (const uint8_t *)p;
    for (unsigned i = 0; i < n; i++) g_cmd_buf[g_cmd_pos++] = src[i];
}

/* Ensure at least `need` bytes free in the buffer; flush if not.
 * After flush_cmds, the buffer is empty (well-defined start). */
static void ensure_space(unsigned need) {
    if (!g_initialized) return;
    if (g_cmd_pos + need + 1 > CMD_BUF_CAP) {
        flush_cmds();
    }
}

static void flush_cmds(void) {
    if (g_cmd_pos == 0) return;
    /* Terminate the buffer with OP_END (the host's parser stops
     * at OP_END before EOB, so an explicit terminator is
     * defense-in-depth). */
    put_u8(OP_END);
    sys2(SYS_TUI_FLUSH_DRAW, (uint32_t)(uintptr_t)g_cmd_buf, (uint32_t)g_cmd_pos);
    g_cmd_pos = 0;
}

/* ============================================================
 *  Public API: lifecycle
 * ============================================================ */

bool tui_init(unsigned flags, int rows, int cols) {
    if (g_initialized) return true;
    if (rows <= 0) rows = TUI_MAX_ROWS;
    if (cols <= 0) cols = TUI_MAX_COLS;
    int32_t r = (int32_t)sys3(SYS_TUI_INIT, (uint32_t)rows, (uint32_t)cols, flags);
    if (r < 0) return false;
    g_initialized = true;
    g_rows = rows;
    g_cols = cols;
    g_cmd_pos = 0;
    return true;
}

void tui_shutdown(void) {
    if (!g_initialized) return;
    flush_cmds();
    sys0(SYS_TUI_SHUTDOWN);
    g_initialized = false;
}

int tui_rows(void) { return g_rows; }
int tui_cols(void) { return g_cols; }

/* ============================================================
 *  Clip
 * ============================================================ */

void tui_set_clip(int row, int col, int h, int w) {
    ensure_space(9);
    put_u8(OP_SET_CLIP);
    put_u16((uint16_t)row); put_u16((uint16_t)col);
    put_u16((uint16_t)h);   put_u16((uint16_t)w);
}

void tui_clear_clip(void) {
    ensure_space(1);
    put_u8(OP_CLEAR_CLIP);
}

/* ============================================================
 *  Pen
 * ============================================================ */

/* The pen lives host-side; we just shovel ops. */
void tui_set_fg(TuiColor c) {
    ensure_space(3);
    put_u8(OP_SET_FG);
    put_u16((uint16_t)c);
}

void tui_set_bg(TuiColor c) {
    ensure_space(3);
    put_u8(OP_SET_BG);
    put_u16((uint16_t)c);
}

void tui_set_attr(unsigned attrs) {
    ensure_space(2);
    put_u8(OP_SET_ATTR);
    put_u8((uint8_t)attrs);
}

void tui_reset(void) {
    tui_set_fg(TUI_DEFAULT_COLOR);
    tui_set_bg(TUI_DEFAULT_COLOR);
    tui_set_attr(TUI_ATTR_NONE);
}

/* ============================================================
 *  Drawing primitives
 * ============================================================ */

void tui_set_cell(int row, int col, char c,
                  TuiColor fg, TuiColor bg, unsigned attrs) {
    ensure_space(11);
    put_u8(OP_SET_CELL);
    put_u16((uint16_t)row); put_u16((uint16_t)col);
    put_u8((uint8_t)c);
    put_u16((uint16_t)fg);  put_u16((uint16_t)bg);
    put_u8((uint8_t)attrs);
}

void tui_move(int row, int col) {
    ensure_space(5);
    put_u8(OP_MOVE);
    put_u16((uint16_t)row); put_u16((uint16_t)col);
}

void tui_putc(char c) {
    ensure_space(2);
    put_u8(OP_PUTC);
    put_u8((uint8_t)c);
}

void tui_puts(const char *s) {
    /* Walk to find length; cap at 200 so a single op doesn't
     * dominate the buffer. Long strings get split into multiple
     * PUTS ops. */
    while (*s) {
        unsigned n = 0;
        while (s[n] && n < 200) n++;
        ensure_space(3 + n);
        put_u8(OP_PUTS);
        put_u16((uint16_t)n);
        put_bytes(s, n);
        s += n;
    }
}

void tui_clear(void) {
    ensure_space(1);
    put_u8(OP_CLEAR);
}

void tui_fill_rect(int row, int col, int h, int w, char c) {
    ensure_space(10);
    put_u8(OP_FILL_RECT);
    put_u16((uint16_t)row); put_u16((uint16_t)col);
    put_u16((uint16_t)h);   put_u16((uint16_t)w);
    put_u8((uint8_t)c);
}

void tui_text_block(int row, int col, int h, int w, const char *text) {
    /* Walk lines, emit a PRINT per line truncated to w. We need
     * pen state to honor it — but the host pen is updated only by
     * OP_SET_FG/BG/ATTR; the PRINT op carries its own per-call
     * style. The library has no view into the current pen because
     * pen state lives host-side now. Workaround: use color sentinels
     * that mean 'inherit from pen'. Host treats fg/bg = 0xFFFF as
     * 'use current pen' — but that's not implemented; we pass
     * VM_TUI_DEFAULT_COLOR (256) which renders as terminal default.
     *
     * text_block is intentionally simple: it emits with default
     * colors. Guests that want colored text blocks use tui_set_fg/bg
     * before each line + tui_move + tui_puts. */
    int r = row;
    while (*text && r < row + h) {
        unsigned n = 0;
        while (text[n] && text[n] != '\n' && (int)n < w) n++;
        if (n > 0) {
            ensure_space(12 + n);
            put_u8(OP_PRINT);
            put_u16((uint16_t)r);
            put_u16((uint16_t)col);
            put_u16((uint16_t)TUI_DEFAULT_COLOR);
            put_u16((uint16_t)TUI_DEFAULT_COLOR);
            put_u8(0);
            put_u16((uint16_t)n);
            put_bytes(text, n);
        }
        text += n;
        while (*text && *text != '\n') text++;     /* skip overflow */
        if (*text == '\n') text++;
        r++;
    }
}

/* Box: 0=single, 1=double, 2=ASCII. */
static void box_with_style(int row, int col, int h, int w, uint8_t style) {
    ensure_space(10);
    put_u8(OP_BOX);
    put_u16((uint16_t)row); put_u16((uint16_t)col);
    put_u16((uint16_t)h);   put_u16((uint16_t)w);
    put_u8(style);
}

void tui_box_single(int row, int col, int h, int w) {
    box_with_style(row, col, h, w, 0);
}

void tui_box_double(int row, int col, int h, int w) {
    box_with_style(row, col, h, w, 1);
}

void tui_box_ascii(int row, int col, int h, int w) {
    box_with_style(row, col, h, w, 2);
}

/* ============================================================
 *  Tiles — backed by host SYS_TUI_TILE_* syscalls
 *
 *  Tile storage lives in the host: the per-VM tile slot table
 *  and shared cell arena live in vm_host_tui. Handles are
 *  opaque u32 values from the guest's perspective.
 * ============================================================ */

#define SYS_TUI_TILE_CREATE          1139
#define SYS_TUI_TILE_DESTROY         1140
#define SYS_TUI_TILE_SET             1141
#define SYS_TUI_TILE_FILL            1142
#define SYS_TUI_TILE_SET_TRANSPARENT 1143
#define SYS_TUI_TILE_BLIT            1144
#define SYS_TUI_TILE_GRAB            1145

static inline uint32_t sys4(uint32_t n, uint32_t a, uint32_t b,
                             uint32_t c, uint32_t d) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a1 asm("a1") = b;
    register uint32_t a2 asm("a2") = c;
    register uint32_t a3 asm("a3") = d;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0)
                  : "r"(a1), "r"(a2), "r"(a3), "r"(a7) : "memory");
    return a0;
}

static inline uint32_t sys5(uint32_t n, uint32_t a, uint32_t b,
                             uint32_t c, uint32_t d, uint32_t e) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a1 asm("a1") = b;
    register uint32_t a2 asm("a2") = c;
    register uint32_t a3 asm("a3") = d;
    register uint32_t a4 asm("a4") = e;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0)
                  : "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a7) : "memory");
    return a0;
}

TuiTileId tui_tile_create(int rows, int cols) {
    /* Must flush pending draw commands so the host's canvas state
     * is current — tile create itself doesn't touch the canvas,
     * but the host may need a consistent state for arena layout. */
    flush_cmds();
    int32_t r = (int32_t)sys2(SYS_TUI_TILE_CREATE,
                               (uint32_t)rows, (uint32_t)cols);
    if (r <= 0) return TUI_TILE_NONE;
    return (TuiTileId)r;
}

void tui_tile_destroy(TuiTileId tile) {
    if (tile == TUI_TILE_NONE) return;
    flush_cmds();
    sys1(SYS_TUI_TILE_DESTROY, (uint32_t)tile);
}

void tui_tile_set(TuiTileId tile, int row, int col,
                  char c, TuiColor fg, TuiColor bg, unsigned attrs) {
    if (tile == TUI_TILE_NONE) return;
    flush_cmds();
    uint32_t rc = ((uint32_t)row << 16) | (uint32_t)(col & 0xffff);
    uint32_t ca = ((uint32_t)(unsigned char)c << 8) | (attrs & 0xff);
    sys5(SYS_TUI_TILE_SET, (uint32_t)tile, rc, ca,
         (uint32_t)fg, (uint32_t)bg);
}

void tui_tile_set_transparent(TuiTileId tile, int row, int col) {
    if (tile == TUI_TILE_NONE) return;
    flush_cmds();
    sys3(SYS_TUI_TILE_SET_TRANSPARENT,
         (uint32_t)tile, (uint32_t)row, (uint32_t)col);
}

void tui_tile_fill(TuiTileId tile, char c, TuiColor fg, TuiColor bg,
                   unsigned attrs) {
    if (tile == TUI_TILE_NONE) return;
    flush_cmds();
    uint32_t ca = ((uint32_t)(unsigned char)c << 8) | (attrs & 0xff);
    sys4(SYS_TUI_TILE_FILL, (uint32_t)tile, ca,
         (uint32_t)fg, (uint32_t)bg);
}

void tui_blit_tile(TuiTileId tile, int dest_row, int dest_col) {
    if (tile == TUI_TILE_NONE) return;
    /* Blit modifies the host canvas, so any pending draw commands
     * must be applied first to maintain correct stacking order. */
    flush_cmds();
    sys3(SYS_TUI_TILE_BLIT, (uint32_t)tile,
         (uint32_t)dest_row, (uint32_t)dest_col);
}

void tui_grab(int src_row, int src_col, int h, int w, TuiTileId dest_tile) {
    if (dest_tile == TUI_TILE_NONE) return;
    /* Grab reads the current canvas, so flush pending draws first. */
    flush_cmds();
    uint32_t rc = ((uint32_t)src_row << 16) | (uint32_t)(src_col & 0xffff);
    uint32_t hw = ((uint32_t)h << 16) | (uint32_t)(w & 0xffff);
    sys3(SYS_TUI_TILE_GRAB, (uint32_t)dest_tile, rc, hw);
}

/* ============================================================
 *  Frame control
 * ============================================================ */

void tui_present(void) {
    flush_cmds();
    sys0(SYS_TUI_PRESENT);
}

void tui_present_diff(void) {
    flush_cmds();
    sys0(SYS_TUI_PRESENT_DIFF);
}

/* ============================================================
 *  Input
 * ============================================================ */

bool tui_poll_event(TuiEvent *out) {
    if (!out) return false;
    static WireEvent wire;
    int32_t r = (int32_t)sys1(SYS_TUI_POLL_EVENT, (uint32_t)(uintptr_t)&wire);
    if (r <= 0) {
        out->kind = TUI_EV_NONE;
        return false;
    }
    /* Translate WireEvent → TuiEvent. */
    if (wire.kind == EVK_KEY) {
        out->kind = TUI_EV_KEY;
        out->key.key = wire.key;
        out->key.mods = wire.mods;
        out->key.raw = (wire.key < 256) ? (char)wire.key : 0;
        return true;
    } else if (wire.kind == EVK_MOUSE) {
        out->kind = TUI_EV_MOUSE;
        out->mouse.row = wire.row;
        out->mouse.col = wire.col;
        out->mouse.button = wire.button;
        out->mouse.mods = wire.mods;
        out->mouse.press = !!(wire.flags & EVF_PRESS);
        out->mouse.drag  = !!(wire.flags & EVF_DRAG);
        return true;
    }
    out->kind = TUI_EV_NONE;
    return false;
}

/* ============================================================
 *  Button widget (round D.2)
 * ============================================================ */

bool tui_button_hit(const TuiButton *b, int row, int col) {
    return row >= b->row && row < b->row + b->h &&
           col >= b->col && col < b->col + b->w;
}

void tui_button_draw(const TuiButton *b) {
    TuiColor fg = b->pressed ? b->pressed_fg : b->fg;
    TuiColor bg = b->pressed ? b->pressed_bg : b->bg;

    /* Draw the button body as a filled rectangle. */
    tui_set_fg(fg);
    tui_set_bg(bg);
    tui_fill_rect(b->row, b->col, b->h, b->w, ' ');

    /* A subtle drop-shadow / border via reverse-video corners,
     * but only on idle — when pressed we drop the border to give
     * a "sunken" feel without needing separate drawing. */
    if (!b->pressed) {
        /* Light top-edge using upper half block */
        for (int c = 0; c < b->w; c++) {
            tui_set_cell(b->row, b->col + c,
                         TUI_BLOCK_LOWER_HALF, bg, TUI_DEFAULT_COLOR, 0);
        }
        /* Dark bottom-edge using lower half block */
        for (int c = 0; c < b->w; c++) {
            tui_set_cell(b->row + b->h - 1, b->col + c,
                         TUI_BLOCK_UPPER_HALF, bg, TUI_DEFAULT_COLOR, 0);
        }
    }

    /* Label centered in the bounding box. */
    if (b->label) {
        int lbl_len = 0;
        for (const char *p = b->label; *p; p++) lbl_len++;
        if (lbl_len > b->w - 2) lbl_len = b->w - 2;
        int label_col = b->col + (b->w - lbl_len) / 2;
        int label_row = b->row + (b->h - 1) / 2;
        /* When pressed, nudge the label down one row to simulate
         * "the button moved." If h==1 we skip the nudge. */
        if (b->pressed && b->h > 2) label_row++;
        tui_set_fg(fg);
        tui_set_bg(bg);
        tui_set_attr(TUI_ATTR_BOLD);
        tui_move(label_row, label_col);
        for (int i = 0; i < lbl_len; i++) tui_putc(b->label[i]);
        tui_set_attr(0);
    }
    tui_reset();
}

TuiButtonResult tui_button_handle(TuiButton *b, const TuiEvent *ev) {
    if (!b || !ev || ev->kind != TUI_EV_MOUSE) return TUI_BTN_NONE;

    int hit = tui_button_hit(b, ev->mouse.row, ev->mouse.col);

    /* Press: arm the button if pressed inside its bounds. */
    if (ev->mouse.press && !ev->mouse.drag) {
        if (hit) {
            if (!b->pressed) {
                b->armed = 1;
                b->pressed = 1;
                return TUI_BTN_REDRAW;
            }
        }
        return TUI_BTN_NONE;
    }

    /* Drag: update pressed visual to reflect current hover state
     * while armed. If the user drags off the button while armed,
     * we un-press (still armed, just unpressed visually). Drag
     * back onto the button re-presses. Standard desktop UX. */
    if (ev->mouse.drag) {
        if (b->armed) {
            int want = hit;
            if (want != b->pressed) {
                b->pressed = want;
                return TUI_BTN_REDRAW;
            }
        }
        return TUI_BTN_NONE;
    }

    /* Release: if armed and released over the button, that's a
     * click. Otherwise just clear armed state. */
    if (!ev->mouse.press) {
        if (b->armed) {
            int was_pressed = b->pressed;
            b->armed = 0;
            b->pressed = 0;
            if (hit) {
                /* Caller will likely teardown the menu so don't
                 * worry about returning REDRAW too — CLICKED is
                 * the strongest signal. */
                return TUI_BTN_CLICKED;
            }
            if (was_pressed) return TUI_BTN_REDRAW;
        }
    }
    return TUI_BTN_NONE;
}
