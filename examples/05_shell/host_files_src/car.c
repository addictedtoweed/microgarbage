/* car.c — mouse-tracking car dodge game.
 *
 * You control a 3-row tall car at the bottom of a scrolling
 * road. Move the mouse to steer left/right (and up/down within
 * a dodge window). Dodge oncoming cars, debris, and oil slicks.
 * Grab the yellow ◆ pickups for bonus score. Road bends; trees
 * and road signs drift past on the shoulders.
 *
 * Score = distance survived + pickups grabbed. Game continues
 * until you crash (overlap an obstacle or run off the road).
 *
 * Press q or Esc to quit.
 *
 * Public domain (CC0).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "lib/tui.h"

/* Syscall numbers for things tui.h doesn't expose. */
#define SYS_EXIT             93
#define SYS_TICKS_NOW      1043
#define SYS_SLEEP_TICKS    1045
#define SYS_RAND           1112

static inline uint32_t sys0(uint32_t n) {
    register uint32_t a0 asm("a0");
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "=r"(a0) : "r"(a7) : "memory");
    return a0;
}
static inline void sys1(uint32_t n, uint32_t a) {
    register uint32_t a0 asm("a0") = a;
    register uint32_t a7 asm("a7") = n;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
}

/* ============================================================
 *  Layout
 * ============================================================ */

#define ROWS         30
#define COLS         80

#define HUD_ROW       1     /* top row reserved for score */
#define GAME_TOP      2     /* first scrollable row */
#define GAME_BOTTOM  30     /* last row */
#define GAME_ROWS    (GAME_BOTTOM - GAME_TOP + 1)  /* 29 rows */

#define ROAD_WIDTH    16    /* total road width including stripes */
#define PLAYABLE_W    12    /* inside-the-stripes width */

#define CAR_HEIGHT    3
#define CAR_WIDTH     4
#define CAR_BOTTOM   28     /* default car-bottom row */
#define CAR_DODGE_TOP 22    /* mouse can pull the car up to here */

/* ============================================================
 *  Object kinds — what can appear on a given row
 * ============================================================ */

typedef enum {
    OBJ_NONE        = 0,
    OBJ_ENEMY_CAR   = 1,  /* oncoming car (red, 2x3) */
    OBJ_DEBRIS      = 2,  /* gray block (1x2) */
    OBJ_OIL         = 3,  /* yellow puddle (1x2) */
    OBJ_PICKUP      = 4,  /* gold diamond (1x1) */
    OBJ_TREE        = 5,  /* roadside tree (green) */
    OBJ_SIGN_R      = 6,  /* roadside sign with right arrow */
    OBJ_SIGN_L      = 7,
    OBJ_BUSH        = 8,
} ObjKind;

typedef struct {
    uint8_t  kind;     /* ObjKind */
    int8_t   col_off;  /* column offset from road center for road objects,
                        * or absolute column for roadside objects */
    uint8_t  height;   /* in rows, 1..3 */
} Obj;

/* ============================================================
 *  World state
 *
 *  One row per scrolling slice. Each frame: shift down by
 *  scroll_speed rows, spawn new top rows, render.
 * ============================================================ */

typedef struct {
    int8_t  road_center;  /* column of road center (1-indexed) */
    Obj     left_scenery;
    Obj     right_scenery;
    Obj     road_obj;     /* obstacle/pickup on this row */
    int8_t  road_obj_col; /* absolute column where the road object sits */
} Row;

static Row g_world[GAME_ROWS + 4];   /* small headroom for scroll */

/* Player state */
static int   g_car_col = COLS / 2;   /* center column of car */
static int   g_car_row = CAR_BOTTOM; /* bottom row of car */
static int   g_score = 0;
static int   g_distance = 0;          /* increments every scroll */
static int   g_speed = 1;             /* rows per frame */
static bool  g_dead = false;
static int   g_death_frames = 0;      /* count frames after death for end screen */

/* Road bend state */
static int   g_bend_target_center = COLS / 2;  /* where the road *wants* to be */
static int   g_bend_change_in = 30;            /* rows until next target shift */

