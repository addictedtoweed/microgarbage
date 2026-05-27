/* ============================================================
 *  audiotest.c — minimal guest audio smoke test (shell app)
 *
 *  Run from the shell: `run /host/audiotest.elf` (or bare path). It
 *  exercises the audio ecall path end to end through the real VM:
 *  build a tone in guest memory, load it (object handle), trigger it
 *  (voice handle), set gain, stop. It prints what happened so you can
 *  see the handles come back — proving guest -> ecall -> channel ->
 *  service -> pool/arbiter/mixer works.
 *
 *  It does NOT itself make the host's speakers play — that needs the
 *  platform audio-output backend (ring -> sound device), which is
 *  separate. This confirms the control path is wired correctly.
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "vm_runtime.h"
#include "audio.h"
#include <stdint.h>

/* tiny printf via the host format syscall */
static void puts_(const char *s) {
    uint32_t n = 0; while (s[n]) n++;
    (void)_vm_sys3(SYS_WRITE, 1, (uint32_t)s, n);
}
static void putu(uint32_t v) {
    char b[12]; int i = 10; b[11] = 0;
    if (v == 0) { puts_("0"); return; }
    while (v && i >= 0) { b[i--] = (char)('0' + (v % 10)); v /= 10; }
    puts_(&b[i + 1]);
}

#define TONE_FRAMES 4410          /* 0.1s at 44.1k */
static int16_t g_tone[TONE_FRAMES];

int main(void) {
    /* Bail cleanly if the host has no audio service wired up (some
     * minimal/headless host builds omit it). Otherwise every call
     * returns -ENOSYS and looks like garbage. */
    if (!audio_available()) {
        puts_("audiotest: audio service not available on this host build.\n");
        return 1;
    }

    /* Build a simple square-ish tone (mono16). */
    for (int i = 0; i < TONE_FRAMES; i++)
        g_tone[i] = (int16_t)(((i / 50) & 1) ? 8000 : -8000);

    puts_("audiotest: loading tone...\n");
    audio_object obj = audio_load_sample(g_tone, sizeof(g_tone));
    if (obj == AUDIO_OBJECT_NONE) {
        puts_("audiotest: load FAILED (audio not available?)\n");
        return 1;
    }
    puts_("audiotest: loaded object handle = "); putu(obj); puts_("\n");

    puts_("audiotest: triggering...\n");
    audio_voice v = audio_trigger(obj, AUDIO_GAIN_UNITY, AUDIO_PAN_CENTER);
    if (v == AUDIO_VOICE_NONE) {
        puts_("audiotest: trigger REJECTED / failed\n");
        audio_free(obj);
        return 1;
    }
    puts_("audiotest: playing voice handle = "); putu(v); puts_("\n");

    /* halve the gain */
    audio_set_gain(v, AUDIO_GAIN_UNITY / 2);
    puts_("audiotest: gain set to half\n");

    /* let it run a moment, then stop */
    (void)_vm_sys1(SYS_SLEEP_TICKS, 10);
    audio_stop(v);
    audio_free(obj);
    puts_("audiotest: stopped + freed. OK.\n");

    /* ---- music path: pair two samples, play, stop ---- */
    puts_("audiotest: loading music (intro+loop)...\n");
    audio_object intro = audio_load_sample(g_tone, sizeof(g_tone));
    audio_object loop  = audio_load_sample(g_tone, sizeof(g_tone));
    if (intro == AUDIO_OBJECT_NONE || loop == AUDIO_OBJECT_NONE) {
        puts_("audiotest: music sample load failed\n");
        return 1;
    }
    audio_object music = audio_load_music(intro, loop);
    /* the music object holds its own refs; drop ours */
    audio_free(intro);
    audio_free(loop);
    if (music == AUDIO_OBJECT_NONE) {
        puts_("audiotest: load_music failed\n");
        return 1;
    }
    puts_("audiotest: music handle = "); putu(music); puts_("\n");
    audio_voice mv = audio_play_music(music, 0);
    if (mv == AUDIO_VOICE_NONE) {
        puts_("audiotest: play_music REJECTED\n");
        audio_free(music);
        return 1;
    }
    puts_("audiotest: music playing, voice = "); putu(mv); puts_("\n");

    /* enable the band meter and read it a few times while music plays */
    audio_fft_enable(1);
    for (int t = 0; t < 3; t++) {
        (void)_vm_sys1(SYS_SLEEP_TICKS, 4);
        uint8_t bands[16];
        uint32_t nb = audio_get_levels(bands, 16);
        puts_("audiotest: meter bands="); putu(nb); puts_(" [");
        for (uint32_t i = 0; i < nb; i++) { putu(bands[i]); puts_(i+1<nb?" ":""); }
        puts_("]\n");
    }
    audio_fft_enable(0);

    /* ---- drop-in .wav asset playback ---- */
    puts_("audiotest: trying /host/asset.wav ...\n");
    {
        audio_object wav = audio_load_wav("/host/asset.wav");
        if (wav == AUDIO_OBJECT_NONE) {
            puts_("audiotest: no asset.wav (drop one in host dir to test)\n");
        } else {
            puts_("audiotest: loaded asset.wav, object = "); putu(wav); puts_("\n");
            audio_voice av = audio_trigger(wav, AUDIO_GAIN_UNITY, AUDIO_PAN_CENTER);
            puts_("audiotest: asset playing, voice = "); putu(av); puts_("\n");
            (void)_vm_sys1(SYS_SLEEP_TICKS, 8);
            audio_stop(av);
            audio_free(wav);
            puts_("audiotest: asset stopped + freed.\n");
        }
    }

    (void)_vm_sys1(SYS_SLEEP_TICKS, 10);
    audio_stop(mv);
    audio_free(music);
    puts_("audiotest: music stopped + freed. OK.\n");
    return 0;
}
