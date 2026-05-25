/* car.c — car dodge game (keyboard or mouse steering).
 *
 * You control a 3-row tall car at the bottom of a scrolling
 * road. Steer with the arrow keys / WASD / hjkl, or with the
 * mouse if your terminal reports pointer motion. Dodge oncoming
 * cars, debris, and oil slicks. Grab the yellow diamonds for
 * bonus score. Road bends; trees and road signs drift past on
 * the shoulders.
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

#include "tui.h"

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

#define ROAD_WIDTH    22    /* total road width including stripes */
#define PLAYABLE_W    18    /* inside-the-stripes width */

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
    /* 1 row × 2 cols. Bright orange/tan rubble on the road.
     * Uses xterm 256-color 208 (bright orange) which contrasts
     * sharply with the asphalt's dark gray bg=8. Full blocks
     * rather than shaded for maximum visibility. */
    g_tile_debris = tui_tile_create(1, 2);
    if (g_tile_debris == TUI_TILE_NONE) return;
    tui_tile_set(g_tile_debris, 1, 1, TUI_BLOCK_FULL, 208, TUI_DEFAULT_COLOR, 0);
    tui_tile_set(g_tile_debris, 1, 2, TUI_BLOCK_FULL, 208, TUI_DEFAULT_COLOR, 0);
}

