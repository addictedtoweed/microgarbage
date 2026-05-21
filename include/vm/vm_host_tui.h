/* ============================================================
 *  vm_host_tui.h — terminal-canvas service for guest VMs
 *
 *  Moves the back-buffered canvas + diff-present + input parser
 *  out of every guest into a shared host module. Guests submit
 *  batched drawing commands; the host renders to stdout.
 *
 *  Why
 *  -----------------------------------------------------------
 *  Before round T.3 the TUI library lived in each guest ELF:
 *  ~10 KB of code + ~42 KB of canvas BSS per guest using it.
 *  With the canvas in the host, that cost drops to ~200 bytes
 *  per guest (command-buffer builders), and the canvas state is
 *  paid once host-side regardless of how many guests use TUI.
 *
 *  Ownership model
 *  -----------------------------------------------------------
 *  One VM at a time owns the canvas. SYS_TUI_INIT records the
 *  caller's vm_id as the owner; subsequent SYS_TUI_* calls from
 *  a non-owner return -EBUSY. SYS_TUI_SHUTDOWN releases. If the
 *  owning VM exits without calling shutdown, vm_host_tui_release
 *  must be invoked from the unload path (vm_system handles this).
 *
 *  Matches real-terminal foreground semantics: only one app
 *  controls the screen at a time. A future window-manager guest
 *  would mediate ownership by being the canvas owner itself and
 *  multiplexing input/output to its children.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef MICROGARBAGE_VM_HOST_TUI_H
#define MICROGARBAGE_VM_HOST_TUI_H

#include "vm/vm_system.h"

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  Compile-time tuning
 * ============================================================ */

#ifndef VM_TUI_MAX_ROWS
#define VM_TUI_MAX_ROWS 30
#endif
#ifndef VM_TUI_MAX_COLS
#define VM_TUI_MAX_COLS 80
#endif

/* Draw-command buffer cap. Guests pass a smaller buffer in
 * SYS_TUI_FLUSH_DRAW; this is the cap on what we accept in a
 * single flush call. Sized for a moderately complex frame:
 * ~100 SET_CELL ops at 7 bytes each = 700 bytes, plus a few
 * PRINT ops with strings = 1024 is comfortable. */
#ifndef VM_TUI_MAX_FLUSH_BYTES
#define VM_TUI_MAX_FLUSH_BYTES 4096
#endif

/* ============================================================
 *  Init flags (mirror tui.h TuiInitFlags so the guest header
 *  can pass them through verbatim).
 * ============================================================ */

#define VM_TUI_USE_ALT_SCREEN      (1u << 0)
#define VM_TUI_USE_RAW             (1u << 1)
#define VM_TUI_HIDE_CURSOR         (1u << 2)
#define VM_TUI_USE_MOUSE           (1u << 3)
#define VM_TUI_USE_SYNC_OUTPUT     (1u << 4)

/* ============================================================
 *  Cell + color types — mirrored from the guest tui.h so the
 *  draw-command wire format matches byte-for-byte.
 * ============================================================ */

/* Color codes 0-15 plus a sentinel for "use terminal default". */
#define VM_TUI_DEFAULT_COLOR 256

#define VM_TUI_ATTR_NONE      0u
#define VM_TUI_ATTR_BOLD      (1u << 0)
#define VM_TUI_ATTR_DIM       (1u << 1)
#define VM_TUI_ATTR_UNDERLINE (1u << 2)
#define VM_TUI_ATTR_REVERSE   (1u << 3)

/* ============================================================
 *  Draw-command wire format
 *
 *  Each command in the flush buffer starts with a 1-byte opcode.
 *  Payload follows in the bytes immediately after.
 *
 *  All multi-byte fields are little-endian. Coordinates are
 *  1-indexed, ANSI convention. The host clamps drawing to the
 *  canvas bounds; out-of-range cells are silently dropped.
 * ============================================================ */

