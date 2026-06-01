/* ============================================================
 *  menu.c — the SELECT DEMO menu, running on the copro side.
 *
 *  Same menu the (legacy) snes/smoke.s renders, but the logic is C
 *  here instead of 65816 assembly. The SNES side runs the bare
 *  runtime kernel (snes/boot.s + snes/kernel.s), which just waits
 *  for COPRO_FRAME_RDY and walks the DMA list. We stage CGRAM /
 *  tilemap / CHR into the cart window each frame, fill DMA
 *  descriptors, commit, then read joypads to move the arrow.
 *
 *  This file is the *first* visible artifact of the copro-runtime
 *  architecture working — kernel.s drives the bus, we drive the
 *  kernel.
 *
 *  Built against examples/common/guest/{vm_runtime,mg_copro}.h.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_copro.h"

#include <stdint.h>
#include <stddef.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

/* ----------------------------------------------------------------
 *  Font: 17 4bpp tiles. Each row of `TILE_ROWS[t]` is plane-0 bits;
 *  planes 1-3 are zero (so we use only colors 0 and 1: backdrop +
 *  text). 8 rows = 8 lit pixels per tile column. See smoke.s for the
 *  same glyphs in 65816 source.
 * ---------------------------------------------------------------- */
#define T_SPC 0
#define T_A   1
#define T_C   2
#define T_D   3
#define T_E   4
#define T_F   5
#define T_I   6
#define T_L   7
#define T_M   8
#define T_O   9
#define T_R   10
#define T_S   11
#define T_T   12
#define T_V   13
#define T_X   14
#define T_3   15
#define T_ARR 16

#define TILE_COUNT 17

static const uint8_t TILE_ROWS[TILE_COUNT][8] = {
    /* 0  SPACE */ { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 },
    /* 1  A     */ { 0x3C,0x42,0x42,0x7E,0x42,0x42,0x42,0x00 },
    /* 2  C     */ { 0x3C,0x42,0x40,0x40,0x40,0x42,0x3C,0x00 },
    /* 3  D     */ { 0x7C,0x42,0x42,0x42,0x42,0x42,0x7C,0x00 },
    /* 4  E     */ { 0x7E,0x40,0x40,0x78,0x40,0x40,0x7E,0x00 },
    /* 5  F     */ { 0x7E,0x40,0x40,0x78,0x40,0x40,0x40,0x00 },
    /* 6  I     */ { 0x38,0x10,0x10,0x10,0x10,0x10,0x38,0x00 },
    /* 7  L     */ { 0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00 },
    /* 8  M     */ { 0x42,0x66,0x5A,0x5A,0x42,0x42,0x42,0x00 },
    /* 9  O     */ { 0x3C,0x42,0x42,0x42,0x42,0x42,0x3C,0x00 },
    /* 10 R     */ { 0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x00 },
    /* 11 S     */ { 0x3C,0x42,0x40,0x3C,0x02,0x42,0x3C,0x00 },
    /* 12 T     */ { 0x7E,0x10,0x10,0x10,0x10,0x10,0x10,0x00 },
    /* 13 V     */ { 0x42,0x42,0x42,0x42,0x42,0x24,0x18,0x00 },
    /* 14 X     */ { 0x42,0x42,0x24,0x18,0x24,0x42,0x42,0x00 },
    /* 15 3     */ { 0x3C,0x42,0x02,0x1C,0x02,0x42,0x3C,0x00 },
    /* 16 >     */ { 0x40,0x20,0x10,0x08,0x10,0x20,0x40,0x00 },
};

/* Menu strings as tile-ID sequences. The kernel's BG1 is 4bpp Mode 1
 * with tilemap at VRAM word $0000, CHR base at $1000 (see kernel.s). */
static const uint8_t TITLE[] = { T_S,T_E,T_L,T_E,T_C,T_T,T_SPC,T_D,T_E,T_M,T_O };
static const uint8_t OPT0[]  = { T_F,T_M,T_V,T_SPC,T_D,T_E,T_M,T_O };
static const uint8_t OPT1[]  = { T_3,T_D,T_SPC,T_D,T_E,T_M,T_O };
static const uint8_t OPT2[]  = { T_M,T_I,T_X,T_E,T_R,T_SPC,T_D,T_E,T_M,T_O };

/* On-screen layout — 32-tile-wide field, menu centered. */
#define ROW_TITLE 8
#define COL_TITLE 10
#define ROW_OPT0  12
#define ROW_OPT1  14
#define ROW_OPT2  16
#define COL_ARROW 9
#define COL_OPT   11

/* Cart-window byte offsets where we stage each chunk. The runtime
 * kernel reads from these offsets when it walks the DMA list. */
#define WIN_CGRAM  0x0000u                  /* 4 bytes: 2 colors    */
#define WIN_TMAP   0x0004u                  /* 32*32*2 = 2048       */
#define WIN_CHR    (WIN_TMAP + 32u*32u*2u)  /* 17 tiles * 32 bytes  */
#define CHR_BYTES  (TILE_COUNT * 32u)

