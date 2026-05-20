/* tui.c — implementation of the TUI library.
 *
 * Round K: direct-write drawing, basic 16-color, raw-mode input
 * parser with escape-sequence handling. No back buffer yet.
 *
 * The library talks to the host via the standard syscalls
 * (SYS_READ, SYS_WRITE, SYS_FFLUSH, SYS_TTY_SET_RAW). The host
 * doesn't need to know anything about TUI features — it just
 * shuffles bytes. This means a real STM32 deployment talking to
 * PuTTY over a UART works identically to a sandbox host process
 * talking to PuTTY over a named pipe.
 *
 * Public domain (CC0).
 */

#include "tui.h"

/* ============================================================
 *  Syscall stubs
 *
 *  Replicated here so the library is standalone — each guest
 *  that links tui.c gets these. Inline asm is tiny and the
 *  duplication is harmless.
 * ============================================================ */

#define SYS_READ              63
#define SYS_WRITE             64
#define SYS_FFLUSH            82
#define SYS_YIELD           1040
#define SYS_TICKS_NOW       1043
#define SYS_TICK_HZ         1044
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
 *  Internal state
 * ============================================================ */

static unsigned g_flags = 0;
static bool     g_initialized = false;
static bool     g_raw_active = false;

/* Clip rectangle (1-indexed). Defaults to a generous fullscreen
 * fallback. Round L will add a real terminal-size query (CSI 18 t
 * "what size are you?" or DECRPCR). */
static int g_clip_r = 1;
static int g_clip_c = 1;
static int g_clip_h = 50;       /* room for tall terminals */
static int g_clip_w = 200;

/* Cached terminal-side attributes so we can skip redundant SGR
 * emissions when nothing changed since the last cell. Round L's
 * back buffer will replace this with full diff-based emission. */
static TuiColor  g_cur_fg = TUI_DEFAULT_COLOR;
static TuiColor  g_cur_bg = TUI_DEFAULT_COLOR;
static unsigned  g_cur_attr = TUI_ATTR_NONE;
/* Set to true when we've issued ANY SGR sequence; lets tui_reset
 * skip the work if everything's already default. */
static bool      g_sgr_dirty = false;

/* ============================================================
 *  Tiny output buffer
 *
 *  Every tui_putc/tui_puts/tui_move/etc. appends to this buffer
 *  and tui_present() flushes it via one sys_write. This is a
 *  significant win over per-byte writes — each sys_write is an
 *  ECALL that's much more expensive than a memcpy. A 80x24 redraw
 *  with one byte per call would be 1920+ ecalls; buffered, it's
 *  one.
 *
 *  The buffer auto-flushes when it would overflow, so callers
 *  don't need to think about size. Round L's back-buffered
 *  rendering reduces the per-frame byte count substantially,
 *  making 4 KB plenty.
 * ============================================================ */

#define OUT_BUF_CAP 4096
static char g_out_buf[OUT_BUF_CAP];
static unsigned g_out_pos = 0;

static void out_flush(void) {
    if (g_out_pos == 0) return;
    /* Best-effort write; on a connected pipe this rarely fails.
     * If it does, output is lost — we don't have a great recovery
     * path. */
    sys_write(1, g_out_buf, g_out_pos);
    g_out_pos = 0;
}

static void out_byte(char c) {
    if (g_out_pos >= OUT_BUF_CAP) {
        out_flush();
    }
    g_out_buf[g_out_pos++] = c;
}

