/* host_files_src/snake.c — Nokia-style snake game.
 *
 * Demonstrates several things together:
 *
 *   - Raw-mode TTY toggle (SYS_TTY_SET_RAW): turns the terminal
 *     into byte-at-a-time mode so we can read arrow keys as
 *     ANSI escape sequences rather than line-buffered input.
 *   - Periodic game loop using SYS_SLEEP_UNTIL with an autoreload
 *     deadline — slow frames self-correct rather than drifting.
 *   - SYS_TICKS_NOW for a millisecond clock the game uses for
 *     timing AND as a PRNG seed.
 *   - ANSI escape sequences for cursor positioning and screen
 *     clearing (no library; just bytes to stdout).
 *
 * The game runs until the snake hits a wall, hits itself, or
 * the player presses 'q'. On exit it clears the screen, shows
 * the cursor again, restores cooked TTY mode, and returns to
 * the shell as if nothing happened.
 *
 * Build: dropped into host_files/snake.elf by 05_shell/build.sh.
 * Run from inside the shell:
 *
 *     [/]
 *     $ run /host/snake.elf
 */

#define SYS_READ            63
#define SYS_WRITE           64
#define SYS_EXIT            93
#define SYS_YIELD         1040
#define SYS_TICKS_NOW     1043
#define SYS_TICK_HZ       1044
#define SYS_SLEEP_TICKS   1045
#define SYS_SLEEP_UNTIL   1046
#define SYS_TTY_SET_RAW   1105

/* ============================================================
 *  Syscall inline asm
 * ============================================================ */

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

