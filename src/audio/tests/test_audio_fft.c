/* ============================================================
 *  test_audio_fft.c — band meter logic against known tones
 *
 *  The correctness we CAN verify in the sandbox: feed a synthetic
 *  sine at a known frequency, assert the resulting band levels peak
 *  in the band that contains that frequency (and not in distant
 *  bands). This proves the FFT kernel + log-spaced band bucketing +
 *  level mapping are wired correctly — independent of any audio
 *  output or real-time behaviour.
 *
 *  Build:
 *    cc -std=c11 -lm -Iinclude -o t \
 *       src/audio/tests/test_audio_fft.c src/audio/audio_fft.c \
 *       src/audio/audio_fft_kernel.c
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_fft.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                     \
    else { g_fail++; printf("  FAIL  %s  (%s:%d)\n",            \
                            msg, __FILE__, __LINE__); }         \
} while (0)

#define SR 44100u

/* Generate `n` stereo frames of a sine at freq Hz, amplitude amp,
 * into interleaved L,R (same on both channels). */
static void gen_sine(int16_t *stereo, uint32_t n, double freq, double amp) {
    for (uint32_t i = 0; i < n; i++) {
        double s = amp * __builtin_sin(2.0 * M_PI * freq * (double)i / (double)SR);
        int16_t v = (int16_t)s;
        stereo[2*i+0] = v;
        stereo[2*i+1] = v;
    }
}

/* Which band contains bin index `bin`? */
static int band_of_bin(const AudioFft *f, uint32_t bin) {
    for (uint32_t b = 0; b < AUDIO_FFT_BANDS; b++)
        if (bin >= f->band_lo[b] && bin <= f->band_hi[b]) return (int)b;
    return -1;
}

/* The bin a frequency maps to, for AUDIO_FFT_SIZE. */
static uint32_t bin_of_freq(double freq) {
    return (uint32_t)((freq * (double)AUDIO_FFT_SIZE / (double)SR) + 0.5);
}

/* Index of the loudest band. */
static int peak_band(const AudioFft *f) {
    int best = 0; uint8_t bv = 0;
    for (uint32_t b = 0; b < AUDIO_FFT_BANDS; b++)
        if (f->bands[b] > bv) { bv = f->bands[b]; best = (int)b; }
    return best;
}

/* Feed one window of a tone and run an update. */
static void feed_tone(AudioFft *f, double freq, double amp) {
    int16_t buf[AUDIO_FFT_SIZE * 2];
    /* feed enough frames to trigger an update (>= AUDIO_FFT_SIZE) */
    gen_sine(buf, AUDIO_FFT_SIZE, freq, amp);
    audio_fft_capture(f, buf, AUDIO_FFT_SIZE);
    bool ran = audio_fft_update(f);
    CHECK(ran, "update ran after a full window");
}

/* ---- disabled by default, zero cost ---- */
static void test_disabled_by_default(void) {
    AudioFft f; audio_fft_init(&f, SR);
    CHECK(!audio_fft_enabled(&f), "starts disabled");
    int16_t buf[AUDIO_FFT_SIZE * 2];
    gen_sine(buf, AUDIO_FFT_SIZE, 1000.0, 20000.0);
    audio_fft_capture(&f, buf, AUDIO_FFT_SIZE);     /* no-op */
    CHECK(!audio_fft_update(&f), "update no-op while disabled");
    uint8_t out[AUDIO_FFT_BANDS];
    CHECK(audio_fft_get_bands(&f, out, AUDIO_FFT_BANDS) == 0,
          "get_bands returns 0 while disabled");
}

/* ---- band edges are sane (monotonic, cover the spectrum) ---- */
static void test_band_edges(void) {
    AudioFft f; audio_fft_init(&f, SR);
    int ok = 1;
    for (uint32_t b = 0; b < AUDIO_FFT_BANDS; b++) {
        if (f.band_lo[b] > f.band_hi[b]) ok = 0;
        if (b > 0 && f.band_lo[b] < f.band_lo[b-1]) ok = 0;  /* monotonic */
        if (f.band_hi[b] >= AUDIO_FFT_SIZE/2) ok = 0;        /* in range  */
    }
    CHECK(ok, "band edges monotonic + within usable bins");
    CHECK(f.band_lo[0] >= 1, "first band skips DC");
}