enum {
    VM_TUI_OP_END         = 0,   /* no payload — terminates the buffer */
    VM_TUI_OP_SET_CELL    = 1,   /* { row(u16), col(u16), c(u8),
                                     fg(u16), bg(u16), attrs(u8) } */
    VM_TUI_OP_FILL_RECT   = 2,   /* { row, col, h, w (each u16), c(u8) } */
    VM_TUI_OP_PRINT       = 3,   /* { row(u16), col(u16),
                                     fg(u16), bg(u16), attrs(u8),
                                     len(u16), bytes[len] } */
    VM_TUI_OP_BOX         = 4,   /* { row, col, h, w (each u16),
                                     style(u8) } — 0=single, 1=double, 2=ascii */
    VM_TUI_OP_MOVE        = 5,   /* { row(u16), col(u16) } */
    VM_TUI_OP_SET_FG      = 6,   /* { color(u16) } */
    VM_TUI_OP_SET_BG      = 7,   /* { color(u16) } */
    VM_TUI_OP_SET_ATTR    = 8,   /* { attrs(u8) } */
    VM_TUI_OP_CLEAR       = 9,   /* no payload */
    VM_TUI_OP_SET_CLIP    = 10,  /* { row, col, h, w } */
    VM_TUI_OP_CLEAR_CLIP  = 11,  /* no payload */
    VM_TUI_OP_PUTC        = 12,  /* { c(u8) } — write at current cursor */
    VM_TUI_OP_PUTS        = 13,  /* { len(u16), bytes[len] } */
    /* 14-31 reserved for future ops within T.3 */
};

/* ============================================================
 *  Event-record wire format (host → guest, for SYS_TUI_POLL_EVENT)
 *
 *  16 bytes, packed:
 *
 *    u8  kind           VM_TUI_EVK_*
 *    u8  reserved
 *    u16 key            decoded key or VM_TUI_KEY_*
 *    u8  mods
 *    u8  button         (mouse: VM_TUI_MB_*)
 *    u16 row            (mouse coords)
 *    u16 col
 *    u8  flags          (bit 0: press, bit 1: drag)
 *    u8  reserved
 *    u32 reserved
 * ============================================================ */

#define VM_TUI_EVK_NONE   0
#define VM_TUI_EVK_KEY    1
#define VM_TUI_EVK_MOUSE  2
#define VM_TUI_EVK_PASTE  3
#define VM_TUI_EVK_RESIZE 4

/* Symbolic key values >= 256 (same numbering as guest tui.h). */
#define VM_TUI_KEY_ENTER     256
#define VM_TUI_KEY_BACKSPACE 257
#define VM_TUI_KEY_TAB       258
#define VM_TUI_KEY_ESCAPE    259
#define VM_TUI_KEY_UP        260
#define VM_TUI_KEY_DOWN      261
#define VM_TUI_KEY_LEFT      262
#define VM_TUI_KEY_RIGHT     263
#define VM_TUI_KEY_HOME      264
#define VM_TUI_KEY_END       265
#define VM_TUI_KEY_PAGE_UP   266
#define VM_TUI_KEY_PAGE_DOWN 267
#define VM_TUI_KEY_INSERT    268
#define VM_TUI_KEY_DELETE    269
#define VM_TUI_KEY_F1        270
/* F2..F12 follow */

#define VM_TUI_MB_NONE       0
#define VM_TUI_MB_LEFT       1
#define VM_TUI_MB_MIDDLE     2
#define VM_TUI_MB_RIGHT      3
#define VM_TUI_MB_WHEEL_UP   4
#define VM_TUI_MB_WHEEL_DOWN 5

#define VM_TUI_EVF_PRESS   (1u << 0)
#define VM_TUI_EVF_DRAG    (1u << 1)

typedef struct {
    uint8_t  kind;          /* VM_TUI_EVK_* */
    uint8_t  reserved0;
    uint16_t key;           /* decoded key or VM_TUI_KEY_* */
    uint8_t  mods;
    uint8_t  button;
    uint16_t row;
    uint16_t col;
    uint8_t  flags;         /* press / drag */
    uint8_t  reserved1;
    uint32_t reserved2;
} VmTuiEventRecord;

/* ============================================================
 *  Installation
 * ============================================================ */

/* Install the TUI handlers on `sys`. Returns false if any
 * handler registration fails. The host's stdout is assumed
 * to be fd 1 (so all canvas output flows through the same
 * place as printf). */
