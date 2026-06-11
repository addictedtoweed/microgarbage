/* ============================================================
 *  demo_audio_mixer.c — host-file streaming music + SFX mixing,
 *                       with a sprite-based FFT meter and an on-
 *                       screen control legend.
 *
 *  Loads four WAV files dropped into the bsnes-plus working dir's
 *  ./host/ folder:
 *      /host/music.wav   — looping background track
 *      /host/sfx1.wav    — triggered by B
 *      /host/sfx2.wav    — triggered by Y
 *      /host/sfx3.wav    — triggered by A
 *
 *  On screen:
 *      - BG1 (Mode 1) with a small 5×7 white-on-navy ASCII font
 *        rendering the control legend in the top half.
 *      - 16 sprites along the bottom forming an FFT bar meter — each
 *        sprite's Y position drops as its band's level rises, drawn
 *        from the audio service's `audio_get_levels` ecall.
 *
 *  Controls:
 *      B            trigger sfx1
 *      Y            trigger sfx2
 *      A            trigger sfx3
 *      SELECT       toggle music on / off
 *      START        exit
 *
 *  Exercises:
 *      /host/ host-folder filesystem bridge,
 *      mg_stream_play looping voice,
 *      mg_sfx_load + mg_sfx_play SFX mixing on top of a stream,
 *      audio_fft_enable + audio_get_levels for the meter,
 *      8×8 BG tile font + per-cell tilemap writes,
 *      8×8 OBJ sprites for the FFT bars.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */
#include "vm_runtime.h"
#include "mg_game.h"
#include "mg_audio.h"
#include "audio.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

#define GAIN_UNITY    32767
#define MUSIC_GAIN    (GAIN_UNITY * 6 / 10)

#define FFT_BANDS     16
#define FFT_X_LEFT     64       /* leftmost FFT sprite x (pixels)   */
#define FFT_X_STEP      8       /* one bar per 8 px                 */
#define FFT_Y_BASE    200       /* baseline (low level = sprite here)*/
#define FFT_Y_RANGE    96       /* peak rise above baseline         */

/* ---- 1bpp font glyphs (5×7 in an 8×8 cell, rows 0..6 painted,
 *      row 7 padding). One byte per row, top bit = leftmost pixel.
 *      Index 0 reserved for the empty tile (= space). */
typedef struct { uint8_t row[8]; } Glyph;

static const Glyph FONT_BLANK = {{0,0,0,0,0,0,0,0}};
static const Glyph FONT_A     = {{0x70,0x88,0x88,0xF8,0x88,0x88,0x88,0}};
static const Glyph FONT_B     = {{0xF0,0x88,0x88,0xF0,0x88,0x88,0xF0,0}};
static const Glyph FONT_C     = {{0x70,0x88,0x80,0x80,0x80,0x88,0x70,0}};
static const Glyph FONT_D     = {{0xF0,0x88,0x88,0x88,0x88,0x88,0xF0,0}};
static const Glyph FONT_E     = {{0xF8,0x80,0x80,0xF0,0x80,0x80,0xF8,0}};
static const Glyph FONT_F     = {{0xF8,0x80,0x80,0xF0,0x80,0x80,0x80,0}};
static const Glyph FONT_G     = {{0x70,0x88,0x80,0xB8,0x88,0x88,0x70,0}};
static const Glyph FONT_I     = {{0x70,0x20,0x20,0x20,0x20,0x20,0x70,0}};
static const Glyph FONT_L     = {{0x80,0x80,0x80,0x80,0x80,0x80,0xF8,0}};
static const Glyph FONT_M     = {{0x88,0xD8,0xA8,0xA8,0x88,0x88,0x88,0}};
static const Glyph FONT_O     = {{0x70,0x88,0x88,0x88,0x88,0x88,0x70,0}};
static const Glyph FONT_R     = {{0xF0,0x88,0x88,0xF0,0xA0,0x90,0x88,0}};
static const Glyph FONT_S     = {{0x70,0x88,0x80,0x70,0x08,0x88,0x70,0}};
static const Glyph FONT_T     = {{0xF8,0x20,0x20,0x20,0x20,0x20,0x20,0}};
static const Glyph FONT_U     = {{0x88,0x88,0x88,0x88,0x88,0x88,0x70,0}};
static const Glyph FONT_X     = {{0x88,0x88,0x50,0x20,0x50,0x88,0x88,0}};
static const Glyph FONT_Y     = {{0x88,0x88,0x50,0x20,0x20,0x20,0x20,0}};
static const Glyph FONT_1     = {{0x20,0x60,0x20,0x20,0x20,0x20,0x70,0}};
static const Glyph FONT_2     = {{0x70,0x88,0x08,0x10,0x20,0x40,0xF8,0}};
static const Glyph FONT_3     = {{0x70,0x88,0x08,0x30,0x08,0x88,0x70,0}};
static const Glyph FONT_0     = {{0x70,0x88,0x88,0x88,0x88,0x88,0x70,0}};
static const Glyph FONT_EQ    = {{0x00,0x00,0xF8,0x00,0xF8,0x00,0x00,0}};

