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
 *  Entry point
 * ============================================================ */

void _start(void) {
    /* Seed the RNG with the host tick counter so each run is
     * different. */
    g_rng_state = sys_ticks_now() ^ 0xa5a5a5a5u;

    /* TUI startup. Alt screen keeps the user's shell scrollback
     * pristine; raw mode disables echo and lets us see keypresses
     * one at a time; hidden cursor cleans up the display.
     *
     * Canvas dimensions: 24 rows × 80 cols is the classic
     * minimum. We use 22 rows × 50 cols to give the playfield
     * border (16 rows × 42 cols) + chrome a comfortable home
     * without spending bytes on terminal area we won't draw to. */
    tui_init(TUI_USE_ALT_SCREEN | TUI_USE_RAW | TUI_HIDE_CURSOR,
             22, 50);

    /* Initial snake: 4 cells, centered, heading right. */
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
    draw_chrome();
    draw_snake_initial();
    draw_food();
    tui_present();

    /* Pacing setup: 8 fps (125 ms / frame). */
    unsigned hz = sys_tick_hz();
    if (hz == 0) hz = 1000;

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

        tui_present();
        sys_yield_until_reload();
    }

    /* Game-over screen, if applicable. Draws a centered banner
     * inside a small box, then waits for the user to press any
     * key before exiting. This replaces the previous fixed 1.5s
     * timeout, which on slow terminals could be missed entirely
     * — and now the user explicitly acknowledges the end of the
     * game rather than blinking back to the shell. */
    if (game_over) {
        /* Box dimensions: 5 rows tall, wide enough for the
         * longest expected message. Centered horizontally in
         * the playfield. */
        int box_h = 5;
        int box_w = 36;
        int box_row = cell_row(PLAY_H / 2 - 2);
        int box_col = cell_col(PLAY_W / 2 - box_w / 2 + 1);

        /* Clear the box interior first so any snake body or
         * food underneath gets erased. Use a dark bg to make
         * the dialog visually distinct. */
        tui_set_bg(TUI_BLACK);
        tui_fill_rect(box_row, box_col, box_h, box_w, ' ');

        /* Bright red double-line border. */
        tui_set_fg(TUI_BRIGHT_RED);
        tui_set_attr(TUI_ATTR_BOLD);
        tui_box_double(box_row, box_col, box_h, box_w);
        tui_reset();
        tui_set_bg(TUI_BLACK);

        /* "GAME OVER" centered on line 2 of the box. */
        const char *title = "GAME OVER";
        int title_col = box_col + (box_w - 9) / 2;
        tui_move(box_row + 1, title_col);
        tui_set_fg(TUI_BRIGHT_RED);
        tui_set_attr(TUI_ATTR_BOLD);
        tui_puts(title);

        /* Reason on line 3, centered. */
        tui_set_attr(TUI_ATTR_NONE);
        tui_set_fg(TUI_BRIGHT_WHITE);
        /* Figure out reason length to center it. */
        unsigned reason_len = slen(over_reason);
        int reason_col = box_col + (box_w - (int)reason_len) / 2;
        tui_move(box_row + 2, reason_col);
        tui_puts(over_reason);

        /* "score: N" on line 4, centered. */
        char score_buf[12];
        char *score_str = fmt_u(g_score, score_buf + sizeof(score_buf));
        unsigned score_str_len = slen(score_str);
        unsigned score_total = 7 + score_str_len;   /* "score: " + digits */
        int score_col = box_col + (box_w - (int)score_total) / 2;
        tui_move(box_row + 3, score_col);
        tui_set_fg(TUI_BRIGHT_YELLOW);
        tui_puts("score: ");
        tui_puts(score_str);

        /* Hint at bottom of playfield. */
        tui_reset();
        tui_set_fg(TUI_BRIGHT_BLACK);
        int hint_row = cell_row(PLAY_H) + 1;
        tui_move(hint_row, cell_col(0));
        tui_puts("press any key to exit");
        tui_reset();
        tui_present();

        /* Drain any leftover input that arrived during the
         * collision frame (e.g., the key that turned snake into
         * the wall), then wait for a fresh keypress. We require
         * a fresh press, not just any byte, so a held-down arrow
         * doesn't blow past the screen. */
        sys_set_reload_period(0);
        {
            unsigned drain_until = sys_ticks_now() + (hz / 4); /* 250 ms */
            TuiEvent junk;
            while ((int)(sys_ticks_now() - drain_until) < 0) {
                while (tui_poll_event(&junk)) { /* discard */ }
                sys_sleep_until(sys_ticks_now() + (hz / 100));
            }
        }
        for (;;) {
            TuiEvent ev;
            if (tui_poll_event(&ev) && ev.kind == TUI_EV_KEY) break;
            sys_sleep_until(sys_ticks_now() + (hz / 50)); /* 20 ms */
        }
    }

    /* Clean shutdown: TUI shutdown unwinds alt-screen, raw-mode,
     * and cursor. The user lands back at the shell prompt with
     * their scrollback untouched. */
    tui_shutdown();
    sys_exit(0);
}