/* SNES auto-joypad bit positions (bits in the 16-bit word read at
 * $4218 — see kernel.s read_joypads). */
#define JOY_UP    0x0800u
#define JOY_DN    0x0400u

/* ----------------------------------------------------------------
 *  Helpers
 * ---------------------------------------------------------------- */

static void build_chr(uint8_t *out) {
    for (uint32_t t = 0; t < TILE_COUNT; t++) {
        uint8_t *p = out + t * 32u;
        for (int row = 0; row < 8; row++) {
            p[row * 2 + 0] = TILE_ROWS[t][row];  /* plane 0 */
            p[row * 2 + 1] = 0;                  /* plane 1 */
        }
        for (int i = 16; i < 32; i++) p[i] = 0;  /* planes 2+3 */
    }
}

static void put_string(uint8_t *tmap, int row, int col,
                       const uint8_t *s, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        uint32_t entry = ((uint32_t)row * 32u + (uint32_t)col + i) * 2u;
        tmap[entry + 0] = s[i];
        tmap[entry + 1] = 0;   /* palette/flip/priority = 0 */
    }
}

static void build_tilemap(uint8_t *tmap, int arrow_pos) {
    for (uint32_t i = 0; i < 2048u; i++) tmap[i] = 0;
    put_string(tmap, ROW_TITLE, COL_TITLE, TITLE, (uint32_t)sizeof TITLE);
    put_string(tmap, ROW_OPT0,  COL_OPT,   OPT0,  (uint32_t)sizeof OPT0);
    put_string(tmap, ROW_OPT1,  COL_OPT,   OPT1,  (uint32_t)sizeof OPT1);
    put_string(tmap, ROW_OPT2,  COL_OPT,   OPT2,  (uint32_t)sizeof OPT2);

    int arrow_row = ROW_OPT0 + arrow_pos * 2;
    uint32_t arrow_entry = ((uint32_t)arrow_row * 32u +
                            (uint32_t)COL_ARROW) * 2u;
    tmap[arrow_entry + 0] = T_ARR;
    tmap[arrow_entry + 1] = 0;
}

/* ----------------------------------------------------------------
 *  Main loop. Runs forever — the shell autostart spawns this; bsnes
 *  reads cart bytes through mgapi.dll; the SNES kernel walks the
 *  DMA list each NMI and uploads what we staged.
 * ---------------------------------------------------------------- */

void _start(void) {
    /* CGRAM: color 0 = black (backdrop), color 1 = white (text). */
    static const uint8_t cgram[4] = { 0x00, 0x00, 0xFF, 0x7F };

    static uint8_t chr [CHR_BYTES];
    static uint8_t tmap[2048];

    build_chr(chr);

    int arrow_pos = 0;
    uint16_t last_pads = 0;
    uint32_t last_reset = mg_copro_reset_count();

    for (;;) {
        /* On reset: reset menu state so the user lands back at the
         * top option. The kernel is rebooting in parallel; our next
         * frame-commit gets picked up by the post-reset kernel just
         * as if it were cold boot. */
        uint32_t now_reset = mg_copro_reset_count();
        if (now_reset != last_reset) {
            arrow_pos  = 0;
            last_pads  = 0;
            last_reset = now_reset;
        }

        build_tilemap(tmap, arrow_pos);

        /* Stage the three chunks into the cart window. */
        mg_stage_payload(WIN_CGRAM, cgram, sizeof cgram);
        mg_stage_payload(WIN_CHR,   chr,   sizeof chr);
        mg_stage_payload(WIN_TMAP,  tmap,  sizeof tmap);

        /* DMA list: CGRAM, CHR, tilemap. The kernel fires them in
         * order; CHR before tilemap so the new tiles are visible by
         * the time the tilemap entries reference them. */
        mg_stage_dma_slot(0, MG_BBUS_CGDATA,  MG_DMAP_BYTE,
                          WIN_CGRAM, (uint16_t)sizeof cgram, 0x0000);
        mg_stage_dma_slot(1, MG_BBUS_VMDATAL, MG_DMAP_WORD,
                          WIN_CHR,   (uint16_t)CHR_BYTES,    0x1000);
        mg_stage_dma_slot(2, MG_BBUS_VMDATAL, MG_DMAP_WORD,
                          WIN_TMAP,  (uint16_t)sizeof tmap,  0x0000);
        /* slots 3..7 default to bbus=0 = empty; we don't need them. */

        mg_frame_commit(1);
        mg_wait_vblank();

        /* Read pad 0 (port 1 main controller). */
        uint16_t pads[4];
        mg_read_pads(pads);
        uint16_t now    = pads[0];
        uint16_t edges  = (uint16_t)((now ^ last_pads) & now);

        if ((edges & JOY_UP) && arrow_pos > 0) arrow_pos--;
        if ((edges & JOY_DN) && arrow_pos < 2) arrow_pos++;
        last_pads = now;
    }
}