/* Cached tiles */
static TuiTileId g_tile_car;
static TuiTileId g_tile_enemy;
static TuiTileId g_tile_debris;
static TuiTileId g_tile_oil;
static TuiTileId g_tile_pickup;
static TuiTileId g_tile_tree;
static TuiTileId g_tile_bush;
static TuiTileId g_tile_sign_r;
static TuiTileId g_tile_sign_l;

/* ============================================================
 *  PRNG (host-backed)
 * ============================================================ */

static uint32_t myrand(void) {
    return sys0(SYS_RAND);
}

static int rand_in_range(int lo, int hi) {  /* inclusive */
    if (hi <= lo) return lo;
    return lo + (int)(myrand() % (uint32_t)(hi - lo + 1));
}

/* ============================================================
 *  Tile setup — build all the sprites once at startup
 * ============================================================ */

static void make_car_tile(void) {
    /* 3 rows × 4 cols. Layout:
     *
     *   row 1:    ▄██▄        cabin top, red
     *   row 2:    ████        body, red
     *   row 3:    ▀▀▀▀        wheels, dark gray
     */
    g_tile_car = tui_tile_create(3, 4);
    if (g_tile_car == TUI_TILE_NONE) return;
    tui_tile_fill(g_tile_car, ' ', TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR, 0);
    /* row 1 */
    tui_tile_set(g_tile_car, 1, 1, TUI_BLOCK_LOWER_HALF, 1, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_car, 1, 2, TUI_BLOCK_FULL,       1, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_car, 1, 3, TUI_BLOCK_FULL,       1, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_car, 1, 4, TUI_BLOCK_LOWER_HALF, 1, TUI_DEFAULT_COLOR, 0);
    /* row 2 — body */
    tui_tile_set(g_tile_car, 2, 1, TUI_BLOCK_FULL, 9, TUI_DEFAULT_COLOR, 0);   /* bright red */
    tui_tile_set(g_tile_car, 2, 2, TUI_BLOCK_FULL, 11, TUI_DEFAULT_COLOR, 0);  /* bright yellow center */
    tui_tile_set(g_tile_car, 2, 3, TUI_BLOCK_FULL, 11, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_car, 2, 4, TUI_BLOCK_FULL, 9, TUI_DEFAULT_COLOR, 0);
    /* row 3 — wheels */
    tui_tile_set(g_tile_car, 3, 1, TUI_BLOCK_UPPER_HALF, 8, TUI_DEFAULT_COLOR, 0);  /* dark gray */
    tui_tile_set(g_tile_car, 3, 2, ' ',                  TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_car, 3, 3, ' ',                  TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_car, 3, 4, TUI_BLOCK_UPPER_HALF, 8, TUI_DEFAULT_COLOR, 0);
}

static void make_enemy_tile(void) {
    /* 2 rows × 3 cols, red oncoming car. */
    g_tile_enemy = tui_tile_create(2, 3);
    if (g_tile_enemy == TUI_TILE_NONE) return;
    tui_tile_fill(g_tile_enemy, ' ', TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_enemy, 1, 1, TUI_BLOCK_FULL, 9, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_enemy, 1, 2, TUI_TRI_DOWN,   11, 9, 0);   /* yellow arrow on red */
    tui_tile_set(g_tile_enemy, 1, 3, TUI_BLOCK_FULL, 9, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_enemy, 2, 1, TUI_BLOCK_UPPER_HALF, 8, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_enemy, 2, 2, TUI_BLOCK_FULL, 9, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_enemy, 2, 3, TUI_BLOCK_UPPER_HALF, 8, TUI_DEFAULT_COLOR, 0);
}

static void make_debris_tile(void) {
    /* 1 row × 2 cols, gray rubble. */
    g_tile_debris = tui_tile_create(1, 2);
    if (g_tile_debris == TUI_TILE_NONE) return;
    tui_tile_set(g_tile_debris, 1, 1, TUI_SHADE_DARK, 7, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_debris, 1, 2, TUI_SHADE_DARK, 7, TUI_DEFAULT_COLOR, 0);
}

