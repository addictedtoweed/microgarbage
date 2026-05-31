/* ============================================================
 *  vm_host_tui_tile.c — tile subsystem for the TUI service.
 *
 *  Tiles are per-VM sub-canvases. Each VM has up to
 *  VM_TUI_TILES_PER_VM slots; cells come from a shared host
 *  arena (VM_TUI_TILE_ARENA_BYTES). The handle is opaque:
 *  internally (slot << 16) | generation, with the generation
 *  bumped on each create so a stale handle from a destroyed
 *  tile cannot accidentally alias a freshly-created one.
 *
 *  Storage:
 *    g_tile_arena   one shared cell pool, served bump-style with
 *                   no per-tile free (matches the guest-side
 *                   tile arena's behavior). VM unload compacts
 *                   the arena by reclaiming every range that
 *                   belonged to the dead VM.
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
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_host_tui_internal.h"

#include "vm/vm_ecall.h"
#include "vm/vm_sched.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ============================================================
 *  Storage
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

/* ============================================================
 *  Internals
 * ============================================================ */
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
    /* Arena memory is not reclaimed individually; we'll compact on
     * full release_for_vm. */
    return 0;
}

void vm_host_tui_tile_release_for_vm(uint16_t vm_id) {
    for (uint8_t i = 0; i < VM_TUI_TILES_PER_VM; i++) {
        g_tile_slots[vm_id][i].in_use = false;
    }
    /* Compact the arena: walk all VMs and move surviving allocations
     * down. O(total tiles * arena_size) worst case, but tile
     * creates/destroys are rare events. */
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
            /* Clear transparent bit on the canvas — it has no meaning
             * on canvas cells. */
            g_canvas[dr - 1][dc - 1].flags = 0;
        }
    }
    return 0;
}

static int32_t tile_grab(TileSlot *s, int src_row, int src_col, int h, int w) {
    /* Copy a rectangle from the canvas into the tile, clamping to
     * both the tile dims and canvas bounds. Cells outside the
     * canvas become transparent in the destination. */
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
 *
 *  Every handler:
 *    1. tui_enter(vm_id) — sets up cur_session()
 *    2. checks g_owner_vm matches (the calling VM must own the
 *       canvas — tiles are scoped per session)
 *    3. dispatches to the static tile_* primitive above
 * ============================================================ */

static void handle_tile_create(VmCpu *cpu, void *system) {
    (void)system;
    tui_enter(cpu->vm_id);
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
    tui_enter(cpu->vm_id);
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
    tui_enter(cpu->vm_id);
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
    tui_enter(cpu->vm_id);
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
    tui_enter(cpu->vm_id);
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
    tui_enter(cpu->vm_id);
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
    tui_enter(cpu->vm_id);
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
    tui_enter(cpu->vm_id);
    if (g_owner_vm != cpu->vm_id) {
        cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBUSY;
        return;
    }
    TileSlot *s = resolve_handle(cpu->vm_id, cpu->regs[VM_REG_A0]);
    if (!s) { cpu->regs[VM_REG_A0] = (uint32_t)-VM_EBADF; return; }
    cpu->regs[VM_REG_A0] = ((uint32_t)s->rows << 16) | (uint32_t)s->cols;
}

/* ============================================================
 *  Install / unregister
 * ============================================================ */

bool vm_host_tui_install_tile_handlers(VmSystem *sys) {
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_CREATE,
                           handle_tile_create)) goto fail;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_DESTROY,
                           handle_tile_destroy)) goto fail_create;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_SET,
                           handle_tile_set)) goto fail_destroy;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_FILL,
                           handle_tile_fill)) goto fail_set;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_SET_TRANSPARENT,
                           handle_tile_set_transparent)) goto fail_fill;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_BLIT,
                           handle_tile_blit)) goto fail_strans;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_GRAB,
                           handle_tile_grab)) goto fail_blit;
    if (!vm_ecall_register(sys->ecall_router, SYS_TUI_TILE_DIMS,
                           handle_tile_dims)) goto fail_grab;
    return true;

fail_grab:    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_GRAB);
fail_blit:    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_BLIT);
fail_strans:  vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_SET_TRANSPARENT);
fail_fill:    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_FILL);
fail_set:     vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_SET);
fail_destroy: vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_DESTROY);
fail_create:  vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_CREATE);
fail:         return false;
}

void vm_host_tui_unregister_tile_handlers(VmSystem *sys) {
    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_DIMS);
    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_GRAB);
    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_BLIT);
    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_SET_TRANSPARENT);
    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_FILL);
    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_SET);
    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_DESTROY);
    vm_ecall_unregister(sys->ecall_router, SYS_TUI_TILE_CREATE);
}
