/* ============================================================
 *  vm_host_tui_input.c — input parser for the TUI service.
 *
 *  Reads bytes from the session's transport (or, as a fallback,
 *  the legacy stdio non-blocking reader), maintains a small ring
 *  buffer in the session struct, and runs a CSI / SS3 / escape-
 *  sequence state machine. Decoded events come back through the
 *  vm_host_tui_poll_input() entry point.
 *
 *  Architecture mirrors the original guest-side parser in
 *  examples/common/guest/tui.c: IN_STATE_GROUND / IN_STATE_ESC /
 *  IN_STATE_CSI / IN_STATE_CSI_O, CSI parameter list, SGR mouse
 *  (mode 1006), arrows, function keys F1-F12, modifiers via the
 *  second CSI parameter.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_host_tui_internal.h"
#include "vm/vm_host_stdio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* IN_BUF_CAP aliases the session-struct constant. */
#define IN_BUF_CAP VM_TUI_IN_BUF_CAP

enum {
    IN_STATE_GROUND = 0,
    IN_STATE_ESC,
    IN_STATE_CSI,
    IN_STATE_CSI_O,
};

/* MAX_CSI_PARAMS aliases the session-struct constant. */
#define MAX_CSI_PARAMS VM_TUI_MAX_CSI_PARAMS

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
    /* Pull bytes from THIS session's transport rather than the
     * process default. Falls back to the legacy stdio helper if
     * no transport is bound to this session (which then itself
     * consults the default). */
    VmHostTransport *t = hresolve_transport();
    int r;
    if (t && t->read_nonblock) {
        r = t->read_nonblock(t, g_in_buf + g_in_tail, avail);
    } else {
        r = vm_host_stdio_read_bytes_nonblock(g_in_buf + g_in_tail, avail);
    }
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
                /* (b & 3) == 3: "no button". In any-motion tracking
                 * (xterm mode 1003) the terminal reports bare cursor
                 * movement with this code and the motion bit (32)
                 * set. Treat it as a motion event with no button —
                 * exactly what a move-to-steer UI needs. Without
                 * this, every bare-motion report was dropped and
                 * the cursor-following game saw nothing. */
                button = VM_TUI_MB_NONE;
                break;
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

/* Pump the state machine; produces at most one event. Returns true
 * if an event was produced. */
bool vm_host_tui_poll_input(InEvent *out) {
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

    /* Bare ESC: after two consecutive polls with no follow-up bytes,
     * treat as ESC-key-pressed. */
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

/* Marshal a host-side InEvent into the guest-visible wire record.
 * The structure layout is fixed by vm_host_tui.h. */
void vm_host_tui_marshal_event(const InEvent *src, VmTuiEventRecord *dst) {
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

/* Reset the input parser's ring + state machine. Called by do_init
 * (in vm_host_tui.c) on session shutdown so a fresh init doesn't see
 * stale bytes from the previous session. */
void vm_host_tui_input_reset_(void) {
    g_in_head = 0; g_in_tail = 0;
    g_in_state = IN_STATE_GROUND;
    csi_reset();
    g_esc_idle_polls = 0;
}

/* Test-only: inject bytes into the input ring buffer as if they
 * arrived from stdin. Returns the number of bytes accepted (0 if
 * the buffer is full). Used by test_vm_host_tui.c to exercise the
 * parser without a real TTY. */
unsigned vm_host_tui_test_inject_input_(const void *bytes, unsigned n) {
    const uint8_t *b = (const uint8_t *)bytes;
    if (g_in_head == g_in_tail) g_in_head = g_in_tail = 0;
    unsigned avail = IN_BUF_CAP - g_in_tail;
    if (n > avail) n = avail;
    for (unsigned i = 0; i < n; i++) g_in_buf[g_in_tail + i] = b[i];
    g_in_tail += n;
    return n;
}