static void make_oil_tile(void) {
    /* 1 row × 2 cols, dark with light shade overlay. */
    g_tile_oil = tui_tile_create(1, 2);
    if (g_tile_oil == TUI_TILE_NONE) return;
    tui_tile_set(g_tile_oil, 1, 1, TUI_SHADE_MEDIUM, 8, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_oil, 1, 2, TUI_SHADE_MEDIUM, 8, TUI_DEFAULT_COLOR, 0);
}

static void make_pickup_tile(void) {
    /* 1 row × 1 col gold diamond. */
    g_tile_pickup = tui_tile_create(1, 1);
    if (g_tile_pickup == TUI_TILE_NONE) return;
    tui_tile_set(g_tile_pickup, 1, 1, TUI_DIAMOND, 11, TUI_DEFAULT_COLOR, TUI_ATTR_BOLD);
}

static void make_tree_tile(void) {
    /* 3 rows × 3 cols. Round-ish leafy crown. */
    g_tile_tree = tui_tile_create(3, 3);
    if (g_tile_tree == TUI_TILE_NONE) return;
    tui_tile_fill(g_tile_tree, ' ', TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR, 0);
    /* Crown row 1 */
    tui_tile_set_transparent(g_tile_tree, 1, 1);
    tui_tile_set(g_tile_tree, 1, 2, TUI_BLOCK_FULL, 2, TUI_DEFAULT_COLOR, 0);  /* green */
    tui_tile_set_transparent(g_tile_tree, 1, 3);
    /* Crown row 2 */
    tui_tile_set(g_tile_tree, 2, 1, TUI_BLOCK_FULL, 2, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_tree, 2, 2, TUI_BLOCK_FULL, 10, TUI_DEFAULT_COLOR, 0);  /* lighter green center */
    tui_tile_set(g_tile_tree, 2, 3, TUI_BLOCK_FULL, 2, TUI_DEFAULT_COLOR, 0);
    /* Trunk */
    tui_tile_set_transparent(g_tile_tree, 3, 1);
    tui_tile_set(g_tile_tree, 3, 2, TUI_BLOCK_FULL, 3, TUI_DEFAULT_COLOR, 0);  /* brown-ish (yellow→brown) */
    tui_tile_set_transparent(g_tile_tree, 3, 3);
}

static void make_bush_tile(void) {
    g_tile_bush = tui_tile_create(1, 2);
    if (g_tile_bush == TUI_TILE_NONE) return;
    tui_tile_set(g_tile_bush, 1, 1, TUI_BLOCK_FULL, 2, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_bush, 1, 2, TUI_BLOCK_FULL, 10, TUI_DEFAULT_COLOR, 0);
}

static void make_sign_tiles(void) {
    /* 2 rows × 3 cols. Yellow post with directional arrow. */
    g_tile_sign_r = tui_tile_create(2, 3);
    if (g_tile_sign_r != TUI_TILE_NONE) {
        tui_tile_fill(g_tile_sign_r, ' ', TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR, 0);
        tui_tile_set(g_tile_sign_r, 1, 1, TUI_BLOCK_FULL, 11, TUI_DEFAULT_COLOR, 0);
        tui_tile_set(g_tile_sign_r, 1, 2, TUI_TRI_RIGHT,  0, 11, 0);    /* black on yellow */
        tui_tile_set(g_tile_sign_r, 1, 3, TUI_BLOCK_FULL, 11, TUI_DEFAULT_COLOR, 0);
        tui_tile_set_transparent(g_tile_sign_r, 2, 1);
        tui_tile_set(g_tile_sign_r, 2, 2, TUI_BLOCK_FULL, 7, TUI_DEFAULT_COLOR, 0);   /* post */
        tui_tile_set_transparent(g_tile_sign_r, 2, 3);
    }

    g_tile_sign_l = tui_tile_create(2, 3);
    if (g_tile_sign_l != TUI_TILE_NONE) {
        tui_tile_fill(g_tile_sign_l, ' ', TUI_DEFAULT_COLOR, TUI_DEFAULT_COLOR, 0);
        tui_tile_set(g_tile_sign_l, 1, 1, TUI_BLOCK_FULL, 11, TUI_DEFAULT_COLOR, 0);
        tui_tile_set(g_tile_sign_l, 1, 2, TUI_TRI_LEFT,   0, 11, 0);
        tui_tile_set(g_tile_sign_l, 1, 3, TUI_BLOCK_FULL, 11, TUI_DEFAULT_COLOR, 0);
        tui_tile_set_transparent(g_tile_sign_l, 2, 1);
        tui_tile_set(g_tile_sign_l, 2, 2, TUI_BLOCK_FULL, 7, TUI_DEFAULT_COLOR, 0);
        tui_tile_set_transparent(g_tile_sign_l, 2, 3);
    }
}

