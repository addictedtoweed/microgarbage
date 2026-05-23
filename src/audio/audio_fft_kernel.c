/* ============================================================
 *  audio_fft_kernel.c — fixed-point radix-2 FFT (desktop backend)
 *
 *  A compact iterative Cooley-Tukey radix-2 FFT producing a
 *  magnitude spectrum from real int16 input. FIXED-POINT, no libm —
 *  the repo deliberately avoids floating-point math (include/math.h
 *  is the fixed-point aggregator, not system libm), and this matches
 *  the target: the M4 kernel will be CMSIS-DSP arm_rfft_q15, also
 *  fixed-point. Both produce magnitude bins that audio_fft.c buckets
 *  identically.
 *
 *  Twiddle factors and a Hann window are precomputed once into q15
 *  tables using an integer polynomial sine approximation (accurate to
 *  ~15 bits — far more than a 0..255 meter needs). No runtime sin/cos.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "audio/audio_fft.h"
#include "math/fixed_point.h"

#define KN  AUDIO_FFT_SIZE

/* ---- q15 polynomial sine with quadrant folding, no libm ----
 * qsin(turn): turn in [0, 32768) maps to [0, 2pi). Returns q15.
 * Uses a 3-term odd minimax polynomial on a quarter wave. Accurate
 * to ~15 bits — far more than a 0..255 meter needs. cos = sin+pi/2. */
static q15_t qsin(uint32_t turn) {
    turn &= 0x7FFFu;
    uint32_t q = (turn >> 13) & 3u;
    uint32_t x = turn & 0x1FFFu;               /* 0..8191 */
    /* For quadrants 1 and 3, the angle within-quarter mirrors. */
    uint32_t xx = x;
    if (q == 1 || q == 3) xx = 8192u - x;      /* mirror */
    if (xx > 8191u) xx = 8191u;

    int32_t t  = (int32_t)xx << 2;             /* q15 in [0,1) */
    int64_t t1 = t;
    int64_t t2 = (t1 * t1) >> 15;
    int64_t t3 = (t2 * t1) >> 15;
    int64_t t5 = (t3 * t2) >> 15;
    const int64_t A1 = 51472, A3 = 21073, A5 = 2382;
    int64_t s = (A1 * t1 - A3 * t3 + A5 * t5) >> 15;
    if (s > 32767) s = 32767;
    if (s < -32767) s = -32767;

    /* sign per quadrant: sin is + in q0,q1 and - in q2,q3 */
    if (q >= 2) s = -s;
    return (q15_t)s;
}
static q15_t qcos(uint32_t turn) { return qsin(turn + 8192u); } /* +pi/2 */

/* ---- precomputed tables (built once) ---- */
static q15_t   s_win[KN];        /* Hann window, q15 */
static q15_t   s_tw_re[KN];      /* twiddle cos, indexed by k for full N */
static q15_t   s_tw_im[KN];      /* twiddle sin */
static int     s_ready = 0;

static uint32_t bitrev(uint32_t x, uint32_t bits) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < bits; i++) { r = (r << 1) | (x & 1u); x >>= 1; }
    return r;
}

static void build_tables(void) {
    /* Hann: 0.5 - 0.5*cos(2*pi*i/(N-1)). In q15. */
    for (uint32_t i = 0; i < KN; i++) {
        uint32_t turn = (uint32_t)(((uint64_t)i * 32768u) / (KN - 1)); /* 0..2pi */
        int32_t c = qcos(turn);                       /* q15 */
        int32_t w = (16384 - (c >> 1));               /* 0.5 - 0.5c, q15 */
        if (w < 0) w = 0;
        if (w > 32767) w = 32767;
        s_win[i] = (q15_t)w;
    }
    /* twiddles W_N^k = cos(2pi k/N) - j sin(2pi k/N), for k in [0,N). */
    for (uint32_t k = 0; k < KN; k++) {
        uint32_t turn = (uint32_t)(((uint64_t)k * 32768u) / KN);
        s_tw_re[k] =  qcos(turn);
        s_tw_im[k] = (q15_t)(-qsin(turn));
    }
    s_ready = 1;
}

/* fixed-point work buffers (q15-ish, but we let magnitudes grow into
 * int32 during butterflies, rescaling by >>1 per stage to avoid
 * overflow — standard block-floating-point-lite). */
static int32_t s_re[KN];
static int32_t s_im[KN];

void audio_fft_kernel_mag(const int16_t *in, uint32_t n, uint32_t *out_mag) {
    if (n != KN) return;
    if (!s_ready) build_tables();

    uint32_t bits = 0; while ((1u << bits) < n) bits++;

    /* windowed, bit-reversed load. q15 window * int16 sample -> scaled. */
    for (uint32_t i = 0; i < n; i++) {
        int32_t x = ((int32_t)in[i] * s_win[i]) >> 15;
        uint32_t j = bitrev(i, bits);
        s_re[j] = x;
        s_im[j] = 0;
    }

    /* radix-2 butterflies with per-stage >>1 scaling */
    for (uint32_t len = 2; len <= n; len <<= 1) {
        uint32_t step = n / len;             /* twiddle stride */
        for (uint32_t i = 0; i < n; i += len) {
            uint32_t tw = 0;
            for (uint32_t k = 0; k < len / 2; k++) {
                uint32_t a = i + k;
                uint32_t b = i + k + len / 2;
                int32_t wr = s_tw_re[tw];
                int32_t wi = s_tw_im[tw];
                int32_t vr = ( (int64_t)s_re[b] * wr - (int64_t)s_im[b] * wi ) >> 15;
                int32_t vi = ( (int64_t)s_re[b] * wi + (int64_t)s_im[b] * wr ) >> 15;
                int32_t ur = s_re[a];
                int32_t ui = s_im[a];
                /* >>1 scaling keeps values bounded across log2(N) stages */
                s_re[a] = (ur + vr) >> 1;
                s_im[a] = (ui + vi) >> 1;
                s_re[b] = (ur - vr) >> 1;
                s_im[b] = (ui - vi) >> 1;
                tw += step;
            }
        }
    }

    /* squared magnitude for bins 0..n/2-1 */
    for (uint32_t i = 0; i < n / 2; i++) {
        int64_t mag2 = (int64_t)s_re[i] * s_re[i] + (int64_t)s_im[i] * s_im[i];
        if (mag2 > (int64_t)0xFFFFFFFFu) mag2 = (int64_t)0xFFFFFFFFu;
        out_mag[i] = (uint32_t)mag2;
    }
}
