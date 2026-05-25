/* ============================================================
 *  fast_div.c — Granlund-Möller fast division
 *
 *  Reference: Torbjörn Granlund and Peter L. Montgomery,
 *  "Division by Invariant Integers using Multiplication"
 *  (PLDI 1994). https://gmplib.org/~tege/divcnst-pldi94.pdf
 *
 *  ----------------------------------------------------------------
 *  The core idea
 *  ----------------------------------------------------------------
 *
 *  Plain integer division n/d on a CPU without hardware divide is
 *  slow (software emulation runs ~150 cycles on Cortex-M0). Even
 *  with hardware divide (Cortex-M3/M4), it's ~12 cycles versus 1-3
 *  for multiply. If you know `d` ahead of time and will divide
 *  many `n`s by it, you can precompute an approximate reciprocal
 *  `m` such that:
 *
 *      n / d  ==  (n * m) >> (W + L)        for all n in range
 *
 *  where W is the word width (32 or 64) and L is some shift. The
 *  approximation is "off by at most a fraction less than one" if
 *  m is chosen correctly, so flooring gives the exact integer
 *  quotient.
 *
 *  ----------------------------------------------------------------
 *  Picking m and L
 *  ----------------------------------------------------------------
 *
 *  Let L = ceil(log2(d)). Choose m = ceil(2^(W+L) / d).
 *
 *  Why this works: m is the smallest integer at least as big as
 *  2^(W+L)/d. So m*d >= 2^(W+L), and m*n/d <= n*2^(W+L)/(d*d).
 *  More usefully: floor(m*n / 2^(W+L)) equals floor(n/d) for all
 *  n in [0, 2^W) — Granlund-Möller prove this. The slack between
 *  m*n and (n/d)*2^(W+L) is small enough that the flooring works.
 *
 *  ----------------------------------------------------------------
 *  The W+1 bit problem
 *  ----------------------------------------------------------------
 *
 *  Here's the catch: m can be up to 2^(W+1) - 1. So it doesn't
 *  always fit in W bits. Two cases:
 *
 *   Case 1: m < 2^W. Easy. Compute mulhi(n, m) (which is
 *           (n*m) >> W), then shift by L:
 *               q = mulhi(n, m) >> L
 *
 *   Case 2: m >= 2^W. Store m' = m - 2^W in the prepared struct.
 *           Then n*m = n*2^W + n*m'. After dividing by 2^(W+L):
 *               q = (n + mulhi(n, m')) >> L
 *           But n + mulhi(n, m') can overflow W bits. The trick:
 *               t = mulhi(n, m')
 *               q = ((n - t) >> 1 + t) >> (L - 1)
 *           This rewrites (n + t)/2^L as (((n - t)/2) + t)/2^(L-1),
 *           which keeps every intermediate within W bits because
 *           t < n always (when m >= 2^W).
 *
 *  ----------------------------------------------------------------
 *  Power-of-2 shortcut
 *  ----------------------------------------------------------------
 *
 *  When d is 2^k, division is just `n >> k`. We detect this at
 *  prepare time and set FDIV_FLAG_POW2. The execute function
 *  checks this flag first and skips the multiply entirely.
 *
 *  ----------------------------------------------------------------
 *  Signed division
 *
 *  Signed division has two complications:
 *  1. C semantics truncate toward zero; the magic-number trick
 *     naturally floors. They differ for negative dividends.
 *  2. Sign of the result is sign(n) XOR sign(d).
 *
 *  Our approach: prepare using |d| (storing sign of d in flags).
 *  At execute time, work with |n|, do the unsigned-style divide,
 *  then apply sign. For C-style truncation, the unsigned result
 *  is already correct because we used absolute values throughout —
 *  the truncated quotient of |n|/|d| is the same as the unsigned
 *  floor. Only the sign of the result needs adjustment.
 *
 *  This is more arithmetic than the paper's direct signed variant
 *  but much easier to verify correctness against plain n/d.
 *
 *  ----------------------------------------------------------------
 *  Remainder via the qr functions
 *  ----------------------------------------------------------------
 *
 *  rem = n - q * d. We have q from the magic-number divide and d
 *  is stored in the prepared struct, so this is one multiply and
 *  one subtract. For signed, sign(rem) == sign(n) (C99 semantics).
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#include "math/fast_div.h"
#include "math/bits.h"

/* ============================================================
 *  Bit utilities — count leading zeros, high-half multiply
 * ============================================================ */

