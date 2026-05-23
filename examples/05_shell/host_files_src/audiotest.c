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
 * ============================================================ */

#include "lib/vm_runtime.h"
#include "lib/audio.h"
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
    return 0;
}
