/* ============================================================
 *  musicplayer.c — audio + FFT-equalizer demo (shell app)
 *
 *  Run from the shell:  run /host/musicplayer.elf
 *
 *  Demonstrates the full audio path end-to-end through the VM:
 *    - long streaming PCM: loads /host/music.wav (host-parsed, no
 *      guest-memory limit) and loops it; falls back to a synthesized
 *      chord if no wav is present, so the demo always runs.
 *    - SFX mixing: keys [1][2][3] and left-click trigger short blips
 *      that mix over the music (click pans by column).
 *    - real-time FFT equalizer: 16 bands (0..255) from the mixed
 *      output, drawn as colored block-glyph bars in the TUI.
 *    - mute: [M] toggles the music voice gain. (A true priority-duck
 *      "emergency mute" for critical messages is a backend feature
 *      still to come; this is the simple gain toggle.)
 *    - extended-character TUI with mouse (works in PuTTY).
 *
 *  If the host build has no audio service, the UI still runs and says
 *  so (the meter stays flat).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "lib/tui.h"
#include "lib/audio.h"
#include "lib/vm_runtime.h"
#include <stdint.h>

#define NB           16      /* FFT bands the meter shows */
/* Synth buffers kept small: the guest's 64 KB data region is mostly
 * spent on the TUI canvas (~37 KB) + stack, and the real music comes
 * from the unbounded host-side /host/music.wav path. */
#define SFX_LEN    1024      /* samples per synthesized blip (~23 ms) */
#define CHORD_LEN  1800      /* fallback music loop; 3*600, the chord
                              * periods' LCM, so it loops seamlessly */

static int16_t g_sfx[3][SFX_LEN];
static int16_t g_chord[CHORD_LEN];

/* Triangle oscillator: one sample at `phase`, given `period` samples
 * per cycle and peak amplitude `amp`. Integer-only (no libm). */
static int16_t tri(uint32_t phase, uint32_t period, int amp) {
    uint32_t h = period / 2u;
    if (h == 0) return 0;
    uint32_t x = phase % period;
    int v = (x < h) ? (int)x : (int)(period - x);   /* 0..h ramp */
    return (int16_t)(((v * 2 - (int)h) * amp) / (int)h);
}

/* A short decaying triangle blip at the given period (pitch). */
static void gen_blip(int16_t *buf, int n, uint32_t period) {
    for (int i = 0; i < n; i++) {
        int amp = 9000 * (n - i) / n;               /* linear decay */
        buf[i] = tri((uint32_t)i, period, amp);
    }
}

/* A sustained major chord (root + third + fifth) — fills the meter
 * with stable harmonic content so the equalizer has something to show
 * even without a wav. Periods chosen so CHORD_LEN is a whole number of
 * cycles for a seamless loop (LCM(150,120,100)=600; 3600 = 6*600). */
static void gen_chord(int16_t *buf, int n) {
    for (int i = 0; i < n; i++) {
        int s = tri((uint32_t)i, 150, 5000)
              + tri((uint32_t)i, 120, 4000)
              + tri((uint32_t)i, 100, 3500);
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        buf[i] = (int16_t)s;
    }
}

/* Draw a 16-band equalizer: each band a vertical bar of block glyphs,
 * green (low) -> yellow (mid) -> red (top). */
static void draw_eq(int top, int left, int height, const uint8_t *bands) {
    const int barw = 3, gap = 1;
    for (int b = 0; b < NB; b++) {
        int bar_h = (int)bands[b] * height / 255;
        int x = left + b * (barw + gap);
        for (int r = 0; r < height; r++) {
            int row = top + height - 1 - r;          /* fill bottom-up */
            char  ch = ' ';
            TuiColor fg = TUI_DEFAULT_COLOR;
            if (r < bar_h) {
                ch = (char)TUI_BLOCK_FULL;
                fg = (r * 3 >= height * 2) ? TUI_BRIGHT_RED
                   : (r * 3 >= height)     ? TUI_BRIGHT_YELLOW
                                           : TUI_BRIGHT_GREEN;
            }
            for (int c = 0; c < barw; c++)
                tui_set_cell(row, x + c, ch, fg, TUI_DEFAULT_COLOR, 0);
        }
    }
}

