/* snake.c — classic snake game, built on the tui library.
 *
 * Demonstrates:
 *   - tui_init / tui_shutdown lifecycle
 *   - Cell-based drawing for the playfield
 *   - tui_box_double for the border
 *   - Color attributes (snake head/body distinct from food)
 *   - Pull-mode input loop with tui_poll_event
 *   - Kernel-managed periodic timing via SYS_SET_RELOAD_PERIOD
 *
 * Controls: WASD, hjkl, or arrow keys to steer. q or Ctrl-C
 * quits. Walk into a wall or yourself = game over.
 *
 * Public domain (CC0).
 */

#include "lib/tui.h"

/* ============================================================
 *  Syscall stubs (not in tui — these are timing / control)
 * ============================================================ */

#define SYS_EXIT                93
#define SYS_TICKS_NOW         1043
#define SYS_TICK_HZ           1044
#define SYS_SLEEP_UNTIL       1046
#define SYS_SET_RELOAD_PERIOD 1047
#define SYS_YIELD_UNTIL_RELOAD 1048

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

static inline unsigned sys_ticks_now(void) {
    register unsigned a0 asm("a0");
    register int      a7 asm("a7") = SYS_TICKS_NOW;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline unsigned sys_tick_hz(void) {
    register unsigned a0 asm("a0");
    register int      a7 asm("a7") = SYS_TICK_HZ;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}

static inline void sys_sleep_until(unsigned deadline) {
    register unsigned a0 asm("a0") = deadline;
    register int      a7 asm("a7") = SYS_SLEEP_UNTIL;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

static inline void sys_set_reload_period(unsigned period) {
    register unsigned a0 asm("a0") = period;
    register int      a7 asm("a7") = SYS_SET_RELOAD_PERIOD;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

static inline void sys_yield_until_reload(void) {
    register int a0 asm("a0");
    register int a7 asm("a7") = SYS_YIELD_UNTIL_RELOAD;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
}

/* ============================================================
 *  Tiny formatting helpers
 * ============================================================ */

static unsigned slen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}

/* Render unsigned int into a fixed buffer, return start ptr. */
static char *fmt_u(unsigned v, char *buf_end) {
    char *p = buf_end;
    *--p = '\0';
    if (v == 0) { *--p = '0'; return p; }
    while (v) { *--p = (char)('0' + (v % 10)); v /= 10; }
    return p;
}

/* ============================================================
 *  PRNG (small, deterministic but seeded)
 * ============================================================ */

static unsigned g_rng_state = 0xdeadbeefu;

static unsigned rng_next(void) {
    /* xorshift32 — fine for game randomness. */
    unsigned x = g_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_rng_state = x;
    return x;
}

/* ============================================================
 *  Game state
 * ============================================================ */

/* Playfield dimensions. Sits inside a double-line border, with a
 * score banner above. Total screen footprint:
 *
 *   Row 1:  "snake -- WASD / hjkl / arrows to move, q to quit"
 *   Row 2:  "score: NN"
 *   Row 3:  top border ╔══...══╗
 *   Rows 4..3+PLAY_H: │ . . . │ playfield rows
 *   Row 4+PLAY_H: bottom border ╚══...══╝
 */
#define PLAY_W 40
#define PLAY_H 14
#define MAX_SNAKE (PLAY_W * PLAY_H)

typedef struct { unsigned char x, y; } Cell;

static Cell g_snake[MAX_SNAKE];
static unsigned g_head_idx;       /* head is at g_snake[head_idx]  */
static unsigned g_tail_idx;       /* tail is at g_snake[tail_idx]  */
static unsigned g_length;         /* number of valid cells         */

static signed char g_dx = 1;      /* heading right initially       */
static signed char g_dy = 0;

static Cell g_food;
static unsigned g_score;

/* ============================================================
 *  Snake-as-ring-buffer access helpers
 * ============================================================ */

static Cell *snake_head(void) { return &g_snake[g_head_idx]; }

static void snake_push_head(Cell c) {
    g_head_idx = (g_head_idx + 1) % MAX_SNAKE;
    g_snake[g_head_idx] = c;
    g_length++;
}

static void snake_pop_tail(void) {
    g_tail_idx = (g_tail_idx + 1) % MAX_SNAKE;
    g_length--;
}

static int snake_body_contains(unsigned char x, unsigned char y) {
    unsigned i = g_tail_idx;
    for (unsigned n = 0; n < g_length; n++) {
        if (g_snake[i].x == x && g_snake[i].y == y) return 1;
        i = (i + 1) % MAX_SNAKE;
    }
    return 0;
}

/* ============================================================
 *  Coordinate mapping (game cell → terminal cell)
 * ============================================================ */

/* Playfield cell (x, y) → terminal (row, col). Origin of game
 * is (0,0); origin of terminal is (1,1). Border is at row 3
 * and PLAY_H+3, columns 1 and PLAY_W+2 respectively. So
 * interior cells start at row 4 and column 2. */
static int cell_row(int y) { return 4 + y; }
static int cell_col(int x) { return 2 + x; }

/* ============================================================
 *  Drawing
 * ============================================================ */

static void draw_chrome(void) {
    /* Title and score lines. Reset colors first so partial
     * frame redraws don't inherit a residual fg. */
    tui_reset();
    tui_move(1, 1);
    tui_set_fg(TUI_BRIGHT_WHITE);
    tui_puts("snake -- WASD / hjkl / arrows to move, q to quit");

    tui_move(2, 1);
    tui_set_fg(TUI_BRIGHT_YELLOW);
    tui_puts("score: ");
    char buf[12];
    char *p = fmt_u(g_score, buf + sizeof(buf));
    tui_puts(p);
    /* Pad a few spaces in case score shrank somehow (it can't —
     * but defensive). */
    tui_puts("   ");
    tui_reset();

    /* Border: an ASCII box around the playfield. ASCII chars
     * are 3x cheaper than UTF-8 box-drawing chars (1 byte each
     * vs 3) — important when the border is rebuilt on every
     * full redraw. The visual difference is minor. */
    tui_set_fg(TUI_BRIGHT_CYAN);
    tui_box_ascii(3, 1, PLAY_H + 2, PLAY_W + 2);
    tui_reset();
}

static void update_score_line(void) {
    /* Just rewrite the score portion — cheaper than a full
     * chrome redraw. */
    tui_move(2, 1);
    tui_set_fg(TUI_BRIGHT_YELLOW);
    tui_puts("score: ");
    char buf[12];
    char *p = fmt_u(g_score, buf + sizeof(buf));
    tui_puts(p);
    tui_puts("   ");
    tui_reset();
}

static void draw_cell(unsigned char x, unsigned char y, char ch,
                      TuiColor fg) {
    tui_set_cell(cell_row(y), cell_col(x), ch,
                 fg, TUI_DEFAULT_COLOR, TUI_ATTR_NONE);
}

static void draw_snake_initial(void) {
    /* Walk the snake ring from tail to head, drawing each cell.
     * Head gets the distinguished '@' character; body gets 'o'. */
    unsigned i = g_tail_idx;
    for (unsigned n = 0; n < g_length; n++) {
        Cell c = g_snake[i];
        if (i == g_head_idx) {
            draw_cell(c.x, c.y, '@', TUI_BRIGHT_GREEN);
        } else {
            draw_cell(c.x, c.y, 'o', TUI_GREEN);
        }
        i = (i + 1) % MAX_SNAKE;
    }
}

static void draw_food(void) {
    draw_cell(g_food.x, g_food.y, '*', TUI_BRIGHT_RED);
}

/* ============================================================
 *  Food placement
 * ============================================================ */

static void place_food(void) {
    /* Try random positions until we land on an empty cell. With
     * a 40x14 playfield (560 cells) and a snake of reasonable
     * length this terminates fast. */
    for (int tries = 0; tries < 1000; tries++) {
        unsigned char x = (unsigned char)(rng_next() % PLAY_W);
        unsigned char y = (unsigned char)(rng_next() % PLAY_H);
        if (snake_body_contains(x, y)) continue;
        g_food.x = x; g_food.y = y;
        return;
    }
    /* Failsafe: the playfield is essentially full. Game ends. */
    g_food.x = 0; g_food.y = 0;
}

/* ============================================================
 *  Input → direction
 * ============================================================ */

static void apply_direction(int dx, int dy) {
    /* Reject 180° reversals — common rule in snake to avoid the
     * "press opposite, immediately self-collide" trap. */
    if (dx == -g_dx && g_dx != 0) return;
    if (dy == -g_dy && g_dy != 0) return;
    g_dx = (signed char)dx;
    g_dy = (signed char)dy;
}

/* Drain accumulated input and apply the LAST direction-changing
 * keypress. Returns true if the user wants to quit. */
static int poll_inputs(void) {
    TuiEvent ev;
    int dx = g_dx, dy = g_dy;
    int direction_changed = 0;
    while (tui_poll_event(&ev)) {
        if (ev.kind != TUI_EV_KEY) continue;
        switch (ev.key.key) {
            case 'q': case 'Q':
            case 0x03:                    /* Ctrl-C */
                return 1;
            case 'w': case 'W':
            case 'k':
            case TUI_KEY_UP:
                dx = 0; dy = -1; direction_changed = 1; break;
            case 's': case 'S':
            case 'j':
            case TUI_KEY_DOWN:
                dx = 0; dy = 1; direction_changed = 1; break;
            case 'a': case 'A':
            case 'h':
            case TUI_KEY_LEFT:
                dx = -1; dy = 0; direction_changed = 1; break;
            case 'd': case 'D':
            case 'l':
            case TUI_KEY_RIGHT:
                dx = 1; dy = 0; direction_changed = 1; break;
            default:
                break;
        }
    }
    if (direction_changed) apply_direction(dx, dy);
    return 0;
}

/* ============================================================
 *  Menu screens (round D.3)
 *
 *  We have three states the game can be in outside of active
 *  play: title screen at startup, game-over screen after a
 *  collision, and the actual playing state. Title and game-
 *  over both wait on the same input pattern: any key, mouse
 *  click on a button, then dispatch to either "start play"
 *  or "quit."
 *
 *  Both screens use the buttons from lib/tui.h plus accept
 *  Enter/Space as keyboard shortcuts for the primary action.
 * ============================================================ */

/* Add TUI buttons that snake's menus can use. The widget itself
 * is in lib/tui.h. */

static void reset_game_state(void) {
    /* Snake: 4 cells, centered, heading right. */
    g_head_idx = 3;
    g_tail_idx = 0;
    g_length = 4;
    for (unsigned i = 0; i < g_length; i++) {
        g_snake[i].x = (unsigned char)(PLAY_W / 2 - 2 + i);
        g_snake[i].y = (unsigned char)(PLAY_H / 2);
    }
    g_dx = 1; g_dy = 0;
    g_score = 0;
    place_food();
}

/* Render a centered title screen. Returns 1 if user chose start,
 * 0 if user chose quit (Esc / q). */
static int show_title_screen(void) {
    /* Wipe the canvas to black. */
    tui_set_bg(TUI_BLACK);
    tui_fill_rect(1, 1, 22, 50, ' ');
    tui_reset();

    /* "SNAKE" title in big colored block, centered. */
    tui_set_bg(TUI_BLACK);
    tui_set_fg(TUI_BRIGHT_GREEN);
    tui_set_attr(TUI_ATTR_BOLD);
    tui_move(5, 22);
    tui_puts("SNAKE");
    tui_reset();

    tui_set_bg(TUI_BLACK);
    tui_set_fg(TUI_BRIGHT_WHITE);
    tui_move(8, 14);
    tui_puts("arrow keys or WASD to move");
    tui_move(9, 14);
    tui_puts("eat the red food, don't bite yourself");
    tui_reset();

    /* Two side-by-side buttons: START (default) and QUIT. */
    TuiButton start_btn = {
        .row = 13, .col = 12, .h = 3, .w = 12,
        .label = "START",
        .fg = TUI_BRIGHT_WHITE, .bg = 28,         /* green-ish */
        .pressed_fg = TUI_BRIGHT_WHITE, .pressed_bg = 22,
    };
    TuiButton quit_btn = {
        .row = 13, .col = 28, .h = 3, .w = 12,
        .label = "QUIT",
        .fg = TUI_BRIGHT_WHITE, .bg = 88,         /* dark red */
        .pressed_fg = TUI_BRIGHT_WHITE, .pressed_bg = 52,
    };
    tui_button_draw(&start_btn);
    tui_button_draw(&quit_btn);

    tui_set_bg(TUI_BLACK);
    tui_set_fg(TUI_BRIGHT_BLACK);
    tui_move(17, 13);
    tui_puts("Enter/Space = start    Esc/q = quit");
    tui_reset();

    tui_present();

    unsigned hz = sys_tick_hz();
    if (hz == 0) hz = 1000;

    /* Drain any input that's already queued (e.g. the Enter that
     * launched us from the shell). */
    {
        unsigned drain_until = sys_ticks_now() + (hz / 7);
        TuiEvent junk;
        while ((int)(sys_ticks_now() - drain_until) < 0) {
            while (tui_poll_event(&junk)) { }
            sys_sleep_until(sys_ticks_now() + (hz / 100));
        }
    }

    for (;;) {
        TuiEvent ev;
        while (tui_poll_event(&ev)) {
            if (ev.kind == TUI_EV_KEY) {
                int k = ev.key.key;
                if (k == TUI_KEY_ENTER || k == ' ') return 1;
                if (k == TUI_KEY_ESCAPE || k == 'q' || k == 'Q') return 0;
            } else if (ev.kind == TUI_EV_MOUSE) {
                TuiButtonResult rs = tui_button_handle(&start_btn, &ev);
                if (rs == TUI_BTN_CLICKED) return 1;
                if (rs == TUI_BTN_REDRAW) tui_button_draw(&start_btn);
                TuiButtonResult rq = tui_button_handle(&quit_btn, &ev);
                if (rq == TUI_BTN_CLICKED) return 0;
                if (rq == TUI_BTN_REDRAW) tui_button_draw(&quit_btn);
            }
        }
        tui_present_diff();
        sys_sleep_until(sys_ticks_now() + (hz / 50));   /* ~20 ms */
    }
}

/* Game-over screen. Returns 1 to play again, 0 to quit. */
static int show_game_over_screen(const char *reason) {
    /* Center the dialog over the play area. */
    int box_h = 9;
    int box_w = 38;
    int box_row = cell_row(PLAY_H / 2 - 3);
    int box_col = cell_col(PLAY_W / 2 - box_w / 2 + 1);

    tui_set_bg(TUI_BLACK);
    tui_fill_rect(box_row, box_col, box_h, box_w, ' ');

    tui_set_fg(TUI_BRIGHT_RED);
    tui_set_attr(TUI_ATTR_BOLD);
    tui_box_double(box_row, box_col, box_h, box_w);
    tui_reset();
    tui_set_bg(TUI_BLACK);

    const char *title = "GAME OVER";
    int title_col = box_col + (box_w - 9) / 2;
    tui_move(box_row + 1, title_col);
    tui_set_fg(TUI_BRIGHT_RED);
    tui_set_attr(TUI_ATTR_BOLD);
    tui_puts(title);

    tui_set_attr(TUI_ATTR_NONE);
    tui_set_fg(TUI_BRIGHT_WHITE);
    unsigned reason_len = slen(reason);
    int reason_col = box_col + (box_w - (int)reason_len) / 2;
    tui_move(box_row + 2, reason_col);
    tui_puts(reason);

    char score_buf[12];
    char *score_str = fmt_u(g_score, score_buf + sizeof(score_buf));
    unsigned score_str_len = slen(score_str);
    unsigned score_total = 7 + score_str_len;
    int score_col = box_col + (box_w - (int)score_total) / 2;
    tui_move(box_row + 3, score_col);
    tui_set_fg(TUI_BRIGHT_YELLOW);
    tui_puts("score: ");
    tui_puts(score_str);
    tui_reset();

    /* Buttons. */
    TuiButton play_btn = {
        .row = box_row + 5, .col = box_col + 4, .h = 3, .w = 12,
        .label = "PLAY AGAIN",
        .fg = TUI_BRIGHT_WHITE, .bg = 28,
        .pressed_fg = TUI_BRIGHT_WHITE, .pressed_bg = 22,
    };
    TuiButton exit_btn = {
        .row = box_row + 5, .col = box_col + 22, .h = 3, .w = 12,
        .label = "EXIT",
        .fg = TUI_BRIGHT_WHITE, .bg = 88,
        .pressed_fg = TUI_BRIGHT_WHITE, .pressed_bg = 52,
    };
    tui_button_draw(&play_btn);
    tui_button_draw(&exit_btn);

    tui_present_diff();

    unsigned hz = sys_tick_hz();
    if (hz == 0) hz = 1000;

    /* Drain any input that arrived during the collision frame. */
    {
        unsigned drain_until = sys_ticks_now() + (hz / 4);
        TuiEvent junk;
        while ((int)(sys_ticks_now() - drain_until) < 0) {
            while (tui_poll_event(&junk)) { }
            sys_sleep_until(sys_ticks_now() + (hz / 100));
        }
    }

    for (;;) {
        TuiEvent ev;
        while (tui_poll_event(&ev)) {
            if (ev.kind == TUI_EV_KEY) {
                int k = ev.key.key;
                if (k == TUI_KEY_ENTER || k == ' ') return 1;
                if (k == TUI_KEY_ESCAPE || k == 'q' || k == 'Q') return 0;
            } else if (ev.kind == TUI_EV_MOUSE) {
                TuiButtonResult rp = tui_button_handle(&play_btn, &ev);
                if (rp == TUI_BTN_CLICKED) return 1;
                if (rp == TUI_BTN_REDRAW) tui_button_draw(&play_btn);
                TuiButtonResult re = tui_button_handle(&exit_btn, &ev);
                if (re == TUI_BTN_CLICKED) return 0;
                if (re == TUI_BTN_REDRAW) tui_button_draw(&exit_btn);
            }
        }
        tui_present_diff();
        sys_sleep_until(sys_ticks_now() + (hz / 50));
    }
}

/* ============================================================
 *  Entry point
 * ============================================================ */

void _start(void) {
    /* Seed the RNG with the host tick counter so each run is
     * different. */
    g_rng_state = sys_ticks_now() ^ 0xa5a5a5a5u;

    /* TUI startup.
     *   ALT_SCREEN  — keep the shell's scrollback pristine
     *   RAW         — disable echo, deliver keypresses one at a time
     *   HIDE_CURSOR — keep PuTTY from drawing a roving cursor as
     *                 we move it around redrawing cells
     *   SYNC_OUTPUT — bracket each present in CSI ?2026h/l so the
     *                 terminal holds off repaint until the frame
     *                 is complete. Eliminates the mid-frame tearing
     *                 that makes full-canvas redraws look "jumpy."
     *                 Modern PuTTY supports it; older terminals
     *                 ignore the bracket silently.
     *
     * Canvas dimensions: 22 rows × 50 cols comfortably fits the
     * border (16×42) plus chrome without wasting cycles on areas
     * we don't draw to. */
    tui_init(TUI_USE_ALT_SCREEN | TUI_USE_RAW |
             TUI_HIDE_CURSOR | TUI_USE_SYNC_OUTPUT |
             TUI_USE_MOUSE,
             22, 50);

    unsigned hz = sys_tick_hz();
    if (hz == 0) hz = 1000;

    /* Show the title screen first. User decides whether to play. */
    if (!show_title_screen()) {
        tui_shutdown();
        sys_exit(0);
    }

play_again:
    /* (Re)initialize game state and draw the initial frame. */
    reset_game_state();
    /* Wipe the canvas — we may be coming from a previous game-
     * over screen and need a clean playfield. */
    tui_set_bg(TUI_BLACK);
    tui_fill_rect(1, 1, 22, 50, ' ');
    tui_reset();
    draw_chrome();
    draw_snake_initial();
    draw_food();
    tui_present();

    /* Drain any input that arrived during startup (e.g., the
     * Enter key that launched us). 150 ms grace window. */
    {
        unsigned drain_until = sys_ticks_now() + (hz / 7);
        TuiEvent junk;
        while ((int)(sys_ticks_now() - drain_until) < 0) {
            while (tui_poll_event(&junk)) { /* discard */ }
            sys_sleep_until(sys_ticks_now() + (hz / 100));
        }
    }

    sys_set_reload_period(hz / 8);

    int game_over = 0;
    const char *over_reason = "";

    while (!game_over) {
        if (poll_inputs()) {
            game_over = 0;     /* user-requested quit, no game over screen */
            break;
        }

        /* Compute new head position. */
        Cell h = *snake_head();
        int nx = (int)h.x + g_dx;
        int ny = (int)h.y + g_dy;

        /* Wall collision */
        if (nx < 0 || nx >= PLAY_W || ny < 0 || ny >= PLAY_H) {
            game_over = 1;
            over_reason = "hit a wall";
            break;
        }

        unsigned char ux = (unsigned char)nx;
        unsigned char uy = (unsigned char)ny;
        int eating = (ux == g_food.x && uy == g_food.y);

        if (snake_body_contains(ux, uy)) {
            game_over = 1;
            over_reason = "bit yourself";
            break;
        }

        /* Erase old tail (unless eating, in which case the snake
         * grows by not popping). */
        if (!eating) {
            Cell t = g_snake[g_tail_idx];
            draw_cell(t.x, t.y, ' ', TUI_DEFAULT_COLOR);
            snake_pop_tail();
        }

        /* Demote old head to body, install new head. */
        Cell prev_head = *snake_head();
        draw_cell(prev_head.x, prev_head.y, 'o', TUI_GREEN);
        Cell new_head = { ux, uy };
        snake_push_head(new_head);
        draw_cell(ux, uy, '@', TUI_BRIGHT_GREEN);

        if (eating) {
            g_score++;
            update_score_line();
            place_food();
            draw_food();
        }

        /* Diff present: only cells that changed on the canvas
         * since last frame go on the wire. For snake that's 3
         * cells per frame (old tail erased, old head→body, new
         * head). ~50 bytes per frame instead of ~840 with full
         * present, which both reduces jitter (less data to push
         * through the pipe + render) and keeps the per-frame
         * SGR/move sequence simple enough that synchronized
         * output can hold the terminal still for a meaningfully
         * short interval. */
        tui_present_diff();
        sys_yield_until_reload();
    }

    /* Game-over screen: ask the user whether to play again or
     * exit. Uses the shared menu from round D.3. */
    if (game_over) {
        sys_set_reload_period(0);
        if (show_game_over_screen(over_reason)) {
            goto play_again;
        }
    }

    /* Clean shutdown: TUI shutdown unwinds alt-screen, raw-mode,
     * and cursor. The user lands back at the shell prompt with
     * their scrollback untouched. */
    tui_shutdown();
    sys_exit(0);
}