/* ============================================================
 *  World generation
 * ============================================================ */

static int clamp_road_center(int x) {
    int min_center = ROAD_WIDTH / 2 + 2;
    int max_center = COLS - ROAD_WIDTH / 2 - 2;
    if (x < min_center) x = min_center;
    if (x > max_center) x = max_center;
    return x;
}

static void choose_new_bend_target(void) {
    /* Pick a new road center within a reasonable range of current. */
    int cur = g_world[0].road_center;
    int delta = rand_in_range(-15, 15);
    g_bend_target_center = clamp_road_center(cur + delta);
    g_bend_change_in = rand_in_range(20, 40);
}

/* Advance the road centerline by one row. */
static int next_road_center(int prev) {
    g_bend_change_in--;
    if (g_bend_change_in <= 0) {
        choose_new_bend_target();
    }
    /* Drift toward target by 1 col every 3 rows. */
    if (prev < g_bend_target_center) return prev + (rand_in_range(0, 2) == 0 ? 1 : 0);
    if (prev > g_bend_target_center) return prev - (rand_in_range(0, 2) == 0 ? 1 : 0);
    return prev;
}

/* Generate a new top row given the row below it. */
static void generate_row(Row *r, const Row *below) {
    r->road_center = clamp_road_center(next_road_center(below->road_center));
    r->road_obj.kind = OBJ_NONE;
    r->left_scenery.kind = OBJ_NONE;
    r->right_scenery.kind = OBJ_NONE;

    /* Road obstacle (~12% of rows). Difficulty rises with distance. */
    int spawn_chance = 10 + (g_distance / 50);
    if (spawn_chance > 30) spawn_chance = 30;
    if ((int)(myrand() % 100) < spawn_chance) {
        int roll = (int)(myrand() % 100);
        if (roll < 35)      r->road_obj.kind = OBJ_ENEMY_CAR;
        else if (roll < 60) r->road_obj.kind = OBJ_DEBRIS;
        else if (roll < 80) r->road_obj.kind = OBJ_OIL;
        else                r->road_obj.kind = OBJ_PICKUP;

        /* Place it on one of three lanes within the road. */
        int lane = rand_in_range(-1, 1);
        r->road_obj_col = (int8_t)(r->road_center + lane * 4);
    }

    /* Roadside scenery. */
    if ((int)(myrand() % 100) < 50) {
        int roll = (int)(myrand() % 100);
        Obj *o = &r->left_scenery;
        if (roll < 50)      o->kind = OBJ_TREE;
        else if (roll < 70) o->kind = OBJ_BUSH;
        else if (roll < 85) o->kind = OBJ_SIGN_L;
        else                o->kind = OBJ_SIGN_R;
        /* Place ~5 columns left of the road's left edge. */
        int left_edge = r->road_center - ROAD_WIDTH / 2;
        o->col_off = (int8_t)(left_edge - 4 - rand_in_range(0, 2));
        if (o->col_off < 2) o->kind = OBJ_NONE;
    }

    if ((int)(myrand() % 100) < 50) {
        int roll = (int)(myrand() % 100);
        Obj *o = &r->right_scenery;
        if (roll < 50)      o->kind = OBJ_TREE;
        else if (roll < 70) o->kind = OBJ_BUSH;
        else if (roll < 85) o->kind = OBJ_SIGN_R;
        else                o->kind = OBJ_SIGN_L;
        int right_edge = r->road_center + ROAD_WIDTH / 2;
        o->col_off = (int8_t)(right_edge + 4 + rand_in_range(0, 2));
        if (o->col_off > COLS - 1) o->kind = OBJ_NONE;
    }
}