static inline int sys_tty_set_raw(int enable) {
    register int a0 asm("a0") = enable;
    register int a7 asm("a7") = SYS_TTY_SET_RAW;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

/* ============================================================
 *  Output helpers (no libc — we build strings by hand)
 * ============================================================ */

static unsigned slen(const char *s) {
    unsigned n = 0; while (s[n]) n++; return n;
}

static void puts_(const char *s) { sys_write(1, s, slen(s)); }

/* Format an unsigned int in decimal into the END of `buf` and
 * return a pointer to the first digit. Caller passes the
 * sentinel one-past-end position. */
static char *fmt_u(unsigned v, char *buf_end) {
    *--buf_end = '\0';
    if (v == 0) { *--buf_end = '0'; return buf_end; }
    while (v) { *--buf_end = (char)('0' + v % 10); v /= 10; }
    return buf_end;
}

/* Write a decimal integer to stdout (no newline). */
static void putd(unsigned v) {
    char buf[16];
    char *p = fmt_u(v, buf + sizeof(buf));
    puts_(p);
}

/* ============================================================
 *  ANSI control sequences
 * ============================================================ */

static void ansi_clear_screen(void) { puts_("\x1b[2J"); }
static void ansi_home(void)         { puts_("\x1b[H");  }
static void ansi_hide_cursor(void)  { puts_("\x1b[?25l"); }
static void ansi_show_cursor(void)  { puts_("\x1b[?25h"); }

/* Move cursor to row, col (1-indexed, ANSI convention). */
static void ansi_goto(unsigned row, unsigned col) {
    char buf[24];
    char *p = buf;
    *p++ = 0x1b;
    *p++ = '[';
    char numbuf[8];
    char *n = fmt_u(row, numbuf + sizeof(numbuf));
    while (*n) *p++ = *n++;
    *p++ = ';';
    n = fmt_u(col, numbuf + sizeof(numbuf));
    while (*n) *p++ = *n++;
    *p++ = 'H';
    sys_write(1, buf, (unsigned)(p - buf));
}

/* ============================================================
 *  Tiny LCG-based PRNG
 *
 *  Numerical Recipes constants. Good enough for picking food
 *  positions; we don't need cryptographic quality.
 * ============================================================ */

static unsigned g_rng_state = 1;

static void rng_seed(unsigned s) { g_rng_state = s ? s : 1; }

static unsigned rng_next(void) {
    g_rng_state = g_rng_state * 1664525u + 1013904223u;
    return g_rng_state;
}

/* Uniform-ish in [0, limit). */
static unsigned rng_range(unsigned limit) {
    return rng_next() % limit;
}

/* ============================================================
 *  Game state
 *
 *  Playfield is an interior PLAY_W x PLAY_H grid (the border is
 *  drawn outside this). The snake is a queue of cells stored in
 *  a circular buffer so growth is O(1).
 * ============================================================ */

#define PLAY_W       40
#define PLAY_H       20
#define MAX_SNAKE   256

typedef struct { unsigned char x, y; } Cell;

static Cell g_snake[MAX_SNAKE];
static unsigned g_head_idx;   /* index of head in g_snake */
static unsigned g_tail_idx;   /* index of tail in g_snake */
static unsigned g_length;     /* number of valid cells */

/* Direction: dx, dy. */
static signed char g_dx = 1;
static signed char g_dy = 0;

static Cell g_food;
static unsigned g_score = 0;

/* Cell access helpers */
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

/* Is a cell occupied by the snake's body? (Excludes head — used
 * for collision detection against the new head position.) */
static int snake_body_contains(unsigned char x, unsigned char y) {
    if (g_length == 0) return 0;
    /* Walk from tail toward head, stopping BEFORE the head. */
    unsigned i = g_tail_idx;
    while (i != g_head_idx) {
        if (g_snake[i].x == x && g_snake[i].y == y) return 1;
        i = (i + 1) % MAX_SNAKE;
    }
    return 0;
}

/* ============================================================
 *  Drawing
 *
 *  The playfield is rendered once at start; on each frame we
 *  only redraw the head's new position, erase the old tail
 *  cell, and update the score line. Keeps the byte count to
 *  the terminal low so even slow ttys render smoothly.
 * ============================================================ */

static void draw_border(void) {
    /* Score line at row 1 */
    ansi_goto(1, 1);
    puts_("snake — arrows / WASD / hjkl to move, q to quit");
    ansi_goto(2, 1);
    puts_("score: ");
    putd(g_score);

    /* Top border at row 3, playfield rows 4..3+PLAY_H. */
    ansi_goto(3, 1);
    sys_write(1, "+", 1);
    for (int i = 0; i < PLAY_W; i++) sys_write(1, "-", 1);
    sys_write(1, "+", 1);

    for (int y = 0; y < PLAY_H; y++) {
        ansi_goto(4 + (unsigned)y, 1);
        sys_write(1, "|", 1);
        ansi_goto(4 + (unsigned)y, 2 + PLAY_W);
        sys_write(1, "|", 1);
    }

    ansi_goto(4 + PLAY_H, 1);
    sys_write(1, "+", 1);
    for (int i = 0; i < PLAY_W; i++) sys_write(1, "-", 1);
    sys_write(1, "+", 1);
}

static void draw_cell(unsigned char x, unsigned char y, char ch) {
    /* Cell (x,y) maps to terminal row 4+y, col 2+x (1-indexed). */
    ansi_goto(4 + y, 2 + x);
    sys_write(1, &ch, 1);
}

static void update_score_line(void) {
    ansi_goto(2, 1);
    puts_("score: ");
    putd(g_score);
    /* Clear to end of line in case length shrank */
    puts_("\x1b[K");
}

/* Place food at a random empty cell. */
static void place_food(void) {
    for (;;) {
        unsigned char x = (unsigned char)rng_range(PLAY_W);
        unsigned char y = (unsigned char)rng_range(PLAY_H);
        /* Reject if on the snake (head or body) */
        if (snake_head()->x == x && snake_head()->y == y) continue;
        if (snake_body_contains(x, y)) continue;
        g_food.x = x;
        g_food.y = y;
        draw_cell(x, y, '*');
        return;
    }
}

/* ============================================================
 *  Input: arrow keys via ANSI escape sequences
 *
 *  An arrow key arrives as three bytes:  ESC '[' A/B/C/D
 *  We poll non-blockingly each frame, draining whatever's
 *  available, and update direction (with the classic "no
 *  180-degree turn" rule).
 * ============================================================ */

typedef enum {
    INPUT_NONE = 0,
    INPUT_UP,
    INPUT_DOWN,
    INPUT_RIGHT,
    INPUT_LEFT,
    INPUT_QUIT,
} InputAction;

/* Tiny state machine for the arrow-key escape sequences.
 *
 * Modern terminals use one of two encodings:
 *   ESC [ A  — "cursor key mode" (the default in xterm-likes)
 *   ESC O A  — "application cursor key mode" (vt100, some
 *              configurations of mintty / Windows Terminal)
 *
 * We accept both. After the intro (ESC [ or ESC O) we expect
 * one of A/B/C/D for up/down/right/left.
 *
 * States:
 *   0 — ground
 *   1 — saw ESC, expecting [ or O
 *   2 — saw ESC[ or ESCO, expecting A/B/C/D
 */
static int g_esc_state = 0;

static InputAction poll_input(void) {
    InputAction latest = INPUT_NONE;
    char c;
    while (sys_read(0, &c, 1) > 0) {
        if (g_esc_state == 0) {
            if (c == 0x1b) { g_esc_state = 1; }
            else if (c == 'q' || c == 'Q' || c == 0x03 /* Ctrl-C */) {
                return INPUT_QUIT;
            }
            /* Direct WASD support too — convenient when arrows
             * aren't easy to type. */
            else if (c == 'w' || c == 'W') latest = INPUT_UP;
            else if (c == 's' || c == 'S') latest = INPUT_DOWN;
            else if (c == 'a' || c == 'A') latest = INPUT_LEFT;
            else if (c == 'd' || c == 'D') latest = INPUT_RIGHT;
            /* Vi keys for the keyboard purists. */
            else if (c == 'k') latest = INPUT_UP;
            else if (c == 'j') latest = INPUT_DOWN;
            else if (c == 'h') latest = INPUT_LEFT;
            else if (c == 'l') latest = INPUT_RIGHT;
        } else if (g_esc_state == 1) {
            /* Either '[' (cursor-key mode) or 'O' (application
             * cursor-key mode). Both lead to the same A/B/C/D
             * suffix. */
            if (c == '[' || c == 'O') g_esc_state = 2;
            else                      g_esc_state = 0;   /* malformed; resync */
        } else { /* g_esc_state == 2 */
            switch (c) {
                case 'A': latest = INPUT_UP;    break;
                case 'B': latest = INPUT_DOWN;  break;
                case 'C': latest = INPUT_RIGHT; break;
                case 'D': latest = INPUT_LEFT;  break;
                default: break;
            }
            g_esc_state = 0;
        }
    }
    return latest;
}

/* Apply a direction change, refusing 180-degree turns (can't
 * reverse into your own neck). */
static void apply_direction(InputAction a) {
    signed char ndx = g_dx, ndy = g_dy;
    switch (a) {
        case INPUT_UP:    ndx =  0; ndy = -1; break;
        case INPUT_DOWN:  ndx =  0; ndy =  1; break;
        case INPUT_RIGHT: ndx =  1; ndy =  0; break;
        case INPUT_LEFT:  ndx = -1; ndy =  0; break;
        default: return;
    }
    /* Reject the exact reverse */
    if (ndx == -g_dx && ndy == -g_dy) return;
    g_dx = ndx;
    g_dy = ndy;
}

/* ============================================================
 *  Main game loop
 * ============================================================ */

void _start(void) {
    /* Enter raw mode + hide cursor + clear screen */
    sys_tty_set_raw(1);
    ansi_hide_cursor();
    ansi_clear_screen();

    /* Initialize snake: 4 cells wide, horizontal, near center */
    g_length = 4;
    g_head_idx = 3;
    g_tail_idx = 0;
    for (unsigned i = 0; i < 4; i++) {
        g_snake[i].x = (unsigned char)(PLAY_W / 2 - 2 + (int)i);
        g_snake[i].y = (unsigned char)(PLAY_H / 2);
    }

    rng_seed(sys_ticks_now());

    draw_border();
    /* Draw initial snake body */
    for (unsigned i = g_tail_idx; ; i = (i + 1) % MAX_SNAKE) {
        char ch = (i == g_head_idx) ? 'O' : 'o';
        draw_cell(g_snake[i].x, g_snake[i].y, ch);
        if (i == g_head_idx) break;
    }
    place_food();

    /* Drain any stale input that was sitting in stdin before we
     * entered raw mode (e.g., the Enter that submitted the 'run'
     * command, or any keystrokes the user happened to type while
     * the shell was launching us). Without this, a stray byte
     * could be interpreted as 'q' and we'd exit immediately.
     *
     * Read for ~150 ms, discarding everything. The user's
     * intended input starts after this grace window. */
    unsigned hz = sys_tick_hz();
    if (hz == 0) hz = 1000;            /* fallback: assume ms */
    {
        unsigned drain_until = sys_ticks_now() + (hz / 7);  /* ~150 ms */
        char junk;
        while ((int)(sys_ticks_now() - drain_until) < 0) {
            while (sys_read(0, &junk, 1) > 0) { /* discard */ }
            /* Small yield so we're not spinning at full speed */
            register int a7 asm("a7") = SYS_YIELD;
            asm volatile ("ecall" :: "r"(a7) : "memory");
        }
    }
    /* Reset the escape state machine in case we partially drained
     * a sequence. */
    g_esc_state = 0;

    /* Game-loop pacing. tick_hz is 1000 on the PC host (1 ms).
     * Period 125 ms gives a comfortable speed; tune to taste. */
    unsigned period = hz / 8;           /* ~125 ms / frame */
    unsigned next = sys_ticks_now() + period;

    int game_over = 0;
    const char *over_reason = "";

    while (!game_over) {
        /* 1. Drain any pending input. */
        InputAction a = poll_input();
        if (a == INPUT_QUIT) break;
        if (a != INPUT_NONE) apply_direction(a);

        /* 2. Compute new head position. */
        Cell h = *snake_head();
        int nx = (int)h.x + g_dx;
        int ny = (int)h.y + g_dy;

        /* 3. Wall collision */
        if (nx < 0 || nx >= PLAY_W || ny < 0 || ny >= PLAY_H) {
            game_over = 1;
            over_reason = "hit a wall";
            break;
        }

        unsigned char ux = (unsigned char)nx;
        unsigned char uy = (unsigned char)ny;

        /* 4. Self-collision (against the body, excluding the
         * tail cell which is about to vacate — but only if we're
         * not eating, since eating means we don't pop the tail). */
        int eating = (ux == g_food.x && uy == g_food.y);
        /* For simplicity we test against the current body
         * including the tail. False positive only if the new
         * head lands on the tail cell that's about to leave,
         * which is a legal move in most snake variants. We
         * tolerate the slight conservatism. */
        if (snake_body_contains(ux, uy)) {
            game_over = 1;
            over_reason = "bit yourself";
            break;
        }

        /* 5. Erase old tail (unless eating, in which case grow) */
        if (!eating) {
            Cell t = g_snake[g_tail_idx];
            draw_cell(t.x, t.y, ' ');
            snake_pop_tail();
        }

        /* 6. Demote previous head to body, advance head */
        Cell prev_head = *snake_head();
        draw_cell(prev_head.x, prev_head.y, 'o');
        Cell new_head = { ux, uy };
        snake_push_head(new_head);
        draw_cell(ux, uy, 'O');

        /* 7. Food eaten? */
        if (eating) {
            g_score++;
            update_score_line();
            place_food();
        }

        /* 8. Pace the loop. Autoreload: bumping `next` by period
         * means a slow frame doesn't accumulate drift. */
        sys_sleep_until(next);
        next += period;
    }

    /* Game over: show a centered message for ~1 second, then
     * clean up. */
    if (game_over) {
        ansi_goto(4 + PLAY_H / 2, 2 + (PLAY_W / 2 - 8));
        puts_("GAME OVER — ");
        puts_(over_reason);
        ansi_goto(4 + PLAY_H / 2 + 1, 2 + (PLAY_W / 2 - 7));
        puts_("score: ");
        putd(g_score);
        /* Sleep ~1.5 seconds so the player can read it */
        sys_sleep_until(sys_ticks_now() + (hz + hz/2));
    }

    /* Cleanup: clear the screen, restore cursor, leave the
     * terminal in cooked mode for the parent shell. */
    ansi_clear_screen();
    ansi_home();
    ansi_show_cursor();
    sys_tty_set_raw(0);
    sys_exit(0);
}