/* Count leading zeros now live in math/bits.h (CLZ instruction on the
 * MCU, software fallback elsewhere; clz(0) defined as the full width,
 * which prepare relies on for d == 1). Thin aliases keep the call sites
 * below unchanged. Used only in prepare, so speed doesn't matter. */
static inline uint8_t clz_u32(uint32_t x) { return bits_clz32(x); }
static inline uint8_t clz_u64(uint64_t x) { return bits_clz64(x); }

/* High 32 bits of the 64-bit product a*b. */
static uint32_t mulhi_u32(uint32_t a, uint32_t b) {
    return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
}

/* High 64 bits of the 128-bit product a*b, via four 32x32->64
 * partial products. This is the expensive operation on a 32-bit
 * MCU since there's no hardware support for 128-bit multiply.
 *
 * Let a = a_hi*2^32 + a_lo, b = b_hi*2^32 + b_lo (a_lo, b_lo unsigned).
 * Then a*b = (a_hi*b_hi)*2^64
 *          + (a_hi*b_lo + a_lo*b_hi)*2^32
 *          + a_lo*b_lo
 * High 64 bits =  a_hi*b_hi
 *              + (a_hi*b_lo + a_lo*b_hi) >> 32
 *              + carries from the lower 64 bits
 *
 * We sum carries by computing the cross-product middle term in
 * 64 bits and watching its overflow. */
static uint64_t mulhi_u64(uint64_t a, uint64_t b) {
    uint64_t a_lo = a & 0xFFFFFFFFu;
    uint64_t a_hi = a >> 32;
    uint64_t b_lo = b & 0xFFFFFFFFu;
    uint64_t b_hi = b >> 32;

    uint64_t lo_lo = a_lo * b_lo;     /* contributes only carries to high half */
    uint64_t hi_lo = a_hi * b_lo;
    uint64_t lo_hi = a_lo * b_hi;
    uint64_t hi_hi = a_hi * b_hi;

    /* The middle term (hi_lo + lo_hi + carry from lo_lo) lives at
     * bits 32..95. Its low 32 bits and the high 32 of lo_lo form
     * the low 64 of the product (we discard those). Its high 32
     * bits feed into the upper 64. */
    uint64_t mid = (lo_lo >> 32)
                 + (hi_lo & 0xFFFFFFFFu)
                 + (lo_hi & 0xFFFFFFFFu);

    return hi_hi + (hi_lo >> 32) + (lo_hi >> 32) + (mid >> 32);
}

/* ============================================================
 *  Unsigned 32-bit
 * ============================================================ */

