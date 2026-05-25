/* ============================================================
 *  fire.c — demoscene fire effect with a velocity-sensitive
 *           mouse drag.
 *
 *  The classic bottom-up fire: a heat field is seeded hot along
 *  the bottom edge and cooled+propagated upward every frame, so
 *  flames rise and flicker. Heat maps to a black->red->orange->
 *  yellow->white ramp drawn with shade glyphs.
 *
 *  Mouse (hold a button and drag — bare motion does nothing):
 *    - Slow drag / holding still  -> BURNS: heat is injected in a
 *      small radius, so flames flare up where the cursor lingers.
 *    - Fast drag                  -> CUTS: heat is zeroed along the
 *      path, carving a cool channel the fire closes back over.
 *  Same gesture throughout; cursor SPEED flips burn vs. cut.
 *
 *  q or Esc quits.
 *
 *  Drop-in guest: lives in host_files_src/, built automatically to
 *  /host/fire.elf. Run from the shell with:  fire
 * ============================================================ */

#include <stdint.h>
#include "tui.h"

#define SYS_EXIT          93
#define SYS_TICKS_NOW   1043
#define SYS_SLEEP_TICKS 1045
#define SYS_RAND        1112

static inline uint32_t sys0(uint32_t n) {
    register uint32_t a0 asm("a0");
    register uint32_t a7 asm("a7") = n;
    asm volatile("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline void sys1(uint32_t n, uint32_t a) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a7 asm("a7") = n;
    asm volatile("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

#define ROWS TUI_MAX_ROWS
#define COLS TUI_MAX_COLS

/* Heat field, one byte (0..255) per cell. Row 0 is the top of the
 * screen; the fire is seeded along the bottom row (ROWS-1). */
static uint8_t heat[ROWS][COLS];

/* --- tuning knobs (comment-documented so they're easy to feel out)
 * COOL_MAX   : max heat lost per cell per frame (higher = shorter
 *              flames). The actual loss is randomized 0..COOL_MAX.
 * CUT_SPEED  : drag distance (in cells) between two mouse events at
 *              or above which the drag CUTS instead of BURNS. Lower
 *              = easier to cut; higher = you have to whip it.
 * BURN_RADIUS: half-width of the hot spot painted when burning.
 * SEED_MIN   : floor heat re-seeded along the bottom row each frame.
 */
#define COOL_MAX     3
#define CUT_SPEED    3
#define BURN_RADIUS  2
#define SEED_MIN     180

/* When the button is held but the cursor is motionless, the terminal
 * stops sending mouse events, so the active burn above never fires.
 * We keep a gentle "ember" going at the last position each frame
 * while the button is held: a smaller radius and a heat FLOOR (we
 * raise cells toward this, not slam them to 255) so a resting cursor
 * smoulders rather than blowing up into a full flare. */
#define BURN_IDLE_RADIUS  1
#define BURN_IDLE_FLOOR   200

static inline uint32_t rnd(void) { return sys0(SYS_RAND); }

/* Map a heat value to a (glyph, fg) pair on the fire ramp. Cooler
 * cells use lighter shade glyphs in darker reds; hotter cells use
 * full blocks in yellow/white. bg stays black throughout. */
static void heat_glyph(uint8_t h, char *glyph, TuiColor *fg) {
    if (h < 24)       { *glyph = ' ';             *fg = TUI_BLACK; }
    else if (h < 64)  { *glyph = TUI_SHADE_LIGHT;  *fg = TUI_RED; }
    else if (h < 110) { *glyph = TUI_SHADE_MEDIUM; *fg = TUI_RED; }
    else if (h < 150) { *glyph = TUI_SHADE_DARK;   *fg = TUI_BRIGHT_RED; }
    else if (h < 190) { *glyph = TUI_BLOCK_FULL;   *fg = TUI_YELLOW; }
    else if (h < 230) { *glyph = TUI_BLOCK_FULL;   *fg = TUI_BRIGHT_YELLOW; }
    else              { *glyph = TUI_BLOCK_FULL;   *fg = TUI_BRIGHT_WHITE; }
}

/* Seed the bottom row with fresh randomized heat so the fire keeps
 * feeding. Values jitter around a high floor for a lively base. */
static void seed_bottom(void) {
    for (int c = 0; c < COLS; c++) {
        heat[ROWS - 1][c] = (uint8_t)(SEED_MIN + (rnd() % (256 - SEED_MIN)));
    }
}

/* One propagation step. Each cell pulls heat up from the row below
 * (averaging three neighbours for sideways spread) minus a random
 * cooling amount, so heat rises and decays. */
static void propagate(void) {
    for (int r = 0; r < ROWS - 1; r++) {
        for (int c = 0; c < COLS; c++) {
            int below   = heat[r + 1][c];
            int bleft    = heat[r + 1][c > 0 ? c - 1 : c];
            int bright   = heat[r + 1][c < COLS - 1 ? c + 1 : c];
            int avg = (below + below + bleft + bright) / 4;
            int cool = (int)(rnd() % (COOL_MAX + 1));
            int v = avg - cool;
            heat[r][c] = (uint8_t)(v < 0 ? 0 : v);
        }
    }
}

/* Paint the heat field to the canvas. */
static void render(void) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            char g; TuiColor fg;
            heat_glyph(heat[r][c], &g, &fg);
            tui_set_cell(r, c, g, fg, TUI_BLACK, TUI_ATTR_NONE);
        }
    }
}

/* Clamp helpers (no libm/stdlib pulled in). */
static inline int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
static inline int absi(int v) { return v < 0 ? -v : v; }