/* Shift the world down by `n` rows (toward the player). */
static void scroll_world(int n) {
    if (n <= 0) return;
    /* Move everything down. */
    for (int i = GAME_ROWS - 1; i >= n; i--) {
        g_world[i] = g_world[i - n];
    }
    /* Generate new top rows. */
    for (int i = n - 1; i >= 0; i--) {
        const Row *below = (i + 1 < GAME_ROWS) ? &g_world[i + 1] : &g_world[i];
        generate_row(&g_world[i], below);
    }
    g_distance += n;
    /* Speed up every 100 distance, cap at 3. */
    g_speed = 1 + (g_distance / 100);
    if (g_speed > 3) g_speed = 3;
}

static void world_init(void) {
    /* Seed initial state: straight road centered, no obstacles
     * in the first few rows so the player gets a moment. */
    for (int i = 0; i < GAME_ROWS; i++) {
        g_world[i].road_center = COLS / 2;
        g_world[i].road_obj.kind = OBJ_NONE;
        g_world[i].left_scenery.kind = OBJ_NONE;
        g_world[i].right_scenery.kind = OBJ_NONE;
    }
    g_bend_target_center = COLS / 2;
    g_bend_change_in = 25;
}

/* ============================================================
 *  Rendering
 * ============================================================ */

static void draw_hud(void) {
    /* Top row: dark background. */
    tui_fill_rect(HUD_ROW, 1, 1, COLS, ' ');

    char buf[64];
    snprintf(buf, sizeof(buf), " SCORE %5d   DIST %5d   SPD %d ",
             g_score, g_distance, g_speed);
    tui_set_fg(11);                       /* yellow */
    tui_set_bg(TUI_DEFAULT_COLOR);
    tui_set_attr(TUI_ATTR_BOLD);
    tui_move(HUD_ROW, 2);
    tui_puts(buf);
    tui_reset();
}

static void draw_road_row(int screen_row, const Row *r) {
    int left_edge  = r->road_center - ROAD_WIDTH / 2;
    int right_edge = r->road_center + ROAD_WIDTH / 2;

    /* Off-road backdrop fills row with grass color. */
    tui_set_fg(TUI_DEFAULT_COLOR);
    tui_set_bg(2);                        /* green grass */
    tui_fill_rect(screen_row, 1, 1, COLS, ' ');

    /* Road surface: dark gray. */
    if (left_edge >= 1 && right_edge <= COLS) {
        tui_set_fg(TUI_DEFAULT_COLOR);
        tui_set_bg(8);                    /* dark gray asphalt */
        tui_fill_rect(screen_row, left_edge, 1, right_edge - left_edge + 1, ' ');

        /* Road stripes — solid lines on the edges. */
        tui_set_fg(11);                   /* yellow */
        tui_set_bg(8);
        tui_set_cell(screen_row, left_edge,  TUI_BLOCK_FULL, 11, 8, 0);
        tui_set_cell(screen_row, right_edge, TUI_BLOCK_FULL, 11, 8, 0);

        /* Center dashed line: white dash every other row. */
        if ((g_distance + screen_row) & 1) {
            tui_set_cell(screen_row, r->road_center,
                         TUI_BLOCK_FULL, 15, 8, 0);  /* bright white */
        }
    }
    tui_reset();
}

static void draw_obj_on_road(int screen_row, const Row *r) {
    if (r->road_obj.kind == OBJ_NONE) return;
    int col = r->road_obj_col;
    if (col < 2 || col > COLS - 1) return;

    switch (r->road_obj.kind) {
        case OBJ_ENEMY_CAR:
            /* 2-row tile, blit so its bottom row is screen_row. */
            tui_blit_tile(g_tile_enemy, screen_row - 1, col - 1);
            break;
        case OBJ_DEBRIS:
            tui_blit_tile(g_tile_debris, screen_row, col);
            break;
        case OBJ_OIL:
            tui_blit_tile(g_tile_oil, screen_row, col);
            break;
        case OBJ_PICKUP:
            tui_blit_tile(g_tile_pickup, screen_row, col);
            break;
    }
}