div_u32_t fdiv_u32_prepare(uint32_t d) {
    div_u32_t r;
    r.d = d;
    r.magic = 0;
    r.shift = 0;
    r.flags = 0;

    /* Power-of-2 fast path. d & (d-1) == 0 iff d is a power of 2
     * (and d != 0, which is a precondition). */
    if ((d & (d - 1)) == 0) {
        r.flags = FDIV_FLAG_POW2;
        r.shift = (uint8_t)(31 - clz_u32(d));   /* log2(d) */
        return r;
    }

    /* General case. L = ceil(log2(d)). Since d isn't a power of 2,
     * 32 - clz(d-1) gives ceil(log2(d)) exactly. */
    uint8_t L = (uint8_t)(32 - clz_u32(d - 1));

    /* Compute m = ceil(2^(32+L) / d).
     *
     * 2^(32+L) doesn't fit in 64 bits when L >= 32. But L can be
     * at most 32 here (d fits in 32 bits, and we excluded pow2
     * above so L < 32 isn't guaranteed — for d = 2^31 + 1, L = 32).
     *
     * Strategy: split the numerator. 2^(32+L) = 2^L * 2^32. Long-
     * divide it by d in two steps. The high 32 bits of the quotient
     * come from 2^L div d; the low 32 bits use the remainder.
     *
     * For our typical L (well under 32), the simpler form works:
     *   m = (uint64_t)(2^(32+L) + d - 1) / d
     * where the +d-1 implements ceiling. We use this when 32+L
     * is < 64, which means L < 32. */

    if (L < 32) {
        uint64_t num = ((uint64_t)1 << (32 + L));
        uint64_t m   = (num + d - 1) / d;
        /* If m fits in 32 bits, we're in Case 1: no add-fixup. */
        if (m < ((uint64_t)1 << 32)) {
            r.magic = (uint32_t)m;
            r.shift = L;
        } else {
            /* Case 2: m needs 33 bits. Store m - 2^32. */
            r.magic = (uint32_t)(m - ((uint64_t)1 << 32));
            r.shift = L;
            r.flags |= FDIV_FLAG_ADD;
        }
    } else {
        /* L == 32 case. The numerator 2^64 doesn't fit. Compute
         * ceil(2^64 / d) as: 2^64 / d == (2^64 - 1)/d + (1 if d divides
         * 2^64 else 0). For d not a power of 2, d does not divide
         * 2^64, so the remainder is nonzero and we add 1 to the
         * floor quotient. */
        uint64_t q = (~(uint64_t)0) / d;
        uint64_t rem = (~(uint64_t)0) - q * d;
        if (rem + 1 == d) q++;
        else q++;   /* d doesn't divide 2^64, so always ceiling-bump */

        /* m = q is at least 2^32 here (since 2^64/d > 2^32 when
         * d < 2^32 — true for any 32-bit d). So always Case 2. */
        r.magic = (uint32_t)(q - ((uint64_t)1 << 32));
        r.shift = 32;
        r.flags |= FDIV_FLAG_ADD;
    }
    return r;
}

uint32_t fdiv_u32(uint32_t n, const div_u32_t *d) {
    if (d->flags & FDIV_FLAG_POW2) {
        return n >> d->shift;
    }
    uint32_t t = mulhi_u32(n, d->magic);
    if (d->flags & FDIV_FLAG_ADD) {
        /* Case 2: q = ((n - t) >> 1 + t) >> (L - 1) */
        return (((n - t) >> 1) + t) >> (d->shift - 1);
    }
    /* Case 1: q = mulhi(n, m) >> L */
    return t >> d->shift;
}

fdiv_u32_result_t fdiv_u32_qr(uint32_t n, const div_u32_t *d) {
    fdiv_u32_result_t r;
    r.quot = fdiv_u32(n, d);
    r.rem  = n - r.quot * d->d;
    return r;
}

/* ============================================================
 *  Signed 32-bit
 *
 *  Strategy: prepare on |d|, record sign(d). At execute, divide
 *  |n| by |d| using the unsigned algorithm, then apply the
 *  combined sign.
 *
 *  C-style truncation comes for free because we used absolute
 *  values: |n|/|d| truncates toward zero (same as floor for
 *  non-negative). Sign at the end gives correct truncated result.
 * ============================================================ */

div_s32_t fdiv_s32_prepare(int32_t d) {
    div_s32_t r;
    r.d = d;
    r.magic = 0;
    r.shift = 0;
    r.flags = 0;

    /* Note: INT32_MIN is unsupported because its absolute value
     * doesn't fit in int32_t. The caller is responsible for
     * checking; we don't add a runtime check to avoid the cost. */

    uint32_t abs_d = (d < 0) ? (uint32_t)(-d) : (uint32_t)d;
    if (d < 0) r.flags |= FDIV_FLAG_NEG;

    /* Same prepare math as unsigned, but on abs_d. We borrow the
     * unsigned prepare and copy out the magic and shift. */
    div_u32_t u = fdiv_u32_prepare(abs_d);
    r.magic = u.magic;
    r.shift = u.shift;
    if (u.flags & FDIV_FLAG_POW2) r.flags |= FDIV_FLAG_POW2;
    if (u.flags & FDIV_FLAG_ADD)  r.flags |= FDIV_FLAG_ADD;
    return r;
}