int main(void) {
    if (!tui_init(TUI_USE_ALT_SCREEN | TUI_USE_RAW | TUI_HIDE_CURSOR |
                  TUI_USE_MOUSE | TUI_USE_SYNC_OUTPUT, 0, 0))
        return 1;
    int rows = tui_rows();
    int cols = tui_cols();

    int have_audio = audio_available();
    audio_object music = AUDIO_OBJECT_NONE;
    audio_voice  mv    = AUDIO_VOICE_NONE;
    audio_object sfx[3] = { AUDIO_OBJECT_NONE, AUDIO_OBJECT_NONE, AUDIO_OBJECT_NONE };
    int from_wav = 0;

    if (have_audio) {
        /* Prefer a real (host-streamed) wav; else a synthesized chord. */
        audio_object samp = audio_load_wav("/host/music.wav");
        from_wav = (samp != AUDIO_OBJECT_NONE);
        if (samp == AUDIO_OBJECT_NONE) {
            gen_chord(g_chord, CHORD_LEN);
            samp = audio_load_sample(g_chord, sizeof g_chord);
        }
        if (samp != AUDIO_OBJECT_NONE) {
            music = audio_load_music(samp, samp);    /* loop the sample */
            audio_free(samp);
            if (music != AUDIO_OBJECT_NONE)
                mv = audio_play_music(music, 0);
        }
        gen_blip(g_sfx[0], SFX_LEN, 90);             /* low blip  */
        gen_blip(g_sfx[1], SFX_LEN, 64);             /* mid blip  */
        gen_blip(g_sfx[2], SFX_LEN, 45);             /* high blip */
        for (int i = 0; i < 3; i++)
            sfx[i] = audio_load_sample(g_sfx[i], sizeof g_sfx[i]);
        audio_fft_enable(1);
    }

    uint32_t hz = _vm_sys0(SYS_TICK_HZ);
    if (hz == 0 || hz > 100000u) hz = 1000;
    uint32_t frame_ticks = hz / 30u;
    if (frame_ticks == 0) frame_ticks = 1;

    int muted = 0, running = 1;
    while (running) {
        TuiEvent ev;
        while (tui_poll_event(&ev)) {
            if (ev.kind == TUI_EV_KEY) {
                int k = ev.key.key;
                if (k == 'q' || k == 'Q' || k == TUI_KEY_ESCAPE) {
                    running = 0;
                } else if (have_audio && k == '1') {
                    audio_trigger(sfx[0], AUDIO_GAIN_UNITY, AUDIO_PAN_CENTER);
                } else if (have_audio && k == '2') {
                    audio_trigger(sfx[1], AUDIO_GAIN_UNITY, -16000);
                } else if (have_audio && k == '3') {
                    audio_trigger(sfx[2], AUDIO_GAIN_UNITY,  16000);
                } else if (have_audio && (k == 'm' || k == 'M') && mv) {
                    muted = !muted;
                    audio_set_gain(mv, muted ? 0 : AUDIO_GAIN_UNITY);
                }
            } else if (ev.kind == TUI_EV_MOUSE && have_audio) {
                if (ev.mouse.press && !ev.mouse.drag &&
                    ev.mouse.button == TUI_MB_LEFT && cols > 1) {
                    int pan = (ev.mouse.col * 2 * 32767) / cols - 32767;
                    audio_trigger(sfx[ev.mouse.col % 3], AUDIO_GAIN_UNITY, pan);
                }
            }
        }

        /* ---- render frame ---- */
        tui_reset();
        tui_clear();
        tui_set_fg(TUI_BRIGHT_CYAN);
        tui_move(1, 2);
        tui_puts("microgarbage  -  music + FFT equalizer");
        tui_set_fg(TUI_WHITE);
        tui_move(2, 2);
        tui_puts("[1][2][3] / click = SFX    [M]ute    [Q]uit");
        tui_move(3, 2);
        tui_set_fg(TUI_BRIGHT_BLACK);
        tui_puts(have_audio
                 ? (from_wav ? "source: /host/music.wav (looping)"
                             : "source: synthesized chord (drop /host/music.wav for a song)")
                 : "audio service not available on this host build");
        if (muted) {
            tui_set_fg(TUI_BRIGHT_RED);
            tui_move(2, 50);
            tui_puts("[MUTED]");
        }

        uint8_t bands[NB] = { 0 };
        if (have_audio) audio_get_levels(bands, NB);
        int eqtop = 5;
        int eqh = (rows > eqtop + 3) ? rows - eqtop - 2 : 10;
        draw_eq(eqtop, 4, eqh, bands);

        tui_present_diff();
        (void)_vm_sys1(SYS_SLEEP_TICKS, frame_ticks);
    }

    if (have_audio) {
        audio_fft_enable(0);
        if (mv) audio_stop(mv);
        if (music != AUDIO_OBJECT_NONE) audio_free(music);
        for (int i = 0; i < 3; i++)
            if (sfx[i] != AUDIO_OBJECT_NONE) audio_free(sfx[i]);
    }
    tui_shutdown();
    return 0;
}
