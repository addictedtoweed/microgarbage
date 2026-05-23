/* ============================================================
 *  audio_fft.c — band meter logic (platform-independent, no libm)
 *
 *  Capture window + log-spaced band bucketing + level mapping. The
 *  FFT itself is the swappable kernel (audio_fft_kernel_mag); this
 *  file is pure integer bucketing and is fully testable on the
 *  desktop against synthetic tones. No floating point / libm — the
 *  repo avoids it and the M4 target is fixed-point.
 *
 *  See audio_fft.h for the RT/non-RT split.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_fft.h"

#include <string.h>

/* Scratch for the linearized window and magnitudes. Static (single-
 * context service). */
static int16_t  s_lin[AUDIO_FFT_SIZE];
static uint32_t s_mag[AUDIO_FFT_SIZE / 2];

/* integer log2 of x (position of highest set bit); ilog2(0)=0. */
static uint32_t ilog2_u32(uint32_t x) {
    uint32_t r = 0;
    while (x > 1u) { x >>= 1; r++; }
    return r;
}

void audio_fft_init(AudioFft *f, uint32_t sample_rate) {
    if (!f) return;
    memset(f, 0, sizeof(*f));
    f->enabled     = false;
    f->sample_rate = sample_rate ? sample_rate : 44100u;

    /* Log-spaced band edges over usable bins [first .. last], no pow.
     * We want edge(b) = first * (last/first)^(b/BANDS), with
     * edge(BANDS) == last exactly. Work in a q16 "octave" measure:
     *   total_oct_q16 = log2(last/first) in q16
     * then edge(b) = first * 2^(total_oct_q16 * b / BANDS).
     * 2^x for x = (i + frac) in q16 is (1<<i) * 2^frac; 2^frac is
     * approximated by a small fixed-point table (8 entries) so the
     * top octave isn't dropped by integer truncation. */
    uint32_t usable = AUDIO_FFT_SIZE / 2;          /* bins 0..usable-1 */
    uint32_t first  = 1;
    uint32_t last   = usable - 1;

    /* total octaves between first and last, in q16. log2 via integer
     * part + a fractional refinement: frac = (last - 2^ip) / 2^ip. */
    uint32_t ip = ilog2_u32(last / first);         /* integer octaves */
    uint32_t base = (first << ip);                 /* 2^ip * first     */
    uint32_t frac_q16 = (base > 0)
        ? (uint32_t)(((uint64_t)(last - base) << 16) / base)   /* 0..1 q16 */
        : 0;
    uint32_t total_oct_q16 = (ip << 16) + frac_q16;

    /* 2^frac table, frac in eighths, q16 (1.0 == 65536). */
    static const uint32_t pow2_frac[9] = {
        65536, 71468, 77936, 84990, 92682, 101070, 110218, 120194, 131072
    };

    for (uint32_t b = 0; b <= AUDIO_FFT_BANDS; b++) {
        uint32_t e_q16 = (uint32_t)(((uint64_t)total_oct_q16 * b) / AUDIO_FFT_BANDS);
        uint32_t ipart = e_q16 >> 16;
        uint32_t fpart = e_q16 & 0xFFFFu;
        /* interpolate the 2^frac table (8 segments) */
        uint32_t seg = (fpart * 8u) >> 16;           /* 0..7 */
        uint32_t segpos = (fpart * 8u) & 0xFFFFu;    /* within-seg q16 */
        uint32_t lo2 = pow2_frac[seg], hi2 = pow2_frac[seg + 1];
        uint32_t p2 = lo2 + (uint32_t)(((uint64_t)(hi2 - lo2) * segpos) >> 16);
        uint32_t edge = (uint32_t)(((uint64_t)first * (1u << ipart) * p2) >> 16);
        if (edge < first) edge = first;
        if (edge > last)  edge = last;
        /* store: band b uses [edge(b) .. edge(b+1)] */
        if (b < AUDIO_FFT_BANDS) f->band_lo[b] = (uint16_t)edge;
        if (b > 0)               f->band_hi[b - 1] = (uint16_t)edge;
    }
    /* ensure each band has at least one bin and no gaps/overlap holes */
    for (uint32_t b = 0; b < AUDIO_FFT_BANDS; b++) {
        if (f->band_hi[b] < f->band_lo[b]) f->band_hi[b] = f->band_lo[b];
    }
}

void audio_fft_set_enabled(AudioFft *f, bool enabled) {
    if (!f) return;
    if (enabled && !f->enabled) {
        f->write_pos = 0;
        f->since_update = 0;
        memset(f->window, 0, sizeof(f->window));
        memset(f->bands, 0, sizeof(f->bands));
    }
    f->enabled = enabled;
}

bool audio_fft_enabled(const AudioFft *f) { return f && f->enabled; }

void audio_fft_capture(AudioFft *f, const int16_t *interleaved_stereo,
                       uint32_t n) {
    if (!f || !f->enabled || !interleaved_stereo) return;
    for (uint32_t i = 0; i < n; i++) {
        int32_t l = interleaved_stereo[2 * i + 0];
        int32_t r = interleaved_stereo[2 * i + 1];
        f->window[f->write_pos] = (int16_t)((l + r) / 2);
        f->write_pos = (f->write_pos + 1u) % AUDIO_FFT_SIZE;
    }
    if (f->since_update < (uint32_t)-1 - n) f->since_update += n;
    else f->since_update = (uint32_t)-1;
}

static void linearize(const AudioFft *f) {
    uint32_t pos = f->write_pos;     /* oldest sample is at write_pos */
    for (uint32_t i = 0; i < AUDIO_FFT_SIZE; i++) {
        s_lin[i] = f->window[pos];
        pos = (pos + 1u) % AUDIO_FFT_SIZE;
    }
}

bool audio_fft_update(AudioFft *f) {
    if (!f || !f->enabled) return false;
    if (f->since_update < AUDIO_FFT_SIZE) return false;
    f->since_update = 0;

    linearize(f);
    audio_fft_kernel_mag(s_lin, AUDIO_FFT_SIZE, s_mag);

    for (uint32_t b = 0; b < AUDIO_FFT_BANDS; b++) {
        uint32_t lo = f->band_lo[b], hi = f->band_hi[b];
        uint64_t sum = 0;
        for (uint32_t k = lo; k <= hi; k++) sum += s_mag[k];
        uint32_t width = (hi >= lo) ? (hi - lo + 1u) : 1u;
        uint32_t avg = (uint32_t)(sum / width);

        /* Level on a log (dB-ish) scale via integer log2, no libm.
         * avg is squared magnitude; ilog2(avg) ~ proportional to dB.
         * Map ilog2 in [0..32] to 0..255: each bit ≈ 6 dB, ~8 units. */
        uint32_t lg = ilog2_u32(avg + 1u);       /* 0..31 */
        uint32_t level = lg * 8u;                 /* ~ up to 248 */
        if (level > 255u) level = 255u;
        f->bands[b] = (uint8_t)level;
    }
    return true;
}

uint32_t audio_fft_get_bands(const AudioFft *f, uint8_t *out, uint32_t max) {
    if (!f || !f->enabled || !out) return 0;
    uint32_t n = (max < AUDIO_FFT_BANDS) ? max : AUDIO_FFT_BANDS;
    memcpy(out, f->bands, n);
    return n;
}