int32_t fdiv_s32(int32_t n, const div_s32_t *d) {
    /* Step 1: compute |q| using the unsigned algorithm on |n|. */
    uint32_t abs_n = (n < 0) ? (uint32_t)(-n) : (uint32_t)n;
    uint32_t q_abs;

    if (d->flags & FDIV_FLAG_POW2) {
        q_abs = abs_n >> d->shift;
    } else {
        uint32_t t = mulhi_u32(abs_n, d->magic);
        if (d->flags & FDIV_FLAG_ADD) {
            q_abs = (((abs_n - t) >> 1) + t) >> (d->shift - 1);
        } else {
            q_abs = t >> d->shift;
        }
    }

    /* Step 2: apply sign. Result negative iff exactly one of n, d
     * is negative. */
    int n_neg = (n < 0);
    int d_neg = (d->flags & FDIV_FLAG_NEG) != 0;
    if (n_neg ^ d_neg) return -(int32_t)q_abs;
    return (int32_t)q_abs;
}

fdiv_s32_result_t fdiv_s32_qr(int32_t n, const div_s32_t *d) {
    fdiv_s32_result_t r;
    r.quot = fdiv_s32(n, d);
    r.rem  = n - r.quot * d->d;
    return r;
}

/* ============================================================
 *  Unsigned 64-bit
 *
 *  Same algorithm as u32, but with 64-bit words throughout. The
 *  "magic" value can be up to 65 bits; we store m - 2^64 in the
 *  Case 2 path. The 128-bit multiplications are simulated via
 *  mulhi_u64.
 * ============================================================ */

div_u64_t fdiv_u64_prepare(uint64_t d) {
    div_u64_t r;
    r.d = d;
    r.magic = 0;
    r.shift = 0;
    r.flags = 0;

    if ((d & (d - 1)) == 0) {
        r.flags = FDIV_FLAG_POW2;
        r.shift = (uint8_t)(63 - clz_u64(d));
        return r;
    }

    /* L = ceil(log2(d)). For non-power-of-2 d, 64 - clz(d-1). */
    uint8_t L = (uint8_t)(64 - clz_u64(d - 1));

    /* Compute m = ceil(2^(64+L) / d).
     *
     * The numerator is a 128-bit value (1 followed by 64+L zero bits)
     * which doesn't fit in any single C integer type. We do bit-by-bit
     * long division: simulate the standard pencil-and-paper algorithm
     * with the numerator implicitly represented as the binary string.
     *
     * State: (q_hi, q_lo) = quotient accumulator (up to 128 bits)
     *        r            = current remainder (always < d, fits in 64 bits)
     *
     * For each bit position from high to low (bits 64+L down to 0):
     *   r = r * 2 + bit
     *   if r >= d: subtract d, shift a 1 into the quotient at this position
     *   else:      shift a 0 into the quotient
     *
     * The numerator has a single 1 bit at position 64+L; all other
     * bits are zero. So we can start with the bit count L+1 and shift
     * through the remaining 64+L positions efficiently. */

    uint64_t q_hi = 0, q_lo = 0;
    uint64_t rem = 0;

    /* We're dividing 2^(64+L) by d. That's 64+L+1 total bit positions.
     * Start at the highest set bit (position 64+L) and work down. */
    int total_bits = 64 + L + 1;
    for (int bit = total_bits - 1; bit >= 0; bit--) {
        /* Shift remainder left by 1, bring in the next dividend bit. */
        uint64_t bit_in = (bit == 64 + L) ? 1 : 0;

        /* rem = rem * 2 + bit_in. Watch for overflow: rem can be up to
         * d-1 < 2^64, so rem*2 may need a 65th bit. We handle this by
         * checking before the multiply. */
        int carry_out = (rem >> 63) & 1;
        rem = (rem << 1) | bit_in;

        /* Now rem (with carry_out as its conceptual 65th bit) is the
         * full new remainder. Reduce by d if needed. */
        int rem_ge_d;
        if (carry_out) {
            /* rem represents 2^64 + (rem as uint64). Definitely >= d. */
            rem_ge_d = 1;
            rem -= d;   /* this is (2^64 + rem) - d, which fits in 64 bits */
        } else {
            rem_ge_d = (rem >= d);
            if (rem_ge_d) rem -= d;
        }

        /* Shift quotient left by 1, append rem_ge_d as the new low bit. */
        q_hi = (q_hi << 1) | (q_lo >> 63);
        q_lo = (q_lo << 1) | (uint64_t)rem_ge_d;
    }

    /* q_hi:q_lo is now floor(2^(64+L) / d). For non-pow2 d, the true
     * ceiling is one more (d doesn't divide 2^(64+L) exactly). */
    if (q_lo == ~(uint64_t)0) {
        q_lo = 0;
        q_hi++;
    } else {
        q_lo++;
    }

    /* Now q_hi:q_lo is m. Case 1: q_hi == 0 means m fits in 64 bits.
     * Case 2: q_hi == 1 means m needs 65 bits; store m - 2^64 = q_lo. */
    if (q_hi == 0) {
        r.magic = q_lo;
        r.shift = L;
    } else {
        r.magic = q_lo;
        r.shift = L;
        r.flags |= FDIV_FLAG_ADD;
    }
    return r;
}

