/* ============================================================
 *  vm_host_tui_internal.h — shared internals for vm_host_tui.c
 *  and its split-off siblings (input parser, tile subsystem).
 *
 *  NOT a public header: nothing under include/ should include
 *  this. Lives next to the .c files that include it. Exposes the
 *  minimum each split-off file needs:
 *
 *    HostCell           short alias for VmTuiHostCell
 *    cur_session()      vm_id -> VmTuiSession lookup
 *    tui_enter/tui_leave  set/clear the current-VM context
 *    g_* macros         session-state shims (the ~160 access
 *                       sites use these instead of touching
 *                       cur_session() directly)
 *    hresolve_transport / hwrite / hflush_stdout_if_default
 *                       transport-routing helpers for output
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#ifndef VM_HOST_TUI_INTERNAL_H
#define VM_HOST_TUI_INTERNAL_H

#include "vm/vm_host_tui.h"
#include "vm/vm_host_transport.h"
#include "vm/vm_core.h"
#include "vm/vm_system.h"   /* VmSystem (typedef, can't forward-decl) */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Short alias for VmTuiHostCell. Used pervasively in the implementation
 * files; keeping the alias here keeps the ~100 references one-namespace. */
typedef VmTuiHostCell HostCell;

/* The current VM whose ECALL is executing. Set by tui_enter at
 * handler entry, cleared by tui_leave on exit. cur_session() reads it. */
extern uint16_t vm_host_tui_g_current_vm;
static inline void tui_enter(uint16_t vm_id) { vm_host_tui_g_current_vm = vm_id; }
static inline void tui_leave(void)           { vm_host_tui_g_current_vm = UINT16_MAX; }

/* Returns the session struct for the VM whose handler is currently
 * running. Defined in vm_host_tui.c. Always returns a non-NULL pointer
 * (the fallback singleton if no pool was registered) so the g_* macros
 * never dereference NULL. */
VmTuiSession *cur_session(void);

/* g_* shims for cur_session()->FIELD. The implementation files use
 * the ~26 macros below extensively rather than re-typing
 * cur_session()->X every time. Correctness rests on tui_enter being
 * called before any of them are touched — every ECALL handler does
 * this at its top. */
#define g_owner_vm           (cur_session()->owner_vm)
#define g_initialized        (cur_session()->initialized)
#define g_flags              (cur_session()->flags)
#define g_rows               (cur_session()->rows)
#define g_cols               (cur_session()->cols)
#define g_raw_mode_we_set    (cur_session()->raw_mode_we_set)
#define g_pen_fg             (cur_session()->pen_fg)
#define g_pen_bg             (cur_session()->pen_bg)
#define g_pen_attrs          (cur_session()->pen_attrs)
#define g_cur_row            (cur_session()->cur_row)
#define g_cur_col            (cur_session()->cur_col)
#define g_clip_r             (cur_session()->clip_r)
#define g_clip_c             (cur_session()->clip_c)
#define g_clip_h             (cur_session()->clip_h)
#define g_clip_w             (cur_session()->clip_w)
#define g_canvas             (cur_session()->canvas)
#define g_front              (cur_session()->front)
#define g_front_valid        (cur_session()->front_valid)
#define g_in_buf             (cur_session()->in_buf)
#define g_in_head            (cur_session()->in_head)
#define g_in_tail            (cur_session()->in_tail)
#define g_in_state           (cur_session()->in_state)
#define g_csi_params         (cur_session()->csi_params)
#define g_csi_n_params       (cur_session()->csi_n_params)
#define g_csi_curr           (cur_session()->csi_curr)
#define g_csi_has_curr       (cur_session()->csi_has_curr)
#define g_csi_intermediate   (cur_session()->csi_intermediate)
#define g_esc_idle_polls     (cur_session()->esc_idle_polls)

/* Transport-routing helpers. Defined in vm_host_tui.c so all split-off
 * files share one implementation. */
VmHostTransport *hresolve_transport(void);
void             hflush_stdout_if_default(void);
void             hwrite(int fd, const void *p, size_t n);

/* ----------------------------------------------------------------
 * Input parser exports (vm_host_tui_input.c)
 *
 * The input parser owns its session-state via the g_in_* shims
 * above; it exposes a small surface to the ECALL handlers in
 * vm_host_tui.c.
 * ---------------------------------------------------------------- */

/* Modifier flags from the CSI second parameter — same encoding the
 * guest tui.h uses (shift=1, alt=2, ctrl=4). */
#define MOD_SHIFT (1u << 0)
#define MOD_ALT   (1u << 1)
#define MOD_CTRL  (1u << 2)

/* Host-side intermediate representation of one decoded input event.
 * The poll loop drains bytes into one of these; SYS_TUI_POLL_EVENT
 * marshals it to the wire VmTuiEventRecord. */
typedef struct {
    uint8_t kind;       /* VM_TUI_EVK_* */
    int     key;        /* decoded key (or VM_TUI_KEY_*) */
    int     mods;       /* MOD_* bitmask */
    int     row;        /* mouse coord (1-indexed) */
    int     col;
    int     button;     /* VM_TUI_MB_* */
    bool    press;      /* mouse press vs release */
    bool    drag;       /* mouse motion-with-button-held */
} InEvent;

/* Poll one event out of the input ring; returns true if `out` was
 * filled. False = no event ready right now (caller polls again). */
bool vm_host_tui_poll_input(InEvent *out);

/* Marshal a host-side InEvent into the guest's wire format. */
void vm_host_tui_marshal_event(const InEvent *src, VmTuiEventRecord *dst);

/* Reset the input parser's ring + state machine. Called on session
 * shutdown so a fresh init doesn't see stale bytes. */
void vm_host_tui_input_reset_(void);

/* ----------------------------------------------------------------
 * Canvas bounds checks (vm_host_tui.c)
 *
 * Exposed for the tile module's blit/grab — both need to know
 * whether a (row, col) is on the canvas at all (in_canvas) or
 * actually drawable given the active clip rect (drawable).
 * ---------------------------------------------------------------- */
bool in_canvas(int row, int col);
bool drawable(int row, int col);

/* ----------------------------------------------------------------
 * Tile subsystem exports (vm_host_tui_tile.c)
 *
 * The 8 SYS_TUI_TILE_* handlers + the per-VM arena reclaimer.
 * Install/unregister are called from the main TUI install chain
 * in vm_host_tui.c.
 * ---------------------------------------------------------------- */
bool vm_host_tui_install_tile_handlers(VmSystem *sys);
void vm_host_tui_unregister_tile_handlers(VmSystem *sys);

/* Reclaim all tiles owned by `vm_id` and compact the shared arena.
 * Called from vm_host_tui_release_for_vm on VM unload. */
void vm_host_tui_tile_release_for_vm(uint16_t vm_id);

#endif /* VM_HOST_TUI_INTERNAL_H */