/* Lookup: ASCII char -> tile index in our font CHR. 0 = blank. */
#define T_SPACE  0
#define T_A      1
#define T_B      2
#define T_C      3
#define T_D      4
#define T_E      5
#define T_F      6
#define T_G      7
#define T_I      8
#define T_L      9
#define T_M     10
#define T_O     11
#define T_R     12
#define T_S     13
#define T_T     14
#define T_U     15
#define T_X     16
#define T_Y     17
#define T_1     18
#define T_2     19
#define T_3     20
#define T_0     21
#define T_EQ    22
#define T_BLOCK 23      /* solid filled — used for the FFT bars     */
#define FONT_TILE_COUNT 24

/* CHR for the solid block (FFT sprite). All pixels = palette color 1. */
static const Glyph FONT_BLOCK = {{0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF}};

/* Convert one 1bpp glyph into a 4bpp 8×8 SNES tile (32 bytes).
 * Pixels with the bit set become palette color 1; everything else is
 * color 0 (transparent for sprites, backdrop for BG). 4bpp tile
 * layout matches demo_palette.c's SOLID_CHR comment. */
static void glyph_to_4bpp(const Glyph *g, uint8_t *out) {
    for (int row = 0; row < 8; row++) {
        out[row * 2 + 0] = g->row[row];   /* plane 0 = pixel mask     */
        out[row * 2 + 1] = 0;             /* plane 1 = 0              */
    }
    for (int i = 16; i < 32; i++) out[i] = 0;   /* planes 2..3 = 0    */
}

/* Build the whole CHR area: a tile-per-glyph for the font + block.
 * GLYPHS lives at file scope as `static const` so the riscv64-elf gcc
 * doesn't materialize a stack-local copy via memcpy at _start (the
 * guest runtime doesn't provide libc memcpy). */
static uint8_t s_chr[FONT_TILE_COUNT * 32];

static const Glyph *const GLYPHS[FONT_TILE_COUNT] = {
    &FONT_BLANK, &FONT_A, &FONT_B, &FONT_C, &FONT_D, &FONT_E, &FONT_F,
    &FONT_G, &FONT_I, &FONT_L, &FONT_M, &FONT_O, &FONT_R, &FONT_S,
    &FONT_T, &FONT_U, &FONT_X, &FONT_Y, &FONT_1, &FONT_2, &FONT_3,
    &FONT_0, &FONT_EQ, &FONT_BLOCK,
};

static void build_chr(void) {
    for (int i = 0; i < FONT_TILE_COUNT; i++) {
        glyph_to_4bpp(GLYPHS[i], s_chr + i * 32);
    }
}

/* Compact way to spell a label as a tile-index sequence. _ = space. */
#define MAX_LABEL_LEN 24
static const uint8_t LABEL_TITLE[] = {
    T_A,T_U,T_D,T_I,T_O,T_SPACE,T_M,T_I,T_X,T_E,T_R
};
static const uint8_t LABEL_B[] = { T_B, T_EQ, T_S,T_F,T_X,T_1 };
static const uint8_t LABEL_Y[] = { T_Y, T_EQ, T_S,T_F,T_X,T_2 };
static const uint8_t LABEL_A[] = { T_A, T_EQ, T_S,T_F,T_X,T_3 };
static const uint8_t LABEL_SEL[] = { T_S,T_E,T_L, T_EQ, T_M,T_U,T_S,T_I,T_C };
static const uint8_t LABEL_STR[] = { T_S,T_T,T_R, T_EQ, T_E,T_X,T_I,T_T };
static const uint8_t LABEL_FFT[] = { T_F,T_F,T_T };

/* Write a label into BG1 starting at (x, y) tilemap coordinates,
 * using palette 0 of BG1. Build the MgBgTile via its `.word` member
 * (palette/priority/flip all zero) so gcc doesn't emit a memcpy for
 * a designated-initializer struct literal. */
static void put_label(uint8_t x, uint8_t y,
                      const uint8_t *tiles, uint16_t n) {
    for (uint16_t i = 0; i < n; i++) {
        MgBgTile c;
        c.word = (uint16_t)tiles[i];   /* palette/priority/flip = 0 */
        mg_bg_set_tile(MG_BG_LAYER_1, (uint8_t)(x + i), y, c);
    }
}