uint64_t fdiv_u64(uint64_t n, const div_u64_t *d) {
    if (d->flags & FDIV_FLAG_POW2) {
        return n >> d->shift;
    }
    uint64_t t = mulhi_u64(n, d->magic);
    if (d->flags & FDIV_FLAG_ADD) {
        return (((n - t) >> 1) + t) >> (d->shift - 1);
    }
    return t >> d->shift;
}

fdiv_u64_result_t fdiv_u64_qr(uint64_t n, const div_u64_t *d) {
    fdiv_u64_result_t r;
    r.quot = fdiv_u64(n, d);
    r.rem  = n - r.quot * d->d;
    return r;
}

/* ============================================================
 *  Signed 64-bit
 *
 *  Same approach as s32: prepare on |d|, divide |n|, then sign-fix.
 * ============================================================ */

div_s64_t fdiv_s64_prepare(int64_t d) {
    div_s64_t r;
    r.d = d;
    r.magic = 0;
    r.shift = 0;
    r.flags = 0;

    /* INT64_MIN unsupported; caller's responsibility. */
    uint64_t abs_d = (d < 0) ? (uint64_t)(-d) : (uint64_t)d;
    if (d < 0) r.flags |= FDIV_FLAG_NEG;

    div_u64_t u = fdiv_u64_prepare(abs_d);
    r.magic = u.magic;
    r.shift = u.shift;
    if (u.flags & FDIV_FLAG_POW2) r.flags |= FDIV_FLAG_POW2;
    if (u.flags & FDIV_FLAG_ADD)  r.flags |= FDIV_FLAG_ADD;
    return r;
}

int64_t fdiv_s64(int64_t n, const div_s64_t *d) {
    uint64_t abs_n = (n < 0) ? (uint64_t)(-n) : (uint64_t)n;
    uint64_t q_abs;

    if (d->flags & FDIV_FLAG_POW2) {
        q_abs = abs_n >> d->shift;
    } else {
        uint64_t t = mulhi_u64(abs_n, d->magic);
        if (d->flags & FDIV_FLAG_ADD) {
            q_abs = (((abs_n - t) >> 1) + t) >> (d->shift - 1);
        } else {
            q_abs = t >> d->shift;
        }
    }

    int n_neg = (n < 0);
    int d_neg = (d->flags & FDIV_FLAG_NEG) != 0;
    if (n_neg ^ d_neg) return -(int64_t)q_abs;
    return (int64_t)q_abs;
}

fdiv_s64_result_t fdiv_s64_qr(int64_t n, const div_s64_t *d) {
    fdiv_s64_result_t r;
    r.quot = fdiv_s64(n, d);
    r.rem  = n - r.quot * d->d;
    return r;
}
