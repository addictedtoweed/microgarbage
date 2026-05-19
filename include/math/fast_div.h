/* ============================================================
 *  fast_div.h — fast integer division by a runtime-known divisor
 *
 *  Replaces hardware divide (or software divide on M0/M0+) with
 *  a precomputed reciprocal and a multiply-shift. The win is
 *  amortized: prepare a divisor once, then divide many things by
 *  it cheaply.
 *
 *  Typical usage:
 *
 *      div_u32_t d = fdiv_u32_prepare(100);
 *      for (...) {
 *          uint32_t q = fdiv_u32(value, &d);
 *      }
 *
 *  Or both quotient and remainder in one operation:
 *
 *      fdiv_u32_result_t qr = fdiv_u32_qr(value, &d);
 *      // qr.quot, qr.rem
 *
 *  Cycle counts (approximate):
 *
 *    Cortex-M0/M0+ (no hardware divide, software UDIV ~150 cycles):
 *      fdiv_u32:              ~9 cycles total — roughly 17x faster
 *      fdiv_u32_qr:          ~12 cycles total — roughly 25x faster than
 *                            calling software UDIV twice
 *
 *    Cortex-M3 (no hardware divide unless the divide extension is
 *    fitted — most M3 parts don't have it): same as M0 above.
 *
 *    Cortex-M4 (hardware UDIV ~12 cycles): marginal speedup
 *      (~1.3x) when called as a function, ~2.4x if inlined via
 *      LTO. The library is still useful here for the qr variant,
 *      but the dramatic wins are on M0/M3.
 *
 *  64-bit division uses 128-bit multiplications internally. On
 *  any 32-bit MCU the absolute cycle count is higher, but the
 *  proportional speedup versus 64-bit software division is still
 *  large — roughly 10-15x on M0/M3.
 *
 *  Semantics: signed division is C99 truncating, with remainder
 *  taking the sign of the dividend.
 *    -7 /  2 == -3,  -7 %  2 == -1
 *     7 / -2 == -3,   7 % -2 ==  1
 *  This matches plain integer / and %, so the fast version is a
 *  drop-in replacement.
 *
 *  Pre-condition: divisor != 0. Behavior is undefined for d == 0,
 *  same as plain integer division. fdiv_s32_prepare(INT32_MIN) is
 *  not supported (the absolute value doesn't fit in int32_t); same
 *  for fdiv_s64_prepare(INT64_MIN).
 *
 *  Depends on: nothing
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef FAST_DIV_H
#define FAST_DIV_H

#include <stdint.h>

/* ============================================================
 *  Prepared-divisor structs
 *
 *  Internal layout is implementation detail except that `d` is
 *  the original divisor (so callers can inspect it if needed).
 *  The other fields encode the algorithm's precomputed state.
 * ============================================================ */

typedef struct {
    uint32_t d;          /* original divisor                     */
    uint32_t magic;      /* precomputed reciprocal-like value    */
    uint8_t  shift;      /* shift amount or log2(d) for pow2     */
    uint8_t  flags;      /* algorithm path selector              */
} div_u32_t;

typedef struct {
    int32_t  d;
    uint32_t magic;
    uint8_t  shift;
    uint8_t  flags;
} div_s32_t;

typedef struct {
    uint64_t d;
    uint64_t magic;
    uint8_t  shift;
    uint8_t  flags;
} div_u64_t;

typedef struct {
    int64_t  d;
    uint64_t magic;
    uint8_t  shift;
    uint8_t  flags;
} div_s64_t;

/* Internal flag bits. The header exposes them so the inline
 * functions can test them, but callers should treat these as
 * opaque. */
#define FDIV_FLAG_POW2   0x01    /* divisor is a power of 2 — use shift directly */
#define FDIV_FLAG_ADD    0x02    /* magic needed W+1 bits — use add-fixup path  */
#define FDIV_FLAG_NEG    0x04    /* signed: original divisor was negative       */

/* Combined quotient+remainder result structs. Layout matches the
 * standard lldiv_t pattern (quotient first). */
typedef struct { uint32_t quot; uint32_t rem; } fdiv_u32_result_t;
typedef struct { int32_t  quot; int32_t  rem; } fdiv_s32_result_t;
typedef struct { uint64_t quot; uint64_t rem; } fdiv_u64_result_t;
typedef struct { int64_t  quot; int64_t  rem; } fdiv_s64_result_t;

/* ============================================================
 *  Prepare functions
 *
 *  Call once per divisor. The returned struct is small and can
 *  live anywhere (stack, global, struct member). Each prepare
 *  call does a few multiplications, a count-leading-zeros, and
 *  some bit operations — typically tens of cycles. Worth
 *  amortizing if you divide more than ~3 times by the same value.
 * ============================================================ */

div_u32_t fdiv_u32_prepare(uint32_t d);
div_s32_t fdiv_s32_prepare(int32_t  d);
div_u64_t fdiv_u64_prepare(uint64_t d);
div_s64_t fdiv_s64_prepare(int64_t  d);

/* ============================================================
 *  Division (quotient only)
 * ============================================================ */

uint32_t fdiv_u32(uint32_t n, const div_u32_t *d);
int32_t  fdiv_s32(int32_t  n, const div_s32_t *d);
uint64_t fdiv_u64(uint64_t n, const div_u64_t *d);
int64_t  fdiv_s64(int64_t  n, const div_s64_t *d);

/* ============================================================
 *  Combined quotient and remainder
 *
 *  Faster than calling fdiv_*() and then computing the remainder
 *  separately. The remainder is computed as n - quot*d, which
 *  uses the divisor stored in the prepared struct.
 * ============================================================ */

fdiv_u32_result_t fdiv_u32_qr(uint32_t n, const div_u32_t *d);
fdiv_s32_result_t fdiv_s32_qr(int32_t  n, const div_s32_t *d);
fdiv_u64_result_t fdiv_u64_qr(uint64_t n, const div_u64_t *d);
fdiv_s64_result_t fdiv_s64_qr(int64_t  n, const div_s64_t *d);

#endif /* FAST_DIV_H */