#define ARRSZ(a) ((uint16_t)(sizeof(a) / sizeof((a)[0])))

void _start(void) {
    build_chr();

    /* Every other tested demo (sprite, mode7, mode7_3d) calls this
     * before setting up its BG state. Without it, leftover VRAM
     * contents from the shell or a previous demo can corrupt our
     * tilemap area (VRAM word $0400..) and our CHR area ($2000..),
     * leaving the BG layer reading garbage tile indices. */
    mg_ppu_clean_slate();

    /* Mode 1: BG1 4bpp tiles (font + FFT block), tilemap at word
     * $0400, CHR base at word $2000. Sprite CHR base at word $4000. */
    mg_bg_mode(MG_BG_MODE_1);
    mg_bg_setup(MG_BG_LAYER_1, 0x0400, MG_BG_SIZE_32x32, 0x2000);
    mg_bg_enable(MG_BG_LAYER_1, /*main=*/true, /*sub=*/false);
    mg_sprite_sizes(MG_SPR_SIZES_8_16);     /* small = 8x8 (our FFT) */
    mg_sprite_chr_base(0x4000, 0x4000);

    /* Upload CHR for both the font tiles (BG1 reads from $2000) and
     * one identical copy at sprite CHR base ($4000) so the solid
     * block at FONT_TILE_COUNT-1 doubles as the FFT sprite tile. */
    MG_OR_PANIC(mg_chr_upload(0x2000, s_chr, sizeof s_chr));
    MG_OR_PANIC(mg_chr_upload(0x4000, s_chr + T_BLOCK * 32, 32));

    /* DIAGNOSTIC v1.65: bright red backdrop, bright yellow text.
     * If the screen shows red, palette + INIDISP are working and the
     * issue is BG tilemap/CHR. If still black, something is killing
     * the entire PPU output (force-blank? wrong mode? clean_slate
     * leftover state?). */
    mg_palette_set_rgb(0,  255,   0,   0);  /* RED backdrop          */
    mg_palette_set_rgb(1,  255, 255,   0);  /* YELLOW font           */
    mg_palette_set_rgb(129, 64, 224, 232);  /* bright cyan FFT bars  */

    /* Render the static text. Tile coordinates 0..31 x 0..27. */
    put_label( 4,  2, LABEL_TITLE, ARRSZ(LABEL_TITLE));
    put_label( 4,  5, LABEL_B,     ARRSZ(LABEL_B));
    put_label( 4,  7, LABEL_Y,     ARRSZ(LABEL_Y));
    put_label( 4,  9, LABEL_A,     ARRSZ(LABEL_A));
    put_label( 4, 12, LABEL_SEL,   ARRSZ(LABEL_SEL));
    put_label( 4, 14, LABEL_STR,   ARRSZ(LABEL_STR));
    put_label( 4, 17, LABEL_FFT,   ARRSZ(LABEL_FFT));

    /* Pre-place the 16 FFT sprites at their fixed X columns and a
     * dormant Y so they're hidden until the first FFT update. */
    /* Field-by-field assignment (rather than a designated initializer)
     * to keep gcc from emitting a memcpy from rodata for the struct
     * literal -- the riscv64-elf guest doesn't link libc. */
    for (uint8_t i = 0; i < FFT_BANDS; i++) {
        MgSprite sp;
        sp.x          = (int16_t)(FFT_X_LEFT + i * FFT_X_STEP);
        sp.y          = 240;                  /* hidden until levels arrive */
        sp.tile       = 0;                    /* tile 0 at sprite CHR base  */
        sp.palette    = 0;
        sp.priority   = 0;
        sp.hflip      = 0;
        sp.vflip      = 0;
        sp.size_large = 0;
        mg_sprite_set(i, &sp);
    }

    /* Audio init. Re-enabled after the BG1SC encoding fix (v1.78)
     * gave us visible text; the worker thread (v1.68/v1.69) runs the
     * mixer + service drain on its own thread now, so audio ecalls
     * no longer stall the bsnes-plus thread.
     *
     * Files come from the /host/ bridge — the bsnes-plus mapper
     * mounts ./host/ next to bsnes.exe. Missing files return 0
     * (MG_VOICE_REJECTED / null MgSfx) and the demo just plays
     * silence for that slot — the on-screen indicator next to each
     * label still toggles to give visible feedback. */
    MgSfx   sfx[3] = {
        mg_sfx_load("/host/sfx1.wav"),
        mg_sfx_load("/host/sfx2.wav"),
        mg_sfx_load("/host/sfx3.wav"),
    };
    MgVoice music = MG_VOICE_REJECTED;
    uint8_t music_on = 0;

    {
        uint8_t zero = T_0;
        put_label(11,  5, &zero, 1);
        put_label(11,  7, &zero, 1);
        put_label(11,  9, &zero, 1);
        put_label(14, 12, &zero, 1);
    }

    /* Turn on the FFT analyzer in the audio service; audio_get_levels
     * each frame returns the per-band magnitudes for the sprite bars. */
    audio_fft_enable(1);

    /* Backdrop flash on each trigger so visual feedback survives even
     * when the SFX itself doesn't play (e.g. file missing, audio not
     * installed). Lets you tell "I pressed B but no audio" apart from
     * "I pressed B and nothing registered the press at all". */
    uint8_t  flash_r = 0, flash_g = 0, flash_b = 0;
    uint8_t  flash_remaining = 0;

    uint8_t levels[FFT_BANDS] = {0};

    for (;;) {
        MgPads pads = mg_pads();
        if (mg_pad_pressed(pads.p0, MG_BTN_START)) {
            /* Tidy up so background music doesn't outlive the demo. */
            if (music != MG_VOICE_REJECTED) mg_audio_stop(music);
            audio_fft_enable(0);
            sys_exit(0);
        }

        /* B/Y/A: one-shot SFX trigger + visible backdrop flash so the
         * user can tell "press registered, sample missing" apart from
         * "press never registered." Gain Q15 = $7000 (~0.875) keeps
         * headroom under the music; pan 0 = center. */
        if (mg_pad_pressed(pads.p0, MG_BTN_B)) {
            if (sfx[0]) (void)mg_sfx_play(sfx[0], 0x7000, 0);
            { uint8_t blk = T_BLOCK; put_label(11, 5, &blk, 1); }
            flash_r = 220; flash_g = 32;  flash_b = 32;
            flash_remaining = 8;
        }
        if (mg_pad_pressed(pads.p0, MG_BTN_Y)) {
            if (sfx[1]) (void)mg_sfx_play(sfx[1], 0x7000, 0);
            { uint8_t blk = T_BLOCK; put_label(11, 7, &blk, 1); }
            flash_r = 32;  flash_g = 220; flash_b = 32;
            flash_remaining = 8;
        }
        if (mg_pad_pressed(pads.p0, MG_BTN_A)) {
            if (sfx[2]) (void)mg_sfx_play(sfx[2], 0x7000, 0);
            { uint8_t blk = T_BLOCK; put_label(11, 9, &blk, 1); }
            flash_r = 32;  flash_g = 64;  flash_b = 220;
            flash_remaining = 8;
        }

        /* SELECT: toggle the looping music stream on/off. The 0/BLOCK
         * indicator at (14, 12) follows the state. */
        if (mg_pad_pressed(pads.p0, MG_BTN_SELECT)) {
            if (music_on) {
                mg_audio_stop(music);
                music = MG_VOICE_REJECTED;
                music_on = 0;
                { uint8_t z = T_0; put_label(14, 12, &z, 1); }
            } else {
                music = mg_stream_play("/host/music.wav", MG_AUDIO_LOOP);
                if (music != MG_VOICE_REJECTED) {
                    music_on = 1;
                    { uint8_t blk = T_BLOCK; put_label(14, 12, &blk, 1); }
                }
            }
        }

        /* Drive backdrop CGRAM[0]. Default is RED; flashes when a
         * trigger lands so the press is unmistakable even without
         * audio. */
        if (flash_remaining > 0) {
            mg_palette_set_rgb(0, flash_r, flash_g, flash_b);
            flash_remaining--;
        } else {
            mg_palette_set_rgb(0, 255, 0, 0);
        }

        /* FFT sprite bars. audio_get_levels returns up to N bands of
         * magnitude (0..255). Map each to a Y position in
         * [FFT_Y_BASE - FFT_Y_RANGE .. FFT_Y_BASE]; higher level →
         * sprite higher on screen → smaller Y. If audio_get_levels
         * fails (no service) the levels[] array stays at zeros from
         * last frame and the bars sit at FFT_Y_BASE. */
        (void)audio_get_levels(levels, FFT_BANDS);
        for (uint8_t i = 0; i < FFT_BANDS; i++) {
            int16_t x  = (int16_t)(FFT_X_LEFT + i * FFT_X_STEP);
            uint8_t y  = (uint8_t)(FFT_Y_BASE
                                   - ((uint16_t)levels[i] * FFT_Y_RANGE) / 255u);
            mg_sprite_move(i, x, y);
        }

        mg_frame_commit();
        mg_wait_frame();
    }
}