static void draw_scenery(int screen_row, const Obj *o) {
    if (o->kind == OBJ_NONE) return;
    int col = o->col_off;
    if (col < 1 || col > COLS) return;
    switch (o->kind) {
        case OBJ_TREE:
            tui_blit_tile(g_tile_tree, screen_row - 2, col - 1);
            break;
        case OBJ_BUSH:
            tui_blit_tile(g_tile_bush, screen_row, col);
            break;
        case OBJ_SIGN_R:
            tui_blit_tile(g_tile_sign_r, screen_row - 1, col - 1);
            break;
        case OBJ_SIGN_L:
            tui_blit_tile(g_tile_sign_l, screen_row - 1, col - 1);
            break;
    }
}

static void draw_world(void) {
    /* Draw road from top to bottom (so taller tiles can extend
     * upward into already-drawn rows). */
    for (int i = 0; i < GAME_ROWS; i++) {
        int screen_row = GAME_TOP + i;
        const Row *r = &g_world[i];
        draw_road_row(screen_row, r);
    }
    /* Scenery and obstacles in a second pass so multi-row tiles
     * overlay cleanly. */
    for (int i = 0; i < GAME_ROWS; i++) {
        int screen_row = GAME_TOP + i;
        const Row *r = &g_world[i];
        draw_scenery(screen_row, &r->left_scenery);
        draw_scenery(screen_row, &r->right_scenery);
        draw_obj_on_road(screen_row, r);
    }
}

static void draw_car(void) {
    int top_row = g_car_row - CAR_HEIGHT + 1;
    int left_col = g_car_col - CAR_WIDTH / 2;
    if (top_row < GAME_TOP) top_row = GAME_TOP;
    if (left_col < 1) left_col = 1;
    if (left_col + CAR_WIDTH - 1 > COLS) left_col = COLS - CAR_WIDTH + 1;
    tui_blit_tile(g_tile_car, top_row, left_col);
}

/* ============================================================
 *  Collision detection
 *
 *  The car occupies 3 rows × 4 cols centered at (g_car_row, g_car_col).
 *  Bottom row = g_car_row, top row = g_car_row - 2.
 * ============================================================ */

static int car_top_world_row(void) {
    /* World-row index (0 = top of game area). */
    return (g_car_row - CAR_HEIGHT + 1) - GAME_TOP;
}

static int car_bot_world_row(void) {
    return g_car_row - GAME_TOP;
}

static bool car_overlaps_col(int col) {
    int left = g_car_col - CAR_WIDTH / 2;
    int right = left + CAR_WIDTH - 1;
    return col >= left && col <= right;
}

static void check_collisions(void) {
    int top = car_top_world_row();
    int bot = car_bot_world_row();
    if (top < 0) top = 0;
    if (bot >= GAME_ROWS) bot = GAME_ROWS - 1;
    for (int i = top; i <= bot; i++) {
        const Row *r = &g_world[i];
        /* Off the road? */
        int left_edge  = r->road_center - ROAD_WIDTH / 2;
        int right_edge = r->road_center + ROAD_WIDTH / 2;
        int car_left  = g_car_col - CAR_WIDTH / 2;
        int car_right = car_left + CAR_WIDTH - 1;
        if (car_left < left_edge + 1 || car_right > right_edge - 1) {
            g_dead = true;
            return;
        }
        /* Road object overlap? */
        if (r->road_obj.kind == OBJ_NONE) continue;
        int oc = r->road_obj_col;
        int o_left, o_right;
        switch (r->road_obj.kind) {
            case OBJ_ENEMY_CAR: o_left = oc - 1; o_right = oc + 1; break;
            case OBJ_DEBRIS:
            case OBJ_OIL:      o_left = oc;     o_right = oc + 1; break;
            case OBJ_PICKUP:   o_left = oc;     o_right = oc;     break;
            default: continue;
        }
        if (car_right >= o_left && car_left <= o_right) {
            if (r->road_obj.kind == OBJ_PICKUP) {
                g_score += 50;
                /* Consume it. */
                Row *mr = &g_world[i];
                mr->road_obj.kind = OBJ_NONE;
            } else {
                g_dead = true;
                return;
            }
        }
    }
}

/* ============================================================
 *  Input handling
 * ============================================================ */

