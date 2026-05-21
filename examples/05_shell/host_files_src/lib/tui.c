/* lib/tui.c — guest-side shim for the host TUI service.
 *
 * As of round T.4, the canvas and SGR-emit logic live in the
 * host. This file is now a thin command-buffer builder + flush.
 * The public API (tui_init, tui_set_cell, etc.) is unchanged so
 * existing guests (snake.c, future tetris.c) compile without
 * modification.
 *
 * Size impact: this file used to be ~1100 lines + ~80 KB of
 * canvas BSS per guest. Now it's ~300 lines and ~2 KB of
 * command-buffer BSS.
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
     * For T.4 we keep text_block simple: it emits with default
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
 *  Tiles — guest-side, expand to SET_CELL ops at blit time
 *
 *  Round T.3b will move tiles into the host. For now, keep the
 *  tile data in guest RAM (small arena) so source compatibility
 *  is preserved.
 * ============================================================ */

typedef struct {
    char     c;
    uint16_t fg;
    uint16_t bg;
    uint8_t  attrs;
    uint8_t  flags;    /* TUI_CELL_TRANSPARENT */
} GuestCell;

#define TILE_ARENA_BYTES TUI_TILE_ARENA_BYTES
#define TILE_MAX_COUNT   TUI_TILE_MAX_COUNT

typedef struct {
    int rows, cols;
    GuestCell *cells;     /* into g_tile_arena */
    bool in_use;
} TileSlot;

static uint8_t   g_tile_arena[TILE_ARENA_BYTES];
static unsigned  g_tile_arena_used = 0;
static TileSlot  g_tile_slots[TILE_MAX_COUNT];

TuiTileId tui_tile_create(int rows, int cols) {
    if (rows <= 0 || cols <= 0) return TUI_TILE_NONE;
    unsigned need = (unsigned)rows * (unsigned)cols * sizeof(GuestCell);
    if (g_tile_arena_used + need > TILE_ARENA_BYTES) return TUI_TILE_NONE;
    int slot = -1;
    for (int i = 0; i < TILE_MAX_COUNT; i++) {
        if (!g_tile_slots[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return TUI_TILE_NONE;
    g_tile_slots[slot].rows = rows;
    g_tile_slots[slot].cols = cols;
    g_tile_slots[slot].cells = (GuestCell *)(g_tile_arena + g_tile_arena_used);
    g_tile_slots[slot].in_use = true;
    g_tile_arena_used += need;
    /* Initialize with transparent cells. */
    for (unsigned i = 0; i < (unsigned)(rows * cols); i++) {
        g_tile_slots[slot].cells[i].c = ' ';
        g_tile_slots[slot].cells[i].fg = TUI_DEFAULT_COLOR;
        g_tile_slots[slot].cells[i].bg = TUI_DEFAULT_COLOR;
        g_tile_slots[slot].cells[i].attrs = 0;
        g_tile_slots[slot].cells[i].flags = TUI_CELL_TRANSPARENT;
    }
    return (TuiTileId)slot;
}

void tui_tile_destroy(TuiTileId tile) {
    if (tile < 0 || tile >= TILE_MAX_COUNT) return;
    g_tile_slots[tile].in_use = false;
    /* arena memory not reclaimed — same behavior as old impl */
}

void tui_tile_set(TuiTileId tile, int row, int col,
                  char c, TuiColor fg, TuiColor bg, unsigned attrs) {
    if (tile < 0 || tile >= TILE_MAX_COUNT) return;
    TileSlot *s = &g_tile_slots[tile];
    if (!s->in_use) return;
    if (row < 1 || row > s->rows || col < 1 || col > s->cols) return;
    GuestCell *cell = &s->cells[(row - 1) * s->cols + (col - 1)];
    cell->c = c;
    cell->fg = (uint16_t)fg;
    cell->bg = (uint16_t)bg;
    cell->attrs = (uint8_t)attrs;
    cell->flags = 0;
}

void tui_tile_set_transparent(TuiTileId tile, int row, int col) {
    if (tile < 0 || tile >= TILE_MAX_COUNT) return;
    TileSlot *s = &g_tile_slots[tile];
    if (!s->in_use) return;
    if (row < 1 || row > s->rows || col < 1 || col > s->cols) return;
    s->cells[(row - 1) * s->cols + (col - 1)].flags = TUI_CELL_TRANSPARENT;
}

void tui_tile_fill(TuiTileId tile, char c, TuiColor fg, TuiColor bg,
                   unsigned attrs) {
    if (tile < 0 || tile >= TILE_MAX_COUNT) return;
    TileSlot *s = &g_tile_slots[tile];
    if (!s->in_use) return;
    for (int i = 0; i < s->rows * s->cols; i++) {
        s->cells[i].c = c;
        s->cells[i].fg = (uint16_t)fg;
        s->cells[i].bg = (uint16_t)bg;
        s->cells[i].attrs = (uint8_t)attrs;
        s->cells[i].flags = 0;
    }
}

void tui_blit_tile(TuiTileId tile, int dest_row, int dest_col) {
    if (tile < 0 || tile >= TILE_MAX_COUNT) return;
    TileSlot *s = &g_tile_slots[tile];
    if (!s->in_use) return;
    for (int r = 0; r < s->rows; r++) {
        for (int c = 0; c < s->cols; c++) {
            GuestCell *cell = &s->cells[r * s->cols + c];
            if (cell->flags & TUI_CELL_TRANSPARENT) continue;
            tui_set_cell(dest_row + r, dest_col + c,
                         cell->c, (TuiColor)cell->fg,
                         (TuiColor)cell->bg, cell->attrs);
        }
    }
}

void tui_grab(int src_row, int src_col, int h, int w, TuiTileId dest_tile) {
    /* Grab is not supported without a guest-side canvas view.
     * In a future round (T.3b) the host will expose SYS_TUI_GRAB. */
    (void)src_row; (void)src_col; (void)h; (void)w; (void)dest_tile;
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