bool vm_host_install_tui(VmSystem *sys);

/* Called by vm_system_unload_vm when a VM that owns the
 * canvas exits without calling SYS_TUI_SHUTDOWN. Restores
 * the terminal and releases ownership. Safe to call when
 * the VM didn't own the canvas. Also frees any tiles the
 * VM allocated. */
void vm_host_tui_release_for_vm(uint16_t vm_id);

/* ============================================================
 *  Tile subsystem (round T.3b)
 *
 *  Tiles are per-VM sub-canvases. Each VM has up to
 *  VM_TUI_TILES_PER_VM slots; cells come from a shared host
 *  arena (VM_TUI_TILE_ARENA_BYTES). The handle is opaque:
 *  internally (slot << 16) | generation, with the generation
 *  bumped on each create so a stale handle from a destroyed
 *  tile cannot accidentally alias a freshly-created one.
 *
 *  Layout per cell on the host: same as the canvas HostCell
 *  (1B char + 2B fg + 2B bg + 1B attrs + 1B flags = 7B).
 *
 *  The transparent flag lives in the cell's flags field; blit
 *  honors it (skip the canvas cell where the tile cell is
 *  transparent).
 * ============================================================ */

#ifndef VM_TUI_TILES_PER_VM
#define VM_TUI_TILES_PER_VM 16
#endif

#ifndef VM_TUI_TILE_ARENA_BYTES
/* 16 KB worth of cells = ~2200 cells = enough for, e.g.,
 * 8 tetris piece tiles (4×4 = 16 cells × 8 = 128 cells)
 * + a 40×20 chrome tile (800 cells) + a few captures. */
#define VM_TUI_TILE_ARENA_BYTES 16384
#endif

#define VM_TUI_CELL_TRANSPARENT (1u << 0)

/* ============================================================
 *  Unicode glyph indices (0x80..0xBF)
 *
 *  Cells whose `c` byte is in this range get rendered as the
 *  corresponding UTF-8 glyph by the host's emit path. Cells
 *  with c < 0x80 emit as plain ASCII. This keeps cells 1 byte
 *  wide while letting games use block characters, half-blocks,
 *  bullets, and triangles without breaking the wire format.
 * ============================================================ */

#define VM_TUI_GLYPH_BLOCK_FULL        0x80  /* █ */
#define VM_TUI_GLYPH_BLOCK_UPPER_HALF  0x81  /* ▀ */
#define VM_TUI_GLYPH_BLOCK_LOWER_HALF  0x82  /* ▄ */
#define VM_TUI_GLYPH_BLOCK_LEFT_HALF   0x83  /* ▌ */
#define VM_TUI_GLYPH_BLOCK_RIGHT_HALF  0x84  /* ▐ */
#define VM_TUI_GLYPH_SHADE_LIGHT       0x85  /* ░ */
#define VM_TUI_GLYPH_SHADE_MEDIUM      0x86  /* ▒ */
#define VM_TUI_GLYPH_SHADE_DARK        0x87  /* ▓ */
#define VM_TUI_GLYPH_BULLET            0x88  /* ● */
#define VM_TUI_GLYPH_TRIANGLE_UP       0x89  /* ▲ */
#define VM_TUI_GLYPH_TRIANGLE_DOWN     0x8A  /* ▼ */
#define VM_TUI_GLYPH_DIAMOND           0x8B  /* ◆ */
#define VM_TUI_GLYPH_DOUBLE_HORIZ      0x90  /* ═ */
#define VM_TUI_GLYPH_DOUBLE_VERT       0x91  /* ║ */
#define VM_TUI_GLYPH_BULLET_OPEN       0x92  /* ○ */
#define VM_TUI_GLYPH_SQUARE_FILLED     0x93  /* ■ */
#define VM_TUI_GLYPH_SQUARE_EMPTY      0x94  /* □ */
#define VM_TUI_GLYPH_TRIANGLE_LEFT     0x95  /* ◀ */
#define VM_TUI_GLYPH_TRIANGLE_RIGHT    0x96  /* ▶ */

#ifdef __cplusplus
}
#endif

#endif /* MICROGARBAGE_VM_HOST_TUI_H */
