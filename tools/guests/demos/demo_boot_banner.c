/* ============================================================
 *  demo_boot_banner.c — first-boot "how to connect" splash.
 *
 *  Renders a static, all-caps instruction screen on BG1 telling the
 *  developer how to reach the PuTTY dev shell, commits a few frames so
 *  the kernel uploads it, then exits. The VM-unload hook does NOT clear
 *  VRAM (see src/mgapi/vm_init.c mg_state_reset_on_unload — "demos that
 *  don't call clean_slate inherit the previous demo's VRAM"), so the
 *  splash stays on screen after this guest exits, until a demo launched
 *  from the shell overwrites it.
 *
 *  Autostarted at boot via /td0/etc/autostart (seeded by vm_init), so it
 *  is the first thing on screen when the boot ROM loads. Because it
 *  exits immediately it does NOT block the shell's autostart — the PuTTY
 *  prompt is available the moment a client connects.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_game.h"
#include "mg_input.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* Spawn an ELF child and block until it exits (returns its exit code). The
 * port-2 mouse left-click uses this to launch the FMV player — a film-critic
 * "kiosk" needs no PuTTY. When the child exits (its own START), we return here
 * and redraw the splash. */
static inline int sys_spawn_and_wait(const char *path) {
    register int a0 asm("a0") = (int)(unsigned long)path;
    register int a7 asm("a7") = SYS_SPAWN_AND_WAIT;
    asm volatile ("ecall" : "+r"(a0) : "r"(a7) : "memory");
    return a0;
}

/* ---- 5x7-in-8x8 1bpp font. Top bit = leftmost pixel; rows 0..6 used,
 *      row 7 padding. Tile 0 = blank (space). Indices below. ---- */
typedef struct { uint8_t row[8]; } Glyph;

/* Tile-index assignment: 0=space, 1..26 = A..Z, 27..36 = 0..9,
 * 37='(' 38=')' 39='.' 40='/' 41=':'. */
#define T_SP   0
#define T_LPAR 37
#define T_RPAR 38
#define T_DOT  39
#define T_SLSH 40
#define T_COL  41
#define TILE_COUNT 42

static const Glyph FONT[TILE_COUNT] = {
    /* 0  space */ {{0,0,0,0,0,0,0,0}},
    /* 1  A */ {{0x70,0x88,0x88,0xF8,0x88,0x88,0x88,0}},
    /* 2  B */ {{0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0}},
    /* 3  C */ {{0x70,0x88,0x80,0x80,0x80,0x88,0x70,0}},
    /* 4  D */ {{0xF0,0x88,0x88,0x88,0x88,0x88,0xF0,0}},
    /* 5  E */ {{0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0}},
    /* 6  F */ {{0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0}},
    /* 7  G */ {{0x70,0x88,0x80,0xB8,0x88,0x88,0x70,0}},
    /* 8  H */ {{0x88,0x88,0x88,0xF8,0x88,0x88,0x88,0}},
    /* 9  I */ {{0x70,0x20,0x20,0x20,0x20,0x20,0x70,0}},
    /* 10 J */ {{0x38,0x10,0x10,0x10,0x90,0x90,0x60,0}},
    /* 11 K */ {{0x88,0x90,0xA0,0xC0,0xA0,0x90,0x88,0}},
    /* 12 L */ {{0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0}},
    /* 13 M */ {{0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88,0}},
    /* 14 N */ {{0x88,0xC8,0xA8,0x98,0x88,0x88,0x88,0}},
    /* 15 O */ {{0x70,0x88,0x88,0x88,0x88,0x88,0x70,0}},
    /* 16 P */ {{0xF0,0x88,0x88,0xF0,0x80,0x80,0x80,0}},
    /* 17 Q */ {{0x70,0x88,0x88,0x88,0xA8,0x90,0x68,0}},
    /* 18 R */ {{0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0}},
    /* 19 S */ {{0x70,0x88,0x80,0x70,0x08,0x88,0x70,0}},
    /* 20 T */ {{0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0}},
    /* 21 U */ {{0x88,0x88,0x88,0x88,0x88,0x88,0x70,0}},
    /* 22 V */ {{0x88,0x88,0x88,0x88,0x88,0x50,0x20,0}},
    /* 23 W */ {{0x88,0x88,0x88,0xA8,0xA8,0xD8,0x88,0}},
    /* 24 X */ {{0x88,0x88,0x50,0x20,0x50,0x88,0x88,0}},
    /* 25 Y */ {{0x88,0x88,0x50,0x20,0x20,0x20,0x20,0}},
    /* 26 Z */ {{0xF8,0x08,0x10,0x20,0x40,0x80,0xF8,0}},
    /* 27 0 */ {{0x70,0x88,0x98,0xA8,0xC8,0x88,0x70,0}},
    /* 28 1 */ {{0x20,0x60,0x20,0x20,0x20,0x20,0x70,0}},
    /* 29 2 */ {{0x70,0x88,0x08,0x10,0x20,0x40,0xF8,0}},
    /* 30 3 */ {{0x70,0x88,0x08,0x30,0x08,0x88,0x70,0}},
    /* 31 4 */ {{0x10,0x30,0x50,0x90,0xF8,0x10,0x10,0}},
    /* 32 5 */ {{0xF8,0x80,0xF0,0x08,0x08,0x88,0x70,0}},
    /* 33 6 */ {{0x30,0x40,0x80,0xF0,0x88,0x88,0x70,0}},
    /* 34 7 */ {{0xF8,0x08,0x10,0x20,0x40,0x40,0x40,0}},
    /* 35 8 */ {{0x70,0x88,0x88,0x70,0x88,0x88,0x70,0}},
    /* 36 9 */ {{0x70,0x88,0x88,0x78,0x08,0x10,0x60,0}},
    /* 37 ( */ {{0x10,0x20,0x40,0x40,0x40,0x20,0x10,0}},
    /* 38 ) */ {{0x40,0x20,0x10,0x10,0x10,0x20,0x40,0}},
    /* 39 . */ {{0x00,0x00,0x00,0x00,0x00,0x60,0x60,0}},
    /* 40 / */ {{0x08,0x08,0x10,0x20,0x40,0x80,0x80,0}},
    /* 41 : */ {{0x00,0x60,0x60,0x00,0x60,0x60,0x00,0}},
};

