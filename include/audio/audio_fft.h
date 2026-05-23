/* ============================================================
 *  audio_fft.h — band meters over the mixed output
 *
 *  Computes a small set of frequency-band levels from the final
 *  mixed audio output, so any app can read a "band meter" via the
 *  GET_LEVELS ecall WITHOUT doing an FFT itself in interpreted
 *  guest code. Computed once on the service side, shared by all
 *  consumers, enable/disable-able.
 *
 *  ---------------------------------------------------------------
 *  RT-safety (the one rule)
 *  ---------------------------------------------------------------
 *  The FFT does NOT run on the real-time render/DMA path. The
 *  service's render appends output to a rolling capture window
 *  (cheap, RT-safe) via audio_fft_capture(); the non-RT service
 *  loop calls audio_fft_update() periodically, which (only when
 *  enabled, and only when enough new samples have accumulated) runs
 *  the FFT over the captured window, buckets bins into bands, and
 *  stores the band levels. GET_LEVELS just copies the stored bands.
 *  So the FFT is an audio_fft_update()-class background task, never
 *  inside mixer_render. (On the M4 this matters: the DAC DMA fill
 *  has a hard deadline; the FFT runs between fills.)
 *
 *  ---------------------------------------------------------------
 *  FFT kernel is swappable
 *  ---------------------------------------------------------------
 *  The band-bucketing + windowing + level mapping here are
 *  platform-independent and fully testable on the desktop (feed a
 *  known sine, assert the energy lands in the right band). Only the
 *  FFT KERNEL swaps: a compact portable radix-2 (audio_fft_kernel.c)
 *  on the desktop, CMSIS-DSP (arm_rfft_q15) on the M4. Both produce
 *  a magnitude spectrum this module buckets identically.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef AUDIO_FFT_H
#define AUDIO_FFT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* FFT size (samples). 256 is plenty for a meter and cheap (~20-35us
 * on the M4 via CMSIS-DSP). Must be a power of two. */
#ifndef AUDIO_FFT_SIZE
#define AUDIO_FFT_SIZE   256u
#endif

/* Number of frequency bands the meter reports (log-spaced, like a
 * graphic EQ display). */
#ifndef AUDIO_FFT_BANDS
#define AUDIO_FFT_BANDS  16u
#endif

typedef struct {
    bool      enabled;
    uint32_t  sample_rate;

    /* rolling capture: the most recent AUDIO_FFT_SIZE mono samples of
     * the mixed output. capture() writes here (RT side); update()
     * reads here (non-RT side). On the single-context service these
     * never truly race, but capture is kept trivially cheap regardless. */
    int16_t   window[AUDIO_FFT_SIZE];
    uint32_t  write_pos;        /* next write index (mod size)        */
    uint32_t  since_update;     /* samples captured since last FFT    */

    /* most recent band levels, 0..255 each (log-scaled for display). */
    uint8_t   bands[AUDIO_FFT_BANDS];

    /* precomputed band bin ranges (lo..hi inclusive) over the
     * AUDIO_FFT_SIZE/2 usable magnitude bins. */
    uint16_t  band_lo[AUDIO_FFT_BANDS];
    uint16_t  band_hi[AUDIO_FFT_BANDS];
} AudioFft;

/* Initialize. sample_rate sets the bin->frequency mapping for the
 * log-spaced band edges. Meters start DISABLED (zero cost until a
 * consumer enables them). */
void audio_fft_init(AudioFft *f, uint32_t sample_rate);

/* Enable/disable. While disabled, capture() and update() are no-ops
 * (the FFT cost is fully opt-in). */
void audio_fft_set_enabled(AudioFft *f, bool enabled);
bool audio_fft_enabled(const AudioFft *f);

/* RT side: append `n` frames of mixed output to the capture window.
 * `interleaved_stereo` points at n*2 int16 (L,R); we capture the mono
 * mix (L+R)/2. Cheap; safe to call from render. No-op if disabled. */
void audio_fft_capture(AudioFft *f, const int16_t *interleaved_stereo,
                       uint32_t n);

/* Non-RT side: if enabled and enough new samples have accumulated
 * (>= AUDIO_FFT_SIZE since the last run), run the FFT over the window
 * and refresh the band levels. Cheap no-op otherwise. Returns true if
 * it recomputed the bands this call. */
bool audio_fft_update(AudioFft *f);

/* Copy up to `max` band levels (0..255) into `out`. Returns the
 * number written (min(max, AUDIO_FFT_BANDS)). 0 if disabled. */
uint32_t audio_fft_get_bands(const AudioFft *f, uint8_t *out, uint32_t max);

/* ---- the swappable FFT kernel ----
 * Compute the magnitude spectrum of `n` real int16 samples. Writes
 * n/2 magnitude values (bin 0 = DC .. bin n/2-1) into `out_mag` as
 * uint32 (squared magnitude is fine; the bander only compares/relates
 * them). `n` is AUDIO_FFT_SIZE (power of two). Implemented by
 * audio_fft_kernel.c (desktop radix-2) or a CMSIS-DSP backend (M4). */
void audio_fft_kernel_mag(const int16_t *in, uint32_t n, uint32_t *out_mag);

#endif /* AUDIO_FFT_H */