/* ---- a low tone peaks in a low band ---- */
static void test_low_tone(void) {
    AudioFft f; audio_fft_init(&f, SR);
    audio_fft_set_enabled(&f, true);
    /* pick a frequency that lands a few bins up */
    double freq = (double)bin_of_freq(0) ; (void)freq;
    double f_low = 3.0 * (double)SR / (double)AUDIO_FFT_SIZE;  /* ~bin 3 */
    feed_tone(&f, f_low, 20000.0);

    int expect = band_of_bin(&f, bin_of_freq(f_low));
    int peak   = peak_band(&f);
    CHECK(expect >= 0, "low tone bin maps to a band");
    /* allow the peak to be the expected band or an immediate neighbour
     * (windowing spreads a little) */
    CHECK(peak <= expect + 1 && peak + 1 >= expect,
          "low tone peaks in (or adjacent to) its band");
    CHECK(peak < (int)AUDIO_FFT_BANDS / 2, "low tone peak is in lower half");
}

/* ---- a high tone peaks in a high band ---- */
static void test_high_tone(void) {
    AudioFft f; audio_fft_init(&f, SR);
    audio_fft_set_enabled(&f, true);
    /* near the top of the usable range */
    uint32_t hib = AUDIO_FFT_SIZE/2 - 4;
    double f_high = (double)hib * (double)SR / (double)AUDIO_FFT_SIZE;
    feed_tone(&f, f_high, 20000.0);

    int expect = band_of_bin(&f, bin_of_freq(f_high));
    int peak   = peak_band(&f);
    CHECK(expect >= 0, "high tone bin maps to a band");
    CHECK(peak >= expect - 1 && peak <= expect + 1,
          "high tone peaks in (or adjacent to) its band");
    CHECK(peak > (int)AUDIO_FFT_BANDS / 2, "high tone peak is in upper half");
}

/* ---- silence -> all bands ~0 ---- */
static void test_silence(void) {
    AudioFft f; audio_fft_init(&f, SR);
    audio_fft_set_enabled(&f, true);
    int16_t buf[AUDIO_FFT_SIZE * 2];
    memset(buf, 0, sizeof(buf));
    audio_fft_capture(&f, buf, AUDIO_FFT_SIZE);
    audio_fft_update(&f);
    uint8_t out[AUDIO_FFT_BANDS];
    audio_fft_get_bands(&f, out, AUDIO_FFT_BANDS);
    int allzero = 1;
    for (uint32_t b = 0; b < AUDIO_FFT_BANDS; b++) if (out[b] != 0) allzero = 0;
    CHECK(allzero, "silence -> all bands zero");
}

/* ---- louder tone -> higher band level than quiet tone ---- */
static void test_amplitude_tracks_level(void) {
    double f_mid = 20.0 * (double)SR / (double)AUDIO_FFT_SIZE;

    AudioFft fq; audio_fft_init(&fq, SR); audio_fft_set_enabled(&fq, true);
    feed_tone(&fq, f_mid, 2000.0);     /* quiet */
    int pbq = peak_band(&fq);
    uint8_t quiet = fq.bands[pbq];

    AudioFft fl; audio_fft_init(&fl, SR); audio_fft_set_enabled(&fl, true);
    feed_tone(&fl, f_mid, 28000.0);    /* loud */
    int pbl = peak_band(&fl);
    uint8_t loud = fl.bands[pbl];

    CHECK(loud > quiet, "louder tone yields a higher band level");
    CHECK(pbq == pbl, "same frequency peaks in the same band regardless of level");
}

/* ---- update only recomputes after a fresh window ---- */
static void test_update_throttle(void) {
    AudioFft f; audio_fft_init(&f, SR);
    audio_fft_set_enabled(&f, true);
    int16_t buf[AUDIO_FFT_SIZE * 2];
    gen_sine(buf, AUDIO_FFT_SIZE, 1000.0, 10000.0);

    /* feed half a window: not enough to trigger */
    audio_fft_capture(&f, buf, AUDIO_FFT_SIZE/2);
    CHECK(!audio_fft_update(&f), "no recompute before a full new window");
    /* feed the rest */
    audio_fft_capture(&f, buf, AUDIO_FFT_SIZE/2);
    CHECK(audio_fft_update(&f), "recompute once a full window accumulated");
    /* immediately again: nothing new */
    CHECK(!audio_fft_update(&f), "no recompute with no new samples");
}

int main(void) {
    test_disabled_by_default();
    test_band_edges();
    test_low_tone();
    test_high_tone();
    test_silence();
    test_amplitude_tracks_level();
    test_update_throttle();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