static void process_input(void) {
    TuiEvent ev;
    while (tui_poll_event(&ev)) {
        if (ev.kind == TUI_EV_KEY) {
            if (ev.key.key == 'q' || ev.key.key == 'Q' ||
                ev.key.key == TUI_KEY_ESCAPE) {
                g_dead = true;
                g_death_frames = 30;   /* skip end screen and exit */
                return;
            }
        } else if (ev.kind == TUI_EV_MOUSE) {
            /* Track position to where the mouse is. */
            int target_col = ev.mouse.col;
            int target_row = ev.mouse.row;
            if (target_col < 1) target_col = 1;
            if (target_col > COLS) target_col = COLS;
            g_car_col = target_col;
            if (target_row >= CAR_DODGE_TOP && target_row <= CAR_BOTTOM) {
                g_car_row = target_row;
            } else if (target_row < CAR_DODGE_TOP) {
                g_car_row = CAR_DODGE_TOP;
            } else if (target_row > CAR_BOTTOM) {
                g_car_row = CAR_BOTTOM;
            }
        }
    }
}

/* ============================================================
 *  End screen
 * ============================================================ */

static void draw_game_over(void) {
    /* Center a banner in the canvas. */
    const char *line1 = "  CRASH!  ";
    const char *line2 = "Press q to exit";
    char score_line[40];
    snprintf(score_line, sizeof(score_line), "Final score: %d  Distance: %d",
             g_score, g_distance);

    int mid_row = ROWS / 2;
    int half_w = 18;
    int box_left = (COLS - 2 * half_w) / 2;

    tui_set_fg(0);
    tui_set_bg(9);  /* red */
    tui_fill_rect(mid_row - 2, box_left, 5, 2 * half_w, ' ');
    tui_set_attr(TUI_ATTR_BOLD);
    tui_move(mid_row - 1, box_left + (2 * half_w - (int)strlen(line1)) / 2);
    tui_puts(line1);
    tui_set_attr(0);
    tui_set_fg(11);
    tui_move(mid_row, box_left + (2 * half_w - (int)strlen(score_line)) / 2);
    tui_puts(score_line);
    tui_set_fg(15);
    tui_move(mid_row + 1, box_left + (2 * half_w - (int)strlen(line2)) / 2);
    tui_puts(line2);
    tui_reset();
}

/* ============================================================
 *  Main loop
 * ============================================================ */

int main(void) {
    if (!tui_init(TUI_USE_ALT_SCREEN | TUI_HIDE_CURSOR | TUI_USE_MOUSE |
                  TUI_USE_RAW | TUI_USE_SYNC_OUTPUT,
                  ROWS, COLS)) {
        return 1;
    }

    make_car_tile();
    make_enemy_tile();
    make_debris_tile();
    make_oil_tile();
    make_pickup_tile();
    make_tree_tile();
    make_bush_tile();
    make_sign_tiles();

    world_init();

    /* Frame loop. Target ~15 FPS for smooth scrolling. */
    while (!g_dead) {
        process_input();
        if (g_dead) break;
        scroll_world(g_speed);
        check_collisions();
        if (g_dead) break;
        /* Pickup credit accrues; distance also gives passive score. */
        g_score += g_speed;
        draw_hud();
        draw_world();
        draw_car();
        tui_present_diff();
        sys1(SYS_SLEEP_TICKS, 66);  /* ~15 FPS */
    }

    /* End screen until the user presses q or 5 seconds pass. */
    int wait_frames = 75;     /* ~5s at 15 FPS */
    while (wait_frames-- > 0) {
        draw_world();
        draw_car();
        draw_game_over();
        tui_present();
        TuiEvent ev;
        while (tui_poll_event(&ev)) {
            if (ev.kind == TUI_EV_KEY &&
                (ev.key.key == 'q' || ev.key.key == 'Q' ||
                 ev.key.key == TUI_KEY_ESCAPE ||
                 ev.key.key == TUI_KEY_ENTER)) {
                wait_frames = 0;
                break;
            }
        }
        sys1(SYS_SLEEP_TICKS, 66);
    }

    tui_shutdown();
    sys1(SYS_EXIT, 0);
    return 0;
}