/* BURN: dump heat in a filled square around (row,col). */
static void burn_at(int row, int col) {
    for (int dr = -BURN_RADIUS; dr <= BURN_RADIUS; dr++) {
        for (int dc = -BURN_RADIUS; dc <= BURN_RADIUS; dc++) {
            int r = row + dr, c = col + dc;
            if (r < 0 || r >= ROWS || c < 0 || c >= COLS) continue;
            heat[r][c] = 255;
        }
    }
}

/* EMBER: gentle burn for a held-but-motionless cursor. Smaller
 * radius, and only raises cells toward BURN_IDLE_FLOOR (never above
 * it), so a resting cursor smoulders steadily instead of flaring to
 * full white. */
static void ember_at(int row, int col) {
    for (int dr = -BURN_IDLE_RADIUS; dr <= BURN_IDLE_RADIUS; dr++) {
        for (int dc = -BURN_IDLE_RADIUS; dc <= BURN_IDLE_RADIUS; dc++) {
            int r = row + dr, c = col + dc;
            if (r < 0 || r >= ROWS || c < 0 || c >= COLS) continue;
            if (heat[r][c] < BURN_IDLE_FLOOR) heat[r][c] = BURN_IDLE_FLOOR;
        }
    }
}

/* CUT: zero heat along the line from (r0,c0) to (r1,c1) so a fast
 * swipe leaves one continuous cool channel, not dotted gaps.
 * Integer Bresenham — no floats. */
static void cut_line(int r0, int c0, int r1, int c1) {
    int dr = absi(r1 - r0), dc = absi(c1 - c0);
    int sr = r0 < r1 ? 1 : -1, sc = c0 < c1 ? 1 : -1;
    int err = (dc > dr ? dc : -dr) / 2, e2;
    for (;;) {
        /* Zero a small cross at each step so the channel has width. */
        for (int k = -1; k <= 1; k++) {
            int cc = clampi(c0 + k, 0, COLS - 1);
            int rr = clampi(r0,     0, ROWS - 1);
            heat[rr][cc] = 0;
            int rr2 = clampi(r0 + k, 0, ROWS - 1);
            heat[rr2][clampi(c0, 0, COLS - 1)] = 0;
        }
        if (r0 == r1 && c0 == c1) break;
        e2 = err;
        if (e2 > -dc) { err -= dr; c0 += sc; }
        if (e2 <  dr) { err += dc; r0 += sr; }
    }
}

int main(void) {
    /* TUI_USE_MOUSE gives button + drag reporting (xterm 1002).
     * We deliberately DON'T set TUI_USE_MOUSE_MOTION, so bare
     * movement leaves the fire alone — only a held drag affects it,
     * per the demo's design. */
    if (!tui_init(TUI_USE_ALT_SCREEN | TUI_HIDE_CURSOR |
                  TUI_USE_MOUSE | TUI_USE_RAW | TUI_USE_SYNC_OUTPUT,
                  ROWS, COLS)) {
        return 1;
    }

    /* Start with a cold field; the bottom seed lights it up. */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++)
            heat[r][c] = 0;

    int  have_prev = 0;          /* do we have a last drag point? */
    int  prev_r = 0, prev_c = 0; /* last drag position */
    int  button_held = 0;        /* button currently down? */
    int  last_r = 0, last_c = 0; /* last position the button was at */

    for (;;) {
        /* --- input: drain all pending events this frame --------- */
        TuiEvent ev;
        while (tui_poll_event(&ev)) {
            if (ev.kind == TUI_EV_KEY) {
                int k = ev.key.key;
                if (k == 'q' || k == 'Q' || k == TUI_KEY_ESCAPE) {
                    tui_shutdown();
                    sys1(SYS_EXIT, 0);
                }
            } else if (ev.kind == TUI_EV_MOUSE) {
                /* Coordinates are 1-indexed; convert to 0-based. */
                int r = clampi(ev.mouse.row - 1, 0, ROWS - 1);
                int c = clampi(ev.mouse.col - 1, 0, COLS - 1);

                /* Track button state for the motionless-ember path
                 * below. press==true is press or motion-with-button;
                 * press==false is release. */
                if (ev.mouse.press) { button_held = 1; last_r = r; last_c = c; }
                else                { button_held = 0; }

                if (ev.mouse.drag) {
                    if (have_prev) {
                        /* Speed = Chebyshev distance from last point. */
                        int dist = absi(r - prev_r);
                        int dc   = absi(c - prev_c);
                        if (dc > dist) dist = dc;
                        if (dist >= CUT_SPEED) {
                            cut_line(prev_r, prev_c, r, c);  /* fast: slice */
                        } else {
                            burn_at(r, c);                   /* slow: flare */
                        }
                    } else {
                        burn_at(r, c);
                    }
                    prev_r = r; prev_c = c; have_prev = 1;
                } else {
                    /* Not a drag report: forget the path so the next
                     * drag measures speed fresh. (button_held above
                     * still tracks press/release for the ember.) */
                    have_prev = 0;
                }
            }
        }

        /* Held but motionless: the terminal sends no events while the
         * cursor sits still, so keep a gentle ember alive at the last
         * known position each frame. */
        if (button_held) ember_at(last_r, last_c);

        /* --- simulate + draw ----------------------------------- */
        seed_bottom();
        propagate();
        render();
        tui_present_diff();

        /* ~50 fps; also paces the cut/burn speed threshold. */
        sys1(SYS_SLEEP_TICKS, 20);
    }
}