static void out_bytes(const char *p, unsigned n) {
    /* Fast path: fits whole. */
    if (g_out_pos + n <= OUT_BUF_CAP) {
        for (unsigned i = 0; i < n; i++) g_out_buf[g_out_pos + i] = p[i];
        g_out_pos += n;
        return;
    }
    /* Slow path: flush and write directly (for buffers larger than
     * the buffer itself, also covers the boundary case). */
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

/* Emit a decimal integer (positive only — negative would need a
 * sign which we never need in ANSI codes). */
static void out_dec(unsigned v) {
    char buf[12];
    char *p = buf + sizeof(buf);
    *--p = '\0';
    if (v == 0) {
        *--p = '0';
    } else {
        while (v) {
            *--p = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    out_str(p);
}

/* ============================================================
 *  Color and attribute emission
 * ============================================================ */

/* Map a TuiColor index (0-15) to the 30-37/90-97 ANSI code.
 * Returns 0 for invalid index. */
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

/* Emit the CSI sequence to apply fg/bg/attr. Tries to skip when
 * nothing changed since the last call, to keep escape-sequence
 * volume down. */
static void emit_sgr(TuiColor fg, TuiColor bg, unsigned attrs) {
    if (fg == g_cur_fg && bg == g_cur_bg && attrs == g_cur_attr) {
        return;
    }

    /* If we're going from a non-default state to all-default,
     * a single CSI 0 m is shortest. */
    if (fg == TUI_DEFAULT_COLOR && bg == TUI_DEFAULT_COLOR &&
        attrs == TUI_ATTR_NONE && g_sgr_dirty) {
        out_str("\x1b[0m");
        g_cur_fg = fg; g_cur_bg = bg; g_cur_attr = attrs;
        g_sgr_dirty = false;
        return;
    }

    /* General path: CSI 0;<attrs>;<fg>;<bg> m. Reset-first lets us
     * avoid having to track which attributes were previously set
     * and emit explicit OFFs for ones that are turning off. */
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

    g_cur_fg = fg; g_cur_bg = bg; g_cur_attr = attrs;
    g_sgr_dirty = true;
}

/* ============================================================
 *  Cursor positioning
 * ============================================================ */

static void emit_move(int row, int col) {
    /* CSI <row>;<col> H — both 1-indexed. */
    out_str("\x1b[");
    out_dec((unsigned)row);
    out_byte(';');
    out_dec((unsigned)col);
    out_byte('H');
}

/* ============================================================
 *  Public API
 * ============================================================ */

bool tui_init(unsigned flags) {
    if (g_initialized) return true;   /* already up */
    g_flags = flags;

    /* Raw mode first — affects how Enter/Ctrl-C are delivered
     * during the rest of init. Failure is non-fatal: the pipe
     * transport always returns false here because the host's
     * pipe-write side has no tty to operate on. */
    if (flags & TUI_USE_RAW) {
        int r = sys_tty_set_raw(1);
        g_raw_active = (r == 0);
        /* If the syscall failed but we're on a pipe, the pipe is
         * already byte-at-a-time. So g_raw_active stays false but
         * input still works. The flag just tells tui_shutdown
         * whether to undo the change. */
    }

    /* Alt screen: switches to a fresh buffer that doesn't pollute
     * the user's scrollback. CSI ?1049h is the modern variant
     * (xterm-style; preserves cursor pos so we restore correctly
     * on shutdown). */
    if (flags & TUI_USE_ALT_SCREEN) {
        out_str("\x1b[?1049h");
        /* Move to home, clear — the alt screen's contents are
         * undefined until we draw something. */
        out_str("\x1b[H\x1b[2J");
    }

    if (flags & TUI_HIDE_CURSOR) {
        out_str("\x1b[?25l");
    }

    /* (Round L: mouse / bracketed paste / sync-output enables
     * go here.) */

    out_flush();
    g_initialized = true;
    return true;
}

void tui_shutdown(void) {
    if (!g_initialized) return;

    /* Reset all SGR state to defaults so the user's terminal is
     * left clean. */
    out_str("\x1b[0m");
    g_cur_fg = TUI_DEFAULT_COLOR;
    g_cur_bg = TUI_DEFAULT_COLOR;
    g_cur_attr = TUI_ATTR_NONE;
    g_sgr_dirty = false;

    /* Undo in reverse order from init. */
    if (g_flags & TUI_HIDE_CURSOR) {
        out_str("\x1b[?25h");
    }
    if (g_flags & TUI_USE_ALT_SCREEN) {
        out_str("\x1b[?1049l");
    }
    /* (Round L undoes for mouse / bracketed paste / sync.) */

    out_flush();

    if (g_raw_active) {
        sys_tty_set_raw(0);
        g_raw_active = false;
    }

    g_initialized = false;
    g_flags = 0;
}

/* ============================================================
 *  Clip
 * ============================================================ */

void tui_set_clip(int row, int col, int h, int w) {
    if (row < 1) row = 1;
    if (col < 1) col = 1;
    if (h < 0) h = 0;
    if (w < 0) w = 0;
    g_clip_r = row;
    g_clip_c = col;
    g_clip_h = h;
    g_clip_w = w;
}

void tui_clear_clip(void) {
    g_clip_r = 1;
    g_clip_c = 1;
    g_clip_h = 50;
    g_clip_w = 200;
}

static bool clip_contains(int row, int col) {
    return row >= g_clip_r && row < g_clip_r + g_clip_h &&
           col >= g_clip_c && col < g_clip_c + g_clip_w;
}

/* ============================================================
 *  Drawing
 * ============================================================ */

void tui_move(int row, int col) {
    /* Clamp to clip on each axis — moving "into" the clip from
     * outside is fine, but the resulting cursor position must be
     * inside. */
    if (row < g_clip_r) row = g_clip_r;
    if (col < g_clip_c) col = g_clip_c;
    if (row >= g_clip_r + g_clip_h) row = g_clip_r + g_clip_h - 1;
    if (col >= g_clip_c + g_clip_w) col = g_clip_c + g_clip_w - 1;
    emit_move(row, col);
}

void tui_putc(char c) {
    out_byte(c);
}

void tui_puts(const char *s) {
    if (!s) return;
    out_str(s);
}

void tui_set_fg(TuiColor c) {
    emit_sgr(c, g_cur_bg, g_cur_attr);
}

void tui_set_bg(TuiColor c) {
    emit_sgr(g_cur_fg, c, g_cur_attr);
}

void tui_set_attr(unsigned attrs) {
    emit_sgr(g_cur_fg, g_cur_bg, attrs);
}

void tui_reset(void) {
    if (!g_sgr_dirty) return;
    out_str("\x1b[0m");
    g_cur_fg = TUI_DEFAULT_COLOR;
    g_cur_bg = TUI_DEFAULT_COLOR;
    g_cur_attr = TUI_ATTR_NONE;
    g_sgr_dirty = false;
}

void tui_set_cell(int row, int col, char c,
                  TuiColor fg, TuiColor bg, unsigned attrs) {
    if (!clip_contains(row, col)) return;
    emit_sgr(fg, bg, attrs);
    emit_move(row, col);
    out_byte(c);
}

void tui_clear(void) {
    /* Clipped clear: rewrite every cell in the clip with a space,
     * using current bg. This is what gives "clear" a meaningful
     * bg color rather than the terminal's default-erase behavior. */
    for (int r = g_clip_r; r < g_clip_r + g_clip_h; r++) {
        emit_move(r, g_clip_c);
        for (int c = 0; c < g_clip_w; c++) {
            out_byte(' ');
        }
    }
}

/* ============================================================
 *  Higher-level helpers
 *
 *  Box drawing uses Unicode box-drawing chars (U+2500 block).
 *  These are encoded as 3-byte UTF-8 sequences below. Modern
 *  terminals (PuTTY, Windows Terminal, anything from this decade)
 *  render them correctly with their default fonts. Truly old
 *  terminals would see garbage; if that's a concern, an ASCII
 *  fallback (+-|) is easy to add as a flag.
 * ============================================================ */

/* Single-line box: ┌─┐│└─┘├┤┬┴┼ */
static const char *BOX1_TL = "\xE2\x94\x8C";    /* ┌ */
static const char *BOX1_TR = "\xE2\x94\x90";    /* ┐ */
static const char *BOX1_BL = "\xE2\x94\x94";    /* └ */
static const char *BOX1_BR = "\xE2\x94\x98";    /* ┘ */
static const char *BOX1_H  = "\xE2\x94\x80";    /* ─ */
static const char *BOX1_V  = "\xE2\x94\x82";    /* │ */

/* Double-line box: ╔═╗║╚═╝ */
static const char *BOX2_TL = "\xE2\x95\x94";    /* ╔ */
static const char *BOX2_TR = "\xE2\x95\x97";    /* ╗ */
static const char *BOX2_BL = "\xE2\x95\x9A";    /* ╚ */
static const char *BOX2_BR = "\xE2\x95\x9D";    /* ╝ */
static const char *BOX2_H  = "\xE2\x95\x90";    /* ═ */
static const char *BOX2_V  = "\xE2\x95\x91";    /* ║ */

static void box_impl(int row, int col, int h, int w,
                     const char *tl, const char *tr,
                     const char *bl, const char *br,
                     const char *hh, const char *v) {
    if (h < 2 || w < 2) return;     /* too small for a box */

    /* Top row */
    if (clip_contains(row, col)) {
        emit_move(row, col);
        out_str(tl);
        for (int i = 1; i < w - 1; i++) out_str(hh);
        out_str(tr);
    }

    /* Side rows */
    for (int r = 1; r < h - 1; r++) {
        int rr = row + r;
        if (clip_contains(rr, col)) {
            emit_move(rr, col);
            out_str(v);
        }
        if (clip_contains(rr, col + w - 1)) {
            emit_move(rr, col + w - 1);
            out_str(v);
        }
    }

    /* Bottom row */
    if (clip_contains(row + h - 1, col)) {
        emit_move(row + h - 1, col);
        out_str(bl);
        for (int i = 1; i < w - 1; i++) out_str(hh);
        out_str(br);
    }
}

void tui_box_single(int row, int col, int h, int w) {
    box_impl(row, col, h, w,
             BOX1_TL, BOX1_TR, BOX1_BL, BOX1_BR, BOX1_H, BOX1_V);
}

void tui_box_double(int row, int col, int h, int w) {
    box_impl(row, col, h, w,
             BOX2_TL, BOX2_TR, BOX2_BL, BOX2_BR, BOX2_H, BOX2_V);
}

void tui_fill_rect(int row, int col, int h, int w, char c) {
    for (int r = row; r < row + h; r++) {
        if (!clip_contains(r, col)) continue;
        emit_move(r, col);
        for (int x = 0; x < w; x++) {
            if (!clip_contains(r, col + x)) break;
            out_byte(c);
        }
    }
}

void tui_present(void) {
    out_flush();
    /* SYS_FFLUSH is a no-op on the pipe transport (writes don't
     * buffer on Windows named pipes). On the cooked-stdio transport
     * it forces the libc buffer out, which matters because that's
     * what makes ANSI escape sequences without trailing newlines
     * actually visible. */
    sys_fflush(1);
}

/* ============================================================
 *  Input parsing
 *
 *  Pull-mode polling. Reads up to N bytes at a time via SYS_READ,
 *  feeds them into a small state machine, emits one TuiEvent per
 *  call (the rest stays buffered until the next call).
 *
 *  States:
 *    GROUND   — looking for the next byte's category
 *    ESC      — saw ESC, expecting [ or O or another byte
 *    CSI      — saw ESC[, collecting parameters then a final byte
 *    CSI_O    — saw ESCO, expecting a single final byte
 *
 *  Lone ESC: when ESC is in the buffer but no continuation byte
 *  has arrived, we wait for the NEXT tui_poll_event call. If no
 *  continuation has arrived by then, we emit TUI_KEY_ESCAPE.
 *  This isn't perfect (Alt+key sequences arriving slowly could be
 *  misclassified) but it's the standard approach.
 * ============================================================ */

#define IN_BUF_CAP 64
static char     g_in_buf[IN_BUF_CAP];
static unsigned g_in_head = 0;     /* read pointer */
static unsigned g_in_tail = 0;     /* write pointer */

enum {
    IN_STATE_GROUND = 0,
    IN_STATE_ESC,
    IN_STATE_CSI,
    IN_STATE_CSI_O,
};
static int      g_in_state = IN_STATE_GROUND;

/* CSI parameter accumulator. We collect up to 4 numeric parameters
 * and an optional private-marker (e.g., '<' for SGR mouse mode).
 * For round K only the modifier-bearing arrow forms are
 * interesting (e.g., CSI 1;5 A = Ctrl+Up). */
#define MAX_CSI_PARAMS 4
static int       g_csi_params[MAX_CSI_PARAMS];
static int       g_csi_n_params = 0;
static int       g_csi_curr = 0;
static bool      g_csi_has_curr = false;
static char      g_csi_intermediate = 0;   /* '<', '?', etc. */

/* Track when the ESC state was entered so we can emit a lone ESC
 * after one poll with no continuation. Counter increments each
 * call that returned no event while in ESC state. */
static int       g_esc_idle_polls = 0;

static int in_buf_used(void) {
    return (int)(g_in_tail - g_in_head);
}

static void in_buf_drop_consumed(void) {
    /* Compact: if everything's been consumed, reset to position 0
     * so we never run out of room from drifting indices. */
    if (g_in_head == g_in_tail) {
        g_in_head = g_in_tail = 0;
    }
}

static int in_buf_peek(unsigned offset) {
    if (g_in_head + offset >= g_in_tail) return -1;
    return (unsigned char)g_in_buf[g_in_head + offset];
}

static int in_buf_pop(void) {
    if (g_in_head >= g_in_tail) return -1;
    int b = (unsigned char)g_in_buf[g_in_head++];
    in_buf_drop_consumed();
    return b;
}

/* Try to read more bytes into the buffer (non-blocking). */
static void in_buf_refill(void) {
    /* If the buffer's all-consumed already, reset to position 0
     * so we can read up to the full capacity. */
    if (g_in_head == g_in_tail) {
        g_in_head = g_in_tail = 0;
    }
    unsigned avail = IN_BUF_CAP - g_in_tail;
    if (avail == 0) return;   /* full — caller hasn't consumed yet */
    int r = sys_read(0, g_in_buf + g_in_tail, avail);
    if (r > 0) g_in_tail += (unsigned)r;
}

/* Reset CSI parameter state. */
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

/* Map a kitty/xterm modifier code (1=none, 2=shift, 3=alt, 5=ctrl,
 * 9=meta, with combinations) to TUI_MOD_*. */
static int csi_mod_to_tui(int m) {
    int out = TUI_MOD_NONE;
    if (m <= 1) return out;
    m -= 1;   /* now 1=shift, 2=alt, 4=ctrl ish (per xterm) */
    if (m & 1) out |= TUI_MOD_SHIFT;
    if (m & 2) out |= TUI_MOD_ALT;
    if (m & 4) out |= TUI_MOD_CTRL;
    return out;
}

/* Build a key event with no modifier info. */
static void make_key(TuiEvent *ev, int key, char raw) {
    ev->kind = TUI_EV_KEY;
    ev->key.key = key;
    ev->key.mods = 0;
    ev->key.raw = raw;
}

/* CSI final byte arrived — interpret it. The CSI parameters are
 * in g_csi_params; modifier (if any) is in params[1]. */
static bool finish_csi(char final, TuiEvent *out) {
    csi_commit_param();
    int mods = (g_csi_n_params >= 2) ? csi_mod_to_tui(g_csi_params[1]) : 0;

    /* Simple letter finals (no number prefix expected): A B C D F H */
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

    /* Tilde finals — CSI <n> ~ for several keys. */
    if (final == '~' && g_csi_n_params >= 1) {
        int p = g_csi_params[0];
        switch (p) {
            case 1: sym = TUI_KEY_HOME; break;
            case 2: sym = TUI_KEY_INSERT; break;
            case 3: sym = TUI_KEY_DELETE; break;
            case 4: sym = TUI_KEY_END; break;
            case 5: sym = TUI_KEY_PAGE_UP; break;
            case 6: sym = TUI_KEY_PAGE_DOWN; break;
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

    /* Unknown CSI sequence — silently drop and continue parsing. */
    csi_reset();
    g_in_state = IN_STATE_GROUND;
    return false;
}

/* ESC O <X> — DEC application-cursor-key encoding for F1-F4. */
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

    /* Always refill buffer at the start of each poll. */
    in_buf_refill();

    /* Process bytes one at a time. We may consume multiple bytes
     * before producing one event (e.g., a 3-byte CSI sequence). */
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
                /* Map specific control bytes to symbolic keys.
                 * Otherwise pass through as the byte itself
                 * (which includes Ctrl-letter combos as bytes
                 * 1-26). */
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
                /* not reached */
                break;

            case IN_STATE_ESC:
                b = in_buf_pop();
                if (b == '[') {
                    g_in_state = IN_STATE_CSI;
                    csi_reset();
                } else if (b == 'O') {
                    g_in_state = IN_STATE_CSI_O;
                } else {
                    /* ESC <letter> — Alt+<letter> on most
                     * terminals. Emit as a key with ALT mod. */
                    g_in_state = IN_STATE_GROUND;
                    if (b >= 0x20 && b < 0x7F) {
                        out->kind = TUI_EV_KEY;
                        out->key.key = b;
                        out->key.mods = TUI_MOD_ALT;
                        out->key.raw = (char)b;
                        return true;
                    }
                    /* Unknown — drop the byte, keep parsing. */
                }
                break;

            case IN_STATE_CSI:
                b = in_buf_peek(0);
                if (b < 0) return false;   /* incomplete */

                /* Parameter byte 0x30-0x3F (digits, ';', '<', '?', etc.) */
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
                /* Final byte 0x40-0x7E */
                if (b >= 0x40 && b <= 0x7E) {
                    in_buf_pop();
                    if (finish_csi((char)b, out)) {
                        return true;
                    }
                    /* Unknown CSI; loop continues. */
                    break;
                }
                /* Anything else — abandon sequence. */
                in_buf_pop();
                csi_reset();
                g_in_state = IN_STATE_GROUND;
                break;

            case IN_STATE_CSI_O:
                b = in_buf_pop();
                if (finish_csi_o((char)b, out)) {
                    return true;
                }
                /* Unknown — silently drop, keep parsing. */
                break;
        }
    }

    /* No event produced this round. If we're in ESC state with
     * nothing buffered, this is a "lone ESC" candidate. We give
     * it one full poll-cycle of grace before emitting. */
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
