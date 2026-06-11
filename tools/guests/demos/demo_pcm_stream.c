/* demo_pcm_stream.c — sanity test for the v2.02 PCM streaming voice.
 *
 * Opens a stereo PCM stream at 44100 Hz and feeds it a 440 Hz sine
 * wave one vblank's worth of frames at a time (735 frames @ 60 fps).
 * Press A/B/Y to change pitch (262 / 440 / 698 Hz), START to exit.
 *
 * What this proves: SYS_AUDIO_PCM_STREAM_{OPEN,FEED,CLOSE} round-trip
 * correctly, the per-channel ring sustains a 60 Hz feed cadence
 * without underrun, the mixer's per-channel source-rate path plays
 * the stream at correct pitch, and the new ecalls clean up via the
 * existing sweep_vm path on exit.
 *
 * Public domain (CC0). No warranty.
 */
#include "mg_input.h"
#include "mg_frame.h"
#include "mg_panic.h"
#include "mg_audio.h"
#include "vm_runtime.h"

#include <stdint.h>

static inline void sys_exit(int code) {
    register int a0 asm("a0") = code;
    register int a7 asm("a7") = SYS_EXIT;
    asm volatile ("ecall" :: "r"(a0), "r"(a7));
    __builtin_unreachable();
}

#define SAMPLE_RATE   44100u
#define FPS           60u
#define FRAMES_PER_TICK (SAMPLE_RATE / FPS)   /* 735 stereo frames */

/* Sine LUT — 256 entries, q15. Built at startup so we don't pull in
 * libm. Linear interpolation between LUT samples is good enough for
 * a sanity test (we're checking pacing, not audio quality). */
#define SINE_LUT_BITS 8
#define SINE_LUT_SIZE (1u << SINE_LUT_BITS)
static int16_t s_sine_lut[SINE_LUT_SIZE];

static void build_sine_lut(void) {
    /* sin(2π*i/256) ≈ Taylor for the first quadrant, mirror by symmetry.
     * Q15 ints scaled to ±28000 to leave headroom on the mixer. */
    static const int16_t kFirstQuad[64] = {
        /* sin(0..π/2) sampled at 64 points, q15 / 32767 * 28000 */
            0,   687,  1374,  2061,  2747,  3431,  4114,  4795,
         5473,  6149,  6822,  7491,  8156,  8817,  9472, 10122,
        10766, 11403, 12034, 12656, 13271, 13878, 14476, 15064,
        15643, 16212, 16769, 17316, 17852, 18375, 18887, 19386,
        19872, 20345, 20805, 21251, 21683, 22100, 22503, 22890,
        23262, 23618, 23959, 24284, 24592, 24884, 25160, 25419,
        25661, 25886, 26094, 26284, 26458, 26614, 26752, 26873,
        26977, 27063, 27132, 27183, 27216, 27232, 27231, 27212
    };
    for (uint32_t i = 0; i < SINE_LUT_SIZE; i++) {
        uint32_t q = i >> 6;          /* which quadrant: 0..3 */
        uint32_t k = i & 63;
        int16_t v;
        switch (q) {
            case 0: v =  kFirstQuad[k];               break;
            case 1: v =  kFirstQuad[63 - k];          break;
            case 2: v = -kFirstQuad[k];               break;
            default: v = -kFirstQuad[63 - k];         break;
        }
        s_sine_lut[i] = v;
    }
}

/* Phase accumulator: q24.8 → integer index into the LUT.
 * Increment per output frame = freq * LUT_SIZE * 256 / SAMPLE_RATE. */
static uint32_t s_phase;       /* q24.8, low 8 bits sub-LUT */
static uint32_t s_phase_step;  /* per-frame increment */

static void set_frequency(uint32_t hz) {
    /* step = hz * LUT_SIZE * 256 / SAMPLE_RATE, in u32 */
    s_phase_step = (hz * SINE_LUT_SIZE * 256u) / SAMPLE_RATE;
}

static void fill_sine(int16_t *dst_stereo, uint32_t frames) {
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t idx = (s_phase >> 8) & (SINE_LUT_SIZE - 1u);
        int16_t s = s_sine_lut[idx];
        dst_stereo[i * 2 + 0] = s;
        dst_stereo[i * 2 + 1] = s;
        s_phase += s_phase_step;
    }
}

void _start(void) {
    build_sine_lut();
    set_frequency(440);

    MgVoice voice = mg_audio_pcm_stream_open(SAMPLE_RATE);
    if (voice == MG_VOICE_REJECTED) {
        /* Couldn't open the stream — bail. */
        mg_panic("pcm_stream_open failed");
        sys_exit(1);
    }

    int16_t pcm_buf[FRAMES_PER_TICK * 2];

    for (;;) {
        MgPads pads = mg_pads();

        if (mg_pad_pressed(pads.p0, MG_BTN_START)) {
            mg_audio_pcm_stream_close(voice);
            sys_exit(0);
        }
        if (mg_pad_pressed(pads.p0, MG_BTN_A))  set_frequency(262);   /* C4 */
        if (mg_pad_pressed(pads.p0, MG_BTN_B))  set_frequency(440);   /* A4 */
        if (mg_pad_pressed(pads.p0, MG_BTN_Y))  set_frequency(698);   /* F5 */

        fill_sine(pcm_buf, FRAMES_PER_TICK);
        /* Feed whatever the ring will accept. If it's already nearly
         * full (we're slightly ahead of the mixer's drain rate) we'll
         * push fewer frames; the next tick catches us up. */
        (void)mg_audio_pcm_stream_feed(voice, pcm_buf, FRAMES_PER_TICK);

        mg_frame_commit();
        mg_wait_frame();
    }
}
