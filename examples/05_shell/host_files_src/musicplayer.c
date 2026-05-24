/* ============================================================
 *  musicplayer.c — audio + FFT-equalizer demo (shell app)
 *
 *  Run from the shell:  run /host/musicplayer.elf
 *
 *  Demonstrates the full audio path end-to-end through the VM:
 *    - long streaming PCM: loads /host/music.wav (host-parsed, no
 *      guest-memory limit) and loops it; falls back to a synthesized
 *      chord if no wav is present, so the demo always runs. Music
 *      plays at reduced gain so triggered SFX clearly cut through.
 *    - SFX mixing: keys [1][2][3] and left-click trigger short blips
 *      that mix over the music (click pans by column).
 *    - real-time FFT equalizer: 16 bands (0..255) from the mixed
 *      output, drawn as colored block-glyph bars in the TUI.
 *    - mute: [M] toggles the music voice gain.
 *    - an input/trigger readout: shows each key as it registers, the
 *      running SFX count, and the trigger result (voice handle, or
 *      REJECTED if all tracks are busy) — so you can tell input from
 *      audibility at a glance.
 *    - extended-character TUI with mouse (works in PuTTY).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "lib/tui.h"
#include "lib/audio.h"
#include "lib/vm_runtime.h"
#include <stdint.h>

#define NB           16      /* FFT bands the meter shows */
/* Synth buffers: the TUI canvas is host-side (SYS_ALLOC), so the guest
 * data region comfortably holds these. SFX are ~54 ms and loud so they
 * are clearly audible over the music. The real song comes from the
 * unbounded host-side /host/music.wav path. */
#define SFX_LEN    2400      /* samples per synthesized blip (~54 ms) */
#define CHORD_LEN  1800      /* fallback music loop; 3*600 (chord LCM) */
#define MUSIC_GAIN (AUDIO_GAIN_UNITY * 6 / 10)   /* ~60% so SFX pop */

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

/* A punchy blip: quick attack, long decay, loud peak so it reads
 * clearly over the music. */
static void gen_blip(int16_t *buf, int n, uint32_t period) {
    int attack = n / 20; if (attack < 1) attack = 1;   /* ~5% attack */
    for (int i = 0; i < n; i++) {
        int env = (i < attack) ? (26000 * i / attack)
                               : (26000 * (n - i) / (n - attack));
        buf[i] = tri((uint32_t)i, period, env);
    }
}

/* A sustained major chord (root + third + fifth) so the equalizer has
 * stable content without a wav. CHORD_LEN is a whole number of cycles
 * of each period (LCM(150,120,100)=600) for a seamless loop. */
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

/* Print an unsigned decimal at the notional cursor (no libc). */
static void tui_putu(uint32_t v) {
    char b[12]; int i = 10; b[11] = 0;
    if (v == 0) { tui_puts("0"); return; }
    while (v && i >= 0) { b[i--] = (char)('0' + (v % 10u)); v /= 10u; }
    tui_puts(&b[i + 1]);
}

/* Draw the 16-band equalizer: each band a vertical bar of block glyphs,
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
        audio_object samp = audio_load_wav("/host/music.wav");
        from_wav = (samp != AUDIO_OBJECT_NONE);
        if (samp == AUDIO_OBJECT_NONE) {
            gen_chord(g_chord, CHORD_LEN);
            samp = audio_load_sample(g_chord, sizeof g_chord);
        }
        if (samp != AUDIO_OBJECT_NONE) {
            music = audio_load_music(samp, samp);
            audio_free(samp);
            if (music != AUDIO_OBJECT_NONE) {
                mv = audio_play_music(music, 0);
                if (mv) audio_set_gain(mv, MUSIC_GAIN);  /* leave room for SFX */
            }
        }
        gen_blip(g_sfx[0], SFX_LEN, 120);            /* low  */
        gen_blip(g_sfx[1], SFX_LEN,  80);            /* mid  */
        gen_blip(g_sfx[2], SFX_LEN,  56);            /* high */
        for (int i = 0; i < 3; i++)
            sfx[i] = audio_load_sample(g_sfx[i], sizeof g_sfx[i]);
        audio_fft_enable(1);
    }

    uint32_t hz = _vm_sys0(SYS_TICK_HZ);
    if (hz == 0 || hz > 100000u) hz = 1000;
    uint32_t frame_ticks = hz / 30u;
    if (frame_ticks == 0) frame_ticks = 1;

    /* input/trigger diagnostics */
    uint32_t trig_count = 0;
    int      last_key   = 0;     /* last key code seen (proves input) */
    int      last_voice = -1;    /* last SFX trigger result; 0=REJECTED */
    int      flash      = 0;     /* frames to highlight the readout */

    int muted = 0, running = 1;
    while (running) {
        TuiEvent ev;
        while (tui_poll_event(&ev)) {
            if (ev.kind == TUI_EV_KEY) {
                int k = ev.key.key;
                last_key = k;                /* any key updates this */
                flash = 8;
                int idx = -1, pan = AUDIO_PAN_CENTER;
                if (k == 'q' || k == 'Q' || k == TUI_KEY_ESCAPE) running = 0;
                else if (k == '1') { idx = 0; pan = AUDIO_PAN_CENTER; }
                else if (k == '2') { idx = 1; pan = -16000; }
                else if (k == '3') { idx = 2; pan =  16000; }
                else if ((k == 'm' || k == 'M') && have_audio && mv) {
                    muted = !muted;
                    audio_set_gain(mv, muted ? 0 : MUSIC_GAIN);
                }
                if (idx >= 0 && have_audio) {
                    last_voice = (int)audio_trigger(sfx[idx], AUDIO_GAIN_UNITY, pan);
                    trig_count++;
                }
            } else if (ev.kind == TUI_EV_MOUSE && have_audio) {
                if (ev.mouse.press && !ev.mouse.drag &&
                    ev.mouse.button == TUI_MB_LEFT && cols > 1) {
                    int pan = (ev.mouse.col * 2 * 32767) / cols - 32767;
                    last_voice = (int)audio_trigger(sfx[ev.mouse.col % 3],
                                                    AUDIO_GAIN_UNITY, pan);
                    trig_count++;
                    flash = 8;
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
        if (muted) { tui_set_fg(TUI_BRIGHT_RED); tui_move(2, 50); tui_puts("[MUTED]"); }

        /* source line */
        tui_move(3, 2);
        tui_set_fg(TUI_BRIGHT_BLACK);
        tui_puts(have_audio
                 ? (from_wav ? "source: /host/music.wav (looping)"
                             : "source: synthesized chord (drop /host/music.wav for a song)")
                 : "audio service not available on this host build");

        /* input/trigger readout — highlighted briefly on each event */
        tui_move(4, 2);
        tui_set_fg(flash > 0 ? TUI_BRIGHT_WHITE : TUI_BRIGHT_BLACK);
        tui_puts("input: lastkey=");
        if (last_key >= 32 && last_key < 127) tui_putc((char)last_key);
        else { tui_puts("#"); tui_putu((uint32_t)last_key); }
        tui_puts("  sfx=");
        tui_putu(trig_count);
        tui_puts("  result=");
        if (last_voice < 0)       tui_puts("-");
        else if (last_voice == 0) { tui_set_fg(TUI_BRIGHT_RED);   tui_puts("REJECTED"); }
        else                      { tui_set_fg(TUI_BRIGHT_GREEN); tui_puts("voice "); tui_putu((uint32_t)last_voice); }
        if (flash > 0) flash--;

        uint8_t bands[NB] = { 0 };
        if (have_audio) audio_get_levels(bands, NB);
        int eqtop = 6;
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