/* Build the 4bpp CHR area: pixel set -> color 1, else color 0. */
static uint8_t s_chr[TILE_COUNT * 32];

static void build_chr(void) {
    for (int t = 0; t < TILE_COUNT; t++) {
        uint8_t *p = s_chr + t * 32;
        for (int row = 0; row < 8; row++) {
            p[row * 2 + 0] = FONT[t].row[row];   /* plane 0 = pixel mask */
            p[row * 2 + 1] = 0;                  /* plane 1 = 0          */
        }
        for (int i = 16; i < 32; i++) p[i] = 0;  /* planes 2..3 = 0      */
    }
}

/* ASCII char -> tile index. Anything unmapped renders as a space. */
static uint8_t tile_of(char c) {
    if (c >= 'A' && c <= 'Z') return (uint8_t)(1  + (c - 'A'));
    if (c >= '0' && c <= '9') return (uint8_t)(27 + (c - '0'));
    switch (c) {
        case '(': return T_LPAR;
        case ')': return T_RPAR;
        case '.': return T_DOT;
        case '/': return T_SLSH;
        case ':': return T_COL;
        default:  return T_SP;
    }
}

/* Write a NUL-terminated string into BG1 starting at tile (x, y). */
static void put_text(uint8_t x, uint8_t y, const char *s) {
    for (uint8_t i = 0; s[i]; i++) {
        MgBgTile c;
        c.word = (uint16_t)tile_of(s[i]);   /* palette/priority/flip = 0 */
        mg_bg_set_tile(MG_BG_LAYER_1, (uint8_t)(x + i), y, c);
    }
}

/* Draw (or redraw) the whole splash into VRAM. Called at start and again after
 * the FMV player exits, since that demo overwrites the screen. */
static void render_splash(void) {
    /* Clear VRAM (so the margin tile is backdrop) then set up BG1 Mode 1:
     * tilemap at VRAM word $0400, CHR base at word $2000 — same layout the
     * other text demos use. */
    mg_ppu_clean_slate();
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, 0x0400, MG_BG_SIZE_32x32, 0x2000);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);
    MG_OR_PANIC(mg_chr_upload(0x2000, s_chr, sizeof s_chr));

    /* Palette: color 0 = black backdrop, color 1 = white text. */
    mg_palette_set_rgb(0,   0,   0,   0);
    mg_palette_set_rgb(1, 255, 255, 255);

    /* All-caps; long lines wrapped to fit the 32-tile width. */
    put_text(2,  3, "MICROGARBAGE OS");
    put_text(2,  4, "(BSNES DEV KIT CROSS COMPILE)");

    put_text(2,  8, "MOUSE ON PORT 2: LEFT CLICK");
    put_text(2,  9, "TO BECOME A FILM CRITIC");

    put_text(2, 13, "OR CONNECT THE DEV SHELL:");
    put_text(2, 15, "PUTTY 127.0.0.1:2323");
    put_text(2, 17, "IN PUTTY TERMINAL CATEGORY,");
    put_text(2, 18, "TURN OFF LOCAL ECHO AND");
    put_text(2, 19, "LOCAL LINE EDITING.");
}

void _start(void) {
    build_chr();
    render_splash();

    /* Hold the splash by re-committing every frame (the kernel only displays a
     * live guest's frames). Each frame, poll the port-2 mouse: a LEFT-CLICK
     * launches the FMV player; when it exits we redraw and resume. Dismiss to
     * the dev shell with Ctrl-C in PuTTY (the host kills this autostart spawn). */
    uint8_t prev_left = 0;
    for (;;) {
        mg_frame_commit();
        mg_wait_frame();

        MgMouse m = mg_mouse();
        uint8_t left = (m.buttons & MG_MOUSE_LEFT) ? 1u : 0u;
        if (left && !prev_left) {
            (void)sys_spawn_and_wait("/td0/demos/fmv_player.elf");
            render_splash();   /* the FMV overwrote VRAM — redraw */
            prev_left = 0;     /* swallow the launching click */
            continue;
        }
        prev_left = left;
    }
}