static void make_oil_tile(void) {
    /* 1 row × 2 cols. Iridescent oil slick — bright cyan-ish
     * (xterm 256-color 51) on a dark blue (17) bg so it reads
     * as "wet, slick, blue-black" rather than blending into the
     * gray asphalt. Medium-shade glyph for the textured look. */
    g_tile_oil = tui_tile_create(1, 2);
    if (g_tile_oil == TUI_TILE_NONE) return;
    tui_tile_set(g_tile_oil, 1, 1, TUI_SHADE_MEDIUM, 51, 17, 0);
    tui_tile_set(g_tile_oil, 1, 2, TUI_SHADE_MEDIUM, 51, 17, 0);
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

/* True if the user explicitly quit via q/Esc during gameplay
 * (as opposed to crashing). The main loop checks this to skip
 * the game-over menu and return to title. */
static bool g_user_quit = false;

static void process_input(void) {
    TuiEvent ev;
    while (tui_poll_event(&ev)) {
        if (ev.kind == TUI_EV_KEY) {
            int k = ev.key.key;
            if (k == 'q' || k == 'Q' || k == TUI_KEY_ESCAPE) {
                g_dead = true;
                g_user_quit = true;
                return;
            }
            /* Keyboard steering: arrows / WASD / hjkl. Works on any
             * client, including terminals that don't report mouse
             * motion (e.g. PuTTY, which doesn't implement xterm
             * any-motion tracking) and on hardware with no mouse at
             * all. Each press nudges the car one cell; hold to move
             * continuously via key repeat. */
            int dcol = 0, drow = 0;
            if (k == TUI_KEY_LEFT  || k == 'a' || k == 'A' || k == 'h') dcol = -1;
            else if (k == TUI_KEY_RIGHT || k == 'd' || k == 'D' || k == 'l') dcol = +1;
            else if (k == TUI_KEY_UP   || k == 'w' || k == 'W' || k == 'k') drow = -1;
            else if (k == TUI_KEY_DOWN || k == 's' || k == 'S' || k == 'j') drow = +1;

            if (dcol) {
                int t = g_car_col + dcol;
                if (t < 1) t = 1;
                if (t > COLS) t = COLS;
                g_car_col = t;
            }
            if (drow) {
                int t = g_car_row + drow;
                if (t < CAR_DODGE_TOP) t = CAR_DODGE_TOP;
                if (t > CAR_BOTTOM)    t = CAR_BOTTOM;
                g_car_row = t;
            }
        } else if (ev.kind == TUI_EV_MOUSE) {
            /* Mouse steering (when the client reports motion or drag):
             * track the car to the pointer. */
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

/* ============================================================
 *  Title screen, countdown, and game-over menu
 *
 *  Flow:
 *    show_title_screen()   → user clicks START or hits Space/Enter,
 *                            returns 1; or QUIT/Esc returns 0.
 *    show_countdown()      → flashes "3" "2" "1" "GO!", ~500ms each.
 *    [gameplay]
 *    show_game_over_menu() → CONTINUE / EXIT, returns 1 or 0.
 *
 *  Each screen draws over the canvas without altering world state,
 *  so the game-over menu can sit on top of the final-frame
 *  playfield as a translucent-feeling modal.
 * ============================================================ */

/* Render a road-themed title screen with the SPYHUNTER-ish vibe.
 * Returns 1 if user starts, 0 if user quits. */
static int show_title_screen(void) {
    /* Background: dark with a faint road suggestion in the middle. */
    tui_set_bg(0);
    tui_fill_rect(1, 1, ROWS, COLS, ' ');

    /* Vertical road suggestion down the center: dark gray strip. */
    int road_left  = (COLS - ROAD_WIDTH) / 2;
    int road_right = road_left + ROAD_WIDTH - 1;
    tui_set_bg(8);
    tui_fill_rect(2, road_left, ROWS - 1, ROAD_WIDTH, ' ');
    /* Yellow stripes on the edges. */
    tui_set_fg(11);
    tui_set_bg(8);
    for (int r = 2; r < ROWS; r++) {
        tui_set_cell(r, road_left,  TUI_BLOCK_FULL, 11, 8, 0);
        tui_set_cell(r, road_right, TUI_BLOCK_FULL, 11, 8, 0);
    }
    /* Center dashed line. */
    int center = (road_left + road_right) / 2;
    for (int r = 2; r < ROWS; r++) {
        if (r & 1) tui_set_cell(r, center, TUI_BLOCK_FULL, 15, 8, 0);
    }
    tui_reset();

    /* Title text — big and offset. */
    tui_set_bg(0);
    tui_set_fg(196);   /* bright red */
    tui_set_attr(TUI_ATTR_BOLD);
    tui_move(5, (COLS - 11) / 2);
    tui_puts("ROAD DODGE");
    tui_reset();

    tui_set_bg(0);
    tui_set_fg(15);
    tui_move(7, (COLS - 36) / 2);
    tui_puts("Arrows / WASD / mouse to steer");
    tui_move(8, (COLS - 32) / 2);
    tui_puts("Grab the gold diamonds for +50");
    tui_reset();

    /* START + QUIT buttons. */
    TuiButton start_btn = {
        .row = 13, .col = COLS / 2 - 14, .h = 3, .w = 12,
        .label = "START",
        .fg = TUI_BRIGHT_WHITE, .bg = 28,           /* deep green */
        .pressed_fg = TUI_BRIGHT_WHITE, .pressed_bg = 22,
    };
    TuiButton quit_btn = {
        .row = 13, .col = COLS / 2 + 2, .h = 3, .w = 12,
        .label = "QUIT",
        .fg = TUI_BRIGHT_WHITE, .bg = 88,           /* dark red */
        .pressed_fg = TUI_BRIGHT_WHITE, .pressed_bg = 52,
    };
    tui_button_draw(&start_btn);
    tui_button_draw(&quit_btn);

    tui_set_bg(0);
    tui_set_fg(8);
    tui_move(18, (COLS - 36) / 2);
    tui_puts("click button, or Enter/Space/Esc");
    tui_reset();

    tui_present();

    /* Drain stale input (e.g. the Enter that launched us). */
    {
        unsigned drain_ms = 150;
        unsigned start_ms = sys0(SYS_TICKS_NOW);
        while ((int)(sys0(SYS_TICKS_NOW) - start_ms) < (int)drain_ms) {
            TuiEvent junk;
            while (tui_poll_event(&junk)) { }
            sys1(SYS_SLEEP_TICKS, 20);
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
        sys1(SYS_SLEEP_TICKS, 20);
    }
}

/* Show a 3-2-1-GO countdown overlay on top of the (already drawn)
 * world. Each step holds for ~500ms, GO! for ~300ms. The world
 * is redrawn behind the overlay each step so the player sees
 * the road preview. */
static void show_countdown(void) {
    const char *steps[4] = { "3", "2", "1", "GO!" };
    unsigned hold_ms[4]   = { 500, 500, 500, 300 };

    for (int i = 0; i < 4; i++) {
        /* Redraw the world underneath so it looks alive. */
        draw_hud();
        draw_world();
        draw_car();

        /* Big number in the center, drawn last so it's on top. */
        int row = ROWS / 2;
        int col = (COLS - 8) / 2;
        TuiColor fg = (i == 3) ? 46 : 226;     /* green for GO, yellow for digits */

        tui_set_bg(0);
        tui_set_fg(fg);
        tui_set_attr(TUI_ATTR_BOLD);
        /* Bracket the digit with a small filled border for emphasis. */
        tui_fill_rect(row - 1, col, 3, 8, ' ');
        const char *s = steps[i];
        int slen_s = (int)strlen(s);
        int label_col = col + (8 - slen_s) / 2;
        tui_move(row, label_col);
        tui_puts(s);
        tui_reset();

        tui_present_diff();
        sys1(SYS_SLEEP_TICKS, hold_ms[i]);
    }
}

/* Game-over menu. Returns 1 to play again, 0 to exit. */
static int show_game_over_menu(void) {
    int box_h = 9;
    int box_w = 40;
    int box_row = ROWS / 2 - box_h / 2;
    int box_col = (COLS - box_w) / 2;

    /* Dim background by overdrawing the modal box solid. */
    tui_set_bg(0);
    tui_fill_rect(box_row, box_col, box_h, box_w, ' ');

    /* Red double-border. */
    tui_set_fg(196);
    tui_set_attr(TUI_ATTR_BOLD);
    tui_box_double(box_row, box_col, box_h, box_w);
    tui_reset();

    /* "CRASH!" title. */
    tui_set_bg(0);
    tui_set_fg(196);
    tui_set_attr(TUI_ATTR_BOLD);
    const char *title = "CRASH!";
    int title_col = box_col + (box_w - 6) / 2;
    tui_move(box_row + 1, title_col);
    tui_puts(title);
    tui_reset();

    /* Score and distance line, centered. */
    char score_line[40];
    snprintf(score_line, sizeof(score_line),
             "Score: %d   Distance: %d", g_score, g_distance);
    int slen_score = (int)strlen(score_line);
    tui_set_bg(0);
    tui_set_fg(11);
    tui_move(box_row + 3, box_col + (box_w - slen_score) / 2);
    tui_puts(score_line);
    tui_reset();

    /* CONTINUE + EXIT buttons, side by side. */
    TuiButton cont_btn = {
        .row = box_row + 5, .col = box_col + 4, .h = 3, .w = 14,
        .label = "CONTINUE",
        .fg = TUI_BRIGHT_WHITE, .bg = 28,
        .pressed_fg = TUI_BRIGHT_WHITE, .pressed_bg = 22,
    };
    TuiButton exit_btn = {
        .row = box_row + 5, .col = box_col + 22, .h = 3, .w = 14,
        .label = "EXIT",
        .fg = TUI_BRIGHT_WHITE, .bg = 88,
        .pressed_fg = TUI_BRIGHT_WHITE, .pressed_bg = 52,
    };
    tui_button_draw(&cont_btn);
    tui_button_draw(&exit_btn);

    tui_present_diff();

    /* Drain accidental input from the crash frame. */
    {
        unsigned drain_ms = 250;
        unsigned start_ms = sys0(SYS_TICKS_NOW);
        while ((int)(sys0(SYS_TICKS_NOW) - start_ms) < (int)drain_ms) {
            TuiEvent junk;
            while (tui_poll_event(&junk)) { }
            sys1(SYS_SLEEP_TICKS, 20);
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
                TuiButtonResult rc = tui_button_handle(&cont_btn, &ev);
                if (rc == TUI_BTN_CLICKED) return 1;
                if (rc == TUI_BTN_REDRAW) tui_button_draw(&cont_btn);
                TuiButtonResult re = tui_button_handle(&exit_btn, &ev);
                if (re == TUI_BTN_CLICKED) return 0;
                if (re == TUI_BTN_REDRAW) tui_button_draw(&exit_btn);
            }
        }
        tui_present_diff();
        sys1(SYS_SLEEP_TICKS, 20);
    }
}

/* ============================================================
 *  Main loop
 * ============================================================ */

/* Reset all per-round state so the player can replay without
 * relaunching. Called once before each play (after title, after
 * game-over-CONTINUE). Tiles are NOT recreated — they live for
 * the whole process. */
static void reset_round_state(void) {
    g_car_col = COLS / 2;
    g_car_row = CAR_BOTTOM;
    g_score = 0;
    g_distance = 0;
    g_speed = 1;
    g_dead = false;
    g_user_quit = false;
    g_bend_target_center = COLS / 2;
    g_bend_change_in = 25;
    world_init();
}

int main(void) {
    /* car steers by bare mouse movement, so it needs any-motion
     * tracking (1003), not button-only (1002). TUI_USE_MOUSE_MOTION
     * selects that. Without it, moving the mouse without holding a
     * button reported nothing — the car wouldn't follow, and under
     * PuTTY stray reports leaked onto the screen as text. */
    if (!tui_init(TUI_USE_ALT_SCREEN | TUI_HIDE_CURSOR |
                  TUI_USE_MOUSE | TUI_USE_MOUSE_MOTION |
                  TUI_USE_RAW | TUI_USE_SYNC_OUTPUT,
                  ROWS, COLS)) {
        return 1;
    }

    /* Build all sprite tiles once. They persist across rounds. */
    make_car_tile();
    make_enemy_tile();
    make_debris_tile();
    make_oil_tile();
    make_pickup_tile();
    make_tree_tile();
    make_bush_tile();
    make_sign_tiles();

    /* Outer loop: title → reset → countdown → play → over → repeat.
     * The user explicitly exits via QUIT on title or EXIT on the
     * game-over menu. */
    while (1) {
        if (!show_title_screen()) break;  /* user picked QUIT */

        reset_round_state();
        /* Draw the world once before the countdown so the user can
         * see the starting position. */
        draw_hud();
        draw_world();
        draw_car();
        tui_present();

        show_countdown();

        /* Frame loop. Target ~15 FPS for smooth scrolling. */
        while (!g_dead) {
            process_input();
            if (g_dead) break;
            scroll_world(g_speed);
            check_collisions();
            if (g_dead) break;
            /* Pickup credit accrues; distance gives passive score. */
            g_score += g_speed;
            draw_hud();
            draw_world();
            draw_car();
            tui_present_diff();
            sys1(SYS_SLEEP_TICKS, 66);  /* ~15 FPS */
        }

        /* Hold the crash frame briefly so the player sees what
         * happened, then show the menu. Skip the menu entirely
         * if the user pressed q/Esc — they wanted out, not a
         * dialog asking if they want out again. */
        if (g_user_quit) continue;

        sys1(SYS_SLEEP_TICKS, 400);

        if (!show_game_over_menu()) break;   /* user picked EXIT */
        /* CONTINUE: loop back to title... actually skip title and
         * go straight to a new round, matching typical arcade
         * "play again" UX. */
    }

    tui_shutdown();
    sys1(SYS_EXIT, 0);
    return 0;
}
